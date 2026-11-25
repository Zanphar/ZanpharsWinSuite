// Internet Connection Utility
// Copyright (c) 2026-2027 Charles McDonald. BSD 3-Clause License.
// Part of Zanphar's WinSuite; see LICENSE.md in the suite root.

#include "net.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <winternl.h>
#include <shellapi.h>

#pragma comment(lib, "iphlpapi.lib")

namespace net {

// ------------------------------------------------------------------ basics
static std::once_flag g_wsaOnce;
static bool g_wsaOk = false;

bool init() {
    std::call_once(g_wsaOnce, []() {
        WSADATA d{};
        g_wsaOk = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
    });
    return g_wsaOk;
}

double nowMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    if (freq.QuadPart == 0) return 0.0;
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

std::string wstr2utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring utf82wstr(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring fmt1(double v) {
    wchar_t b[64];
    swprintf(b, 64, L"%.1f", v);
    return b;
}

std::wstring fmt0(double v) {
    wchar_t b[64];
    swprintf(b, 64, L"%.0f", v);
    return b;
}

bool isIpLiteral(const std::string& s) {
    IN_ADDR a{};
    return InetPtonA(AF_INET, s.c_str(), &a) == 1;
}

// ------------------------------------------------------------------ ping
static bool resolveV4(const std::string& host, IPAddr& out) {
    if (InetPtonA(AF_INET, host.c_str(), &out) == 1) return true;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out = ((sockaddr_in*)res->ai_addr)->sin_addr.S_un.S_addr;
    freeaddrinfo(res);
    return true;
}

bool pingOnce(const std::string& host, int timeoutMs, double& rttOut) {
    if (!init()) return false;
    IPAddr addr{};
    if (!resolveV4(host, addr)) return false;

    HANDLE h = IcmpCreateFile();
    if (h == INVALID_HANDLE_VALUE) return false;

    unsigned char payload[32];
    memset(payload, 0x4E, sizeof(payload));
    unsigned char reply[sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 16];

    DWORD n = IcmpSendEcho(h, addr, payload, (WORD)sizeof(payload), nullptr, reply,
                           (DWORD)sizeof(reply), (DWORD)timeoutMs);
    bool ok = false;
    if (n > 0) {
        auto* p = (ICMP_ECHO_REPLY*)reply;
        if (p->Status == IP_SUCCESS) {
            ok = true;
            rttOut = (double)p->RoundTripTime;
        }
    }
    IcmpCloseHandle(h);
    return ok;
}

PingStats pingSeries(const std::string& host, int count, int intervalMs, int timeoutMs) {
    PingStats s;
    double sum = 0, sqSum = 0;
    for (int i = 0; i < count; ++i) {
        double rtt = 0;
        bool ok = pingOnce(host, timeoutMs, rtt);
        s.sent++;
        if (ok) {
            s.recv++;
            s.samples.push_back(rtt);
            sum += rtt;
            sqSum += rtt * rtt;
            if (s.minMs == 0 || rtt < s.minMs) s.minMs = rtt;
            if (rtt > s.maxMs) s.maxMs = rtt;
        } else {
            s.samples.push_back(0.0);
        }
        if (i + 1 < count) Sleep(intervalMs);
    }
    if (s.recv > 0) {
        s.avgMs = sum / s.recv;
        double var = (sqSum / s.recv) - (s.avgMs * s.avgMs);
        s.jitterMs = var > 0 ? sqrt(var) : 0.0;
    }
    s.reachable = s.recv > 0;
    s.lossPct = s.sent ? (int)((s.sent - s.recv) * 100.0 / s.sent + 0.5) : 100;
    return s;
}

// ------------------------------------------------------------------ dns
static std::vector<unsigned char> buildQuery(const char* name, unsigned short id,
                                             std::vector<std::string>& labels) {
    std::vector<unsigned char> q;
    q.push_back((unsigned char)(id >> 8));
    q.push_back((unsigned char)(id & 0xFF));
    q.push_back(0x01); // recursion desired
    q.push_back(0x00);
    q.push_back(0x00); q.push_back(0x01); // qdcount
    q.push_back(0x00); q.push_back(0x00);
    q.push_back(0x00); q.push_back(0x00);
    q.push_back(0x00); q.push_back(0x00);

    std::string cur;
    for (const char* p = name; ; ++p) {
        if (*p == '.' || *p == 0) {
            if (!cur.empty()) {
                q.push_back((unsigned char)cur.size());
                q.insert(q.end(), cur.begin(), cur.end());
                labels.push_back(cur);
                cur.clear();
            }
            if (*p == 0) break;
        } else {
            cur.push_back(*p);
        }
    }
    q.push_back(0x00);
    q.push_back(0x00); q.push_back(0x01); // A
    q.push_back(0x00); q.push_back(0x01); // IN
    return q;
}

DnsProbe probeDns(const std::string& server, const char* qname, int timeoutMs) {
    DnsProbe r;
    r.server = server;
    if (!init()) return r;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return r;

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    if (InetPtonA(AF_INET, server.c_str(), &to.sin_addr) != 1) {
        closesocket(s);
        return r;
    }

    unsigned short id = (unsigned short)(GetTickCount() ^ (GetCurrentThreadId() << 3));
    std::vector<std::string> labels;
    auto q = buildQuery(qname, id, labels);

    double t0 = nowMs();
    if (sendto(s, (const char*)q.data(), (int)q.size(), 0, (sockaddr*)&to, sizeof(to)) ==
        SOCKET_ERROR) {
        closesocket(s);
        return r;
    }

    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int sel = select(0, &rd, nullptr, nullptr, &tv);
    if (sel > 0) {
        unsigned char buf[1024];
        int n = recv(s, (char*)buf, sizeof(buf), 0);
        double elapsed = nowMs() - t0;
        if (n >= 12) {
            unsigned short rid = (unsigned short)((buf[0] << 8) | buf[1]);
            int rcode = buf[3] & 0x0F;
            bool qr = (buf[2] & 0x80) != 0;
            if (rid == id && qr) {
                r.responded = true;
                r.rcode = rcode;
                r.ms = elapsed;
            }
        }
    }
    closesocket(s);
    return r;
}

ResolveRes resolve(const char* name, int timeoutMs) {
    ResolveRes r;
    if (!init()) {
        r.err = -1;
        return r;
    }
    struct Shared {
        std::mutex m;
        bool done = false;
        bool ok = false;
        int rc = 0;
        std::vector<std::string> v4;
    };
    auto sh = std::make_shared<Shared>();
    std::string qname(name);

    std::thread([sh, qname]() {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        int rc = getaddrinfo(qname.c_str(), nullptr, &hints, &res);
        std::vector<std::string> v4;
        if (rc == 0) {
            for (addrinfo* p = res; p; p = p->ai_next) {
                char buf[INET_ADDRSTRLEN] = {};
                auto* sa = (sockaddr_in*)p->ai_addr;
                if (InetNtopA(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
                    v4.push_back(buf);
                }
            }
            freeaddrinfo(res);
        }
        std::lock_guard<std::mutex> lk(sh->m);
        sh->done = true;
        sh->ok = (rc == 0 && !v4.empty());
        sh->rc = rc;
        sh->v4 = std::move(v4);
    }).detach();

    double t0 = nowMs();
    while (nowMs() - t0 < timeoutMs) {
        {
            std::lock_guard<std::mutex> lk(sh->m);
            if (sh->done) {
                r.ok = sh->ok;
                r.err = sh->rc;
                r.v4 = sh->v4;
                r.ms = nowMs() - t0;
                return r;
            }
        }
        Sleep(8);
    }
    r.ms = timeoutMs;
    r.err = 110; // timed out
    return r;
}

// ------------------------------------------------------------------ http
static HttpRes httpCommon(const std::wstring& host, const std::wstring& path, bool secure,
                          int timeoutMs, int maxBody, bool download) {
    HttpRes r;
    HINTERNET hs = WinHttpOpen(L"InternetConnectionUtility/1.0 (Windows)", WINHTTP_ACCESS_TYPE_NO_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) {
        r.err = "WinHttpOpen failed";
        return r;
    }
    WinHttpSetTimeouts(hs, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    DWORD prot = WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH;
    HINTERNET hc = WinHttpConnect(hs, host.c_str(), secure ? 443 : 80, 0);
    if (!hc) {
        r.err = "connect failed";
        WinHttpCloseHandle(hs);
        return r;
    }
    HINTERNET hr = WinHttpOpenRequest(hc, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? prot : 0);
    if (!hr) {
        r.err = "request failed";
        WinHttpCloseHandle(hc);
        WinHttpCloseHandle(hs);
        return r;
    }

    double t0 = nowMs();
    BOOL sent = WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(hr, nullptr)) {
        DWORD e = GetLastError();
        char b[128];
        sprintf(b, "http error %lu", (unsigned long)e);
        r.err = b;
        WinHttpCloseHandle(hr);
        WinHttpCloseHandle(hc);
        WinHttpCloseHandle(hs);
        return r;
    }
    double tFirst = nowMs() - t0;

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)status;

    std::string body;
    int cap = maxBody; // httpDownload passes its byte budget here
    DWORD avail = 0;
    long long total = 0;
    int guard = 0;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hr, &avail)) break;
        if (avail == 0) break;
        std::vector<char> buf(std::min<DWORD>(avail, 65536));
        DWORD got = 0;
        if (!WinHttpReadData(hr, buf.data(), (DWORD)buf.size(), &got) || got == 0) break;
        total += got;
        if (!download && (int)body.size() < maxBody) {
            int take = std::min<int>((int)got, maxBody - (int)body.size());
            body.append(buf.data(), take);
        }
        if (download && total >= cap) break;
        if (!download && (int)body.size() >= cap) break;
    } while (++guard < 4096);

    r.bytes = total;
    r.ms = nowMs() - t0;
    r.body = body;
    r.ok = (r.status > 0 && r.status < 500);
    if (download && r.ms > 0.5 && total > 0)
        r.mbps = ((double)total * 8.0) / 1.0e6 / (r.ms / 1000.0);

    (void)tFirst;
    WinHttpCloseHandle(hr);
    WinHttpCloseHandle(hc);
    WinHttpCloseHandle(hs);
    return r;
}

HttpRes httpGet(const std::wstring& host, const std::wstring& path, bool secure,
                int timeoutMs, int maxBody) {
    return httpCommon(host, path, secure, timeoutMs, maxBody, false);
}

HttpRes httpDownload(const std::wstring& host, const std::wstring& path, bool secure,
                     int timeoutMs, int maxBytes) {
    return httpCommon(host, path, secure, timeoutMs, maxBytes, true);
}

// ------------------------------------------------------------------ tcp
TcpRes tcpConnect(const char* host, int port, int timeoutMs) {
    TcpRes r;
    if (!init()) return r;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return r;

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);
    if (InetPtonA(AF_INET, host, &to.sin_addr) != 1) {
        closesocket(s);
        r.err = "bad addr";
        return r;
    }

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    double t0 = nowMs();
    int rc = connect(s, (sockaddr*)&to, sizeof(to));
    if (rc == 0) {
        r.ok = true;
        r.ms = nowMs() - t0;
        closesocket(s);
        return r;
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
        r.err = "connect failed";
        closesocket(s);
        return r;
    }
    fd_set wr;
    FD_ZERO(&wr);
    FD_SET(s, &wr);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int sel = select(0, nullptr, &wr, nullptr, &tv);
    if (sel > 0) {
        int soErr = 0;
        int sl = sizeof(soErr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soErr, &sl);
        if (soErr == 0) {
            r.ok = true;
            r.ms = nowMs() - t0;
        } else {
            char b[64];
            sprintf(b, "socket error %d", soErr);
            r.err = b;
        }
    } else {
        r.err = "timeout";
    }
    closesocket(s);
    return r;
}

// ------------------------------------------------------------------ adapter
static std::wstring addrToW(const sockaddr* sa) {
    if (!sa) return {};
    char buf[INET6_ADDRSTRLEN] = {};
    if (sa->sa_family == AF_INET) {
        auto* v4 = (sockaddr_in*)sa;
        InetNtopA(AF_INET, &v4->sin_addr, buf, sizeof(buf));
        return utf82wstr(buf);
    }
    if (sa->sa_family == AF_INET6) {
        auto* v6 = (sockaddr_in6*)sa;
        InetNtopA(AF_INET6, &v6->sin6_addr, buf, sizeof(buf));
        return utf82wstr(buf);
    }
    return {};
}

static std::string prefixToMask(unsigned long prefix) {
    unsigned long mask = prefix == 0 ? 0 : (0xFFFFFFFFUL << (32 - prefix));
    char b[32];
    sprintf(b, "%lu.%lu.%lu.%lu", (mask >> 24) & 0xFF, (mask >> 16) & 0xFF, (mask >> 8) & 0xFF,
            mask & 0xFF);
    return b;
}

static bool looksVirtual(const std::wstring& s) {
    static const wchar_t* keys[] = {L"virtual", L"vmware", L"hyper-v", L"vethernet", L"loopback",
                                    L"tap-",     L"tap ",   L"vpn",      L"wireguard", L"tailscale",
                                    L"zerotier", L"wan miniport", L"bluetooth", L"npcap", L"pseudo"};
    std::wstring low = s;
    std::transform(low.begin(), low.end(), low.begin(), towlower);
    for (auto k : keys)
        if (low.find(k) != std::wstring::npos) return true;
    return false;
}

std::wstring nicClassKeyForGuid(const std::wstring& guid) {
    static const wchar_t* base =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}";
    std::wstring lowGuid = guid;
    std::transform(lowGuid.begin(), lowGuid.end(), lowGuid.begin(), towlower);

    for (int i = 0; i < 32; ++i) {
        wchar_t sub[8];
        swprintf(sub, 8, L"%04d", i);
        std::wstring path = std::wstring(base) + L"\\" + sub;
        std::wstring id;
        if (!regGetStr(HKEY_LOCAL_MACHINE, path, L"NetCfgInstanceId", id)) continue;
        std::transform(id.begin(), id.end(), id.begin(), towlower);
        if (id == lowGuid) return path;
    }
    return {};
}

static bool adapterDriverInfo(const std::wstring& guid, std::wstring& provider,
                              std::wstring& date, std::wstring& version) {
    std::wstring key = nicClassKeyForGuid(guid);
    if (key.empty()) return false;
    regGetStr(HKEY_LOCAL_MACHINE, key, L"ProviderName", provider);
    regGetStr(HKEY_LOCAL_MACHINE, key, L"DriverDate", date);
    regGetStr(HKEY_LOCAL_MACHINE, key, L"DriverVersion", version);
    return true;
}

bool detectAdapter(Adapter& a) {
    a = Adapter{};
    if (!init()) return false;

    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(),
                                    &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(),
                                  &size);
    }
    if (rc != NO_ERROR) return false;

    IP_ADAPTER_ADDRESSES* best = nullptr;
    int bestScore = -1;
    for (auto* p = (IP_ADAPTER_ADDRESSES*)buf.data(); p; p = p->Next) {
        if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        bool up = p->OperStatus == IfOperStatusUp;
        bool virt = looksVirtual(p->FriendlyName) || looksVirtual(p->Description) ||
                    p->IfType == IF_TYPE_TUNNEL;
        bool hasGw = p->FirstGatewayAddress != nullptr;
        std::string ip;
        for (auto* u = p->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family == AF_INET) {
                std::string cand = wstr2utf8(addrToW(u->Address.lpSockaddr));
                if (ip.empty() || ip.rfind("169.254.", 0) == 0) ip = cand;
            }
        }
        int score = 0;
        if (up) score += 8;
        if (hasGw) score += 4;
        if (!virt) score += 2;
        if (!ip.empty() && ip.rfind("169.254.", 0) != 0) score += 2;
        if (ip.empty()) score -= 5;
        if (score > bestScore) {
            bestScore = score;
            best = p;
        }
    }
    if (!best) return false;

    a.valid = true;
    a.name = best->FriendlyName;
    a.desc = best->Description;
    a.ifType = (int)best->IfType;
    a.up = best->OperStatus == IfOperStatusUp;
    a.isWifi = best->IfType == IF_TYPE_IEEE80211;
    a.isVirtual = looksVirtual(best->FriendlyName) || looksVirtual(best->Description);
    a.ifIndex = best->IfIndex;

    if (best->AdapterName) a.guid = utf82wstr(best->AdapterName);

    if (best->PhysicalAddressLength > 0) {
        wchar_t mac[64] = {};
        int off = 0;
        for (ULONG i = 0; i < best->PhysicalAddressLength && off < 60; ++i)
            off += swprintf(mac + off, 64 - off, i ? L":%02X" : L"%02X", best->PhysicalAddress[i]);
        a.mac = mac;
    }
    a.speedBps = best->TransmitLinkSpeed;

    unsigned long prefix = 24;
    for (auto* u = best->FirstUnicastAddress; u; u = u->Next) {
        if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
        std::string cand = wstr2utf8(addrToW(u->Address.lpSockaddr));
        if (a.ipv4.empty() || a.ipv4.rfind("169.254.", 0) == 0) {
            a.ipv4 = cand;
            prefix = u->OnLinkPrefixLength > 32 ? 24 : u->OnLinkPrefixLength;
        }
    }
    a.mask = prefixToMask(prefix);
    a.apipa = a.ipv4.rfind("169.254.", 0) == 0;

    if (best->FirstGatewayAddress) {
        std::wstring gw = addrToW(best->FirstGatewayAddress->Address.lpSockaddr);
        a.gateway = wstr2utf8(gw);
        if (a.gateway == "0.0.0.0" || a.gateway == "::") a.gateway.clear();
    }
    int dnsCount = 0;
    for (auto* d = best->FirstDnsServerAddress; d && dnsCount < 6; d = d->Next) {
        std::wstring w = addrToW(d->Address.lpSockaddr);
        std::string s = wstr2utf8(w);
        if (!s.empty() && a.dns.size() < 4) {
            a.dns.push_back(s);
            dnsCount++;
        }
    }

    // DHCP state + lease
    ULONG aiSize = 32 * 1024;
    std::vector<unsigned char> aiBuf(aiSize);
    ULONG aiRc = GetAdaptersInfo((IP_ADAPTER_INFO*)aiBuf.data(), &aiSize);
    if (aiRc == ERROR_BUFFER_OVERFLOW) {
        aiBuf.resize(aiSize);
        aiRc = GetAdaptersInfo((IP_ADAPTER_INFO*)aiBuf.data(), &aiSize);
    }
    if (aiRc == NO_ERROR) {
        for (auto* p = (IP_ADAPTER_INFO*)aiBuf.data(); p; p = p->Next) {
            if (p->Index != best->IfIndex) continue;
            a.dhcp = p->DhcpEnabled != 0;
            if (a.dhcp) {
                time_t now = time(nullptr);
                a.leaseHoursLeft = (double)((long long)p->LeaseExpires - (long long)now) / 3600.0;
            }
            break;
        }
    }

    MIB_IPINTERFACE_ROW row{};
    row.Family = AF_INET;
    row.InterfaceIndex = best->IfIndex;
    if (GetIpInterfaceEntry(&row) == NO_ERROR) {
        a.metric = row.Metric;
        a.mtu = row.NlMtu;
        a.autoMetric = row.UseAutomaticMetric != 0;
    }
    if (a.mtu == 0) a.mtu = 1500;

    std::wstring ver;
    adapterDriverInfo(a.guid, a.driverProvider, a.driverDate, ver);
    if (!ver.empty()) a.driverDate += L"  v" + ver;
    return true;
}

bool interfaceStats(ULONG ifIndex, uint64_t& inErr, uint64_t& outErr, uint64_t& inDisc,
                    uint64_t& outDisc) {
    MIB_IF_TABLE2* tbl = nullptr;
    if (GetIfTable2(&tbl) != NO_ERROR || !tbl) return false;
    bool found = false;
    for (ULONG i = 0; i < tbl->NumEntries; ++i) {
        auto& r = tbl->Table[i];
        if (r.InterfaceIndex != ifIndex) continue;
        inErr = r.InErrors;
        outErr = r.OutErrors;
        inDisc = r.InDiscards;
        outDisc = r.OutDiscards;
        found = true;
        break;
    }
    FreeMibTable(tbl);
    return found;
}

int defaultRouteCount() {
    MIB_IPFORWARD_TABLE2* tbl = nullptr;
    if (GetIpForwardTable2(AF_INET, &tbl) != NO_ERROR || !tbl) return -1;
    int n = 0;
    for (ULONG i = 0; i < tbl->NumEntries; ++i) {
        auto& r = tbl->Table[i];
        if (r.DestinationPrefix.PrefixLength == 0 && r.NextHop.si_family == AF_INET &&
            *(uint32_t*)&r.NextHop.Ipv4.sin_addr != 0)
            n++;
    }
    FreeMibTable(tbl);
    return n;
}

std::vector<std::string> duplicateIps() {
    std::vector<std::string> out;
    if (!init()) return out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                    nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                  nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    }
    if (rc != NO_ERROR) return out;

    std::map<std::string, std::set<ULONG>> seen;
    for (auto* p = (IP_ADAPTER_ADDRESSES*)buf.data(); p; p = p->Next) {
        if (p->OperStatus != IfOperStatusUp) continue;
        if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = p->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            std::string ip = wstr2utf8(addrToW(u->Address.lpSockaddr));
            if (ip.rfind("169.254.", 0) == 0 || ip == "127.0.0.1" || ip.empty()) continue;
            seen[ip].insert(p->IfIndex);
        }
    }
    for (auto& kv : seen)
        if (kv.second.size() > 1) out.push_back(kv.first);
    return out;
}

// ------------------------------------------------------------------ wifi
void channelInfo(double centerMhz, int& channel, bool& is5, bool& is6) {
    is5 = is6 = false;
    if (centerMhz >= 2400 && centerMhz < 2500) {
        if (centerMhz == 2484) channel = 14;
        else channel = (int)((centerMhz - 2407) / 5.0 + 0.5);
    } else if (centerMhz >= 4900 && centerMhz < 5900) {
        channel = (int)((centerMhz - 5000) / 5.0 + 0.5);
        is5 = true;
    } else if (centerMhz >= 5925 && centerMhz < 7125) {
        channel = (int)((centerMhz - 5950) / 5.0 + 0.5);
        is6 = true;
    } else {
        channel = centerMhz > 0 ? (int)(centerMhz / 5.0) : -1;
    }
}

static std::wstring ssidW(const DOT11_SSID& s) {
    int len = s.uSSIDLength > 32 ? 32 : (int)s.uSSIDLength;
    std::string raw((const char*)s.ucSSID, len);
    return utf82wstr(raw);
}

static std::wstring bssidW(const DOT11_MAC_ADDRESS& m) {
    wchar_t b[32];
    swprintf(b, 32, L"%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

static const wchar_t* phyName(DOT11_PHY_TYPE t) {
    switch (t) {
        case dot11_phy_type_fhss: return L"FHSS";
        case dot11_phy_type_dsss: return L"DSSS (legacy)";
        case dot11_phy_type_irbaseband: return L"IR";
        case dot11_phy_type_ofdm: return L"802.11a";
        case dot11_phy_type_hrdsss: return L"802.11b";
        case dot11_phy_type_erp: return L"802.11g";
        case dot11_phy_type_ht: return L"802.11n";
        case dot11_phy_type_vht: return L"802.11ac";
        case dot11_phy_type_dmg: return L"802.11ad";
        case dot11_phy_type_he: return L"802.11ax (Wi-Fi 6)";
        default: return L"unknown";
    }
}

static const wchar_t* authName(DOT11_AUTH_ALGORITHM a) {
    switch (a) {
        case DOT11_AUTH_ALGO_80211_OPEN: return L"Open";
        case DOT11_AUTH_ALGO_80211_SHARED_KEY: return L"WEP shared key";
        case DOT11_AUTH_ALGO_WPA: return L"WPA";
        case DOT11_AUTH_ALGO_WPA_PSK: return L"WPA-PSK";
        case DOT11_AUTH_ALGO_WPA_NONE: return L"WPA-None";
        case DOT11_AUTH_ALGO_RSNA: return L"WPA2-Enterprise";
        case DOT11_AUTH_ALGO_RSNA_PSK: return L"WPA2-Personal";
        default: return L"WPA3 / modern";
    }
}

static const wchar_t* cipherName(DOT11_CIPHER_ALGORITHM c) {
    switch (c) {
        case DOT11_CIPHER_ALGO_NONE: return L"None";
        case DOT11_CIPHER_ALGO_WEP40: return L"WEP-40";
        case DOT11_CIPHER_ALGO_TKIP: return L"TKIP";
        case DOT11_CIPHER_ALGO_CCMP: return L"AES-CCMP";
        case DOT11_CIPHER_ALGO_WEP104: return L"WEP-104";
        case DOT11_CIPHER_ALGO_GCMP: return L"AES-GCMP";
        default: return L"unknown";
    }
}

static HANDLE openWlan(DWORD& negVersion) {
    HANDLE h = nullptr;
    DWORD v = 0;
    if (WlanOpenHandle(2, nullptr, &v, &h) != ERROR_SUCCESS) return nullptr;
    negVersion = v;
    return h;
}

bool wifiCurrent(Wifi& w) {
    w = Wifi{};
    DWORD ver = 0;
    HANDLE h = openWlan(ver);
    if (!h) return false;
    w.supported = true;

    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(h, nullptr, &list) != ERROR_SUCCESS || !list) {
        WlanCloseHandle(h, nullptr);
        return false;
    }
    bool anyRadioOff = false, connected = false;

    for (ULONG i = 0; i < list->dwNumberOfItems; ++i) {
        auto& ifc = list->InterfaceInfo[i];
        if (ifc.isState == wlan_interface_state_connected && !connected) {
            PWLAN_CONNECTION_ATTRIBUTES conn = nullptr;
            DWORD sz = 0;
            WLAN_OPCODE_VALUE_TYPE vt;
            if (WlanQueryInterface(h, &ifc.InterfaceGuid, wlan_intf_opcode_current_connection,
                                   nullptr, &sz, (PVOID*)&conn, &vt) == ERROR_SUCCESS &&
                conn) {
                w.connected = true;
                w.ssid = ssidW(conn->wlanAssociationAttributes.dot11Ssid);
                w.bssid = bssidW(conn->wlanAssociationAttributes.dot11Bssid);
                w.signalPct = (int)conn->wlanAssociationAttributes.wlanSignalQuality;
                w.rxKbps = conn->wlanAssociationAttributes.ulRxRate;
                w.txKbps = conn->wlanAssociationAttributes.ulTxRate;
                w.phy = phyName(conn->wlanAssociationAttributes.dot11PhyType);
                w.auth = authName(conn->wlanSecurityAttributes.dot11AuthAlgorithm);
                w.cipher = cipherName(conn->wlanSecurityAttributes.dot11CipherAlgorithm);
                w.profile = conn->strProfileName;
                connected = true;

                PWLAN_BSS_LIST bss = nullptr;
                if (WlanGetNetworkBssList(h, &ifc.InterfaceGuid, nullptr, dot11_BSS_type_any,
                                          FALSE, nullptr, &bss) == ERROR_SUCCESS &&
                    bss) {
                    for (ULONG k = 0; k < bss->dwNumberOfItems; ++k) {
                        auto& e = bss->wlanBssEntries[k];
                        if (bssidW(e.dot11Bssid) == w.bssid) {
                            w.rssi = (int)e.lRssi;
                            double mhz = e.ulChCenterFrequency / 1000.0;
                            channelInfo(mhz, w.channel, w.band5, w.band6);
                            break;
                        }
                    }
                    WlanFreeMemory(bss);
                }
                WlanFreeMemory(conn);
            }
        } else if (ifc.isState == wlan_interface_state_disconnected) {
            if (w.stateText.empty()) w.stateText = L"Disconnected";
        } else if (ifc.isState == wlan_interface_state_authenticating) {
            w.stateText = L"Authenticating";
        } else if (ifc.isState == wlan_interface_state_associating) {
            w.stateText = L"Associating";
        } else if (ifc.isState == wlan_interface_state_disconnecting) {
            w.stateText = L"Disconnecting";
        }

        PWLAN_RADIO_STATE rs = nullptr;
        DWORD rsz = 0;
        WLAN_OPCODE_VALUE_TYPE rvt;
        if (WlanQueryInterface(h, &ifc.InterfaceGuid, wlan_intf_opcode_radio_state, nullptr, &rsz,
                               (PVOID*)&rs, &rvt) == ERROR_SUCCESS &&
            rs) {
            for (DWORD p = 0; p < rs->dwNumberOfPhys; ++p) {
                if (rs->PhyRadioState[p].dot11SoftwareRadioState == dot11_radio_state_off ||
                    rs->PhyRadioState[p].dot11HardwareRadioState == dot11_radio_state_off)
                    anyRadioOff = true;
            }
            WlanFreeMemory(rs);
        }
    }
    if (w.connected) w.radioOn = !anyRadioOff;
    else w.radioOn = !anyRadioOff;

    WlanFreeMemory(list);
    WlanCloseHandle(h, nullptr);

    if (w.connected && w.signalPct >= 0) {
        // Approximate RSSI from quality when the BSS list was unavailable.
        if (w.rssi == -999) w.rssi = -100 + (w.signalPct * 60) / 100;
    }
    return w.supported;
}

bool wifiNeighbors(std::vector<WifiNeighbor>& out) {
    out.clear();
    DWORD ver = 0;
    HANDLE h = openWlan(ver);
    if (!h) return false;

    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(h, nullptr, &list) != ERROR_SUCCESS || !list) {
        WlanCloseHandle(h, nullptr);
        return false;
    }
    if (list->dwNumberOfItems == 0) {
        WlanFreeMemory(list);
        WlanCloseHandle(h, nullptr);
        return false;
    }
    GUID g = list->InterfaceInfo[0].InterfaceGuid;
    WlanFreeMemory(list);

    PWLAN_BSS_LIST bss = nullptr;
    if (WlanGetNetworkBssList(h, &g, nullptr, dot11_BSS_type_any, FALSE, nullptr, &bss) !=
        ERROR_SUCCESS ||
        !bss) {
        WlanCloseHandle(h, nullptr);
        return false;
    }

    std::set<std::wstring> seen;
    for (ULONG i = 0; i < bss->dwNumberOfItems; ++i) {
        auto& e = bss->wlanBssEntries[i];
        std::wstring key = bssidW(e.dot11Bssid);
        if (!seen.insert(key).second) continue;
        WifiNeighbor n;
        n.ssid = ssidW(e.dot11Ssid);
        n.rssi = (int)e.lRssi;
        double mhz = e.ulChCenterFrequency / 1000.0;
        channelInfo(mhz, n.channel, n.band5, n.band6);
        n.quality = (int)e.uLinkQuality;
        out.push_back(n);
    }
    WlanFreeMemory(bss);
    WlanCloseHandle(h, nullptr);

    std::sort(out.begin(), out.end(), [](const WifiNeighbor& a, const WifiNeighbor& b) {
        return a.rssi > b.rssi;
    });
    if (out.size() > 24) out.resize(24);
    return true;
}

// ------------------------------------------------------------------ services
SvcState serviceState(const wchar_t* name) {
    SvcState s;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return s;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!svc) {
        CloseServiceHandle(scm);
        return s;
    }
    s.exists = true;
    SERVICE_STATUS st{};
    if (QueryServiceStatus(svc, &st)) s.state = st.dwCurrentState;
    DWORD need = 0;
    LPQUERY_SERVICE_CONFIGW cfg = nullptr;
    if (!QueryServiceConfigW(svc, nullptr, 0, &need) && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        cfg = (LPQUERY_SERVICE_CONFIGW)malloc(need);
        if (cfg && QueryServiceConfigW(svc, cfg, need, &need)) s.startType = cfg->dwStartType;
        if (cfg) free(cfg);
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return s;
}

bool serviceAuto(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_CHANGE_CONFIG);
    bool ok = false;
    if (svc) {
        ok = ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
                                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != 0;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool serviceStart(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_START | SERVICE_QUERY_STATUS);
    bool ok = false;
    if (svc) {
        if (StartServiceW(svc, 0, nullptr)) ok = true;
        else if (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) ok = true;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

// ------------------------------------------------------------------ registry
bool regGetDword(HKEY root, const std::wstring& path, const wchar_t* name, DWORD& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, sz = sizeof(DWORD), val = 0;
    bool ok = RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)&val, &sz) == ERROR_SUCCESS &&
              type == REG_DWORD;
    if (ok) out = val;
    RegCloseKey(k);
    return ok;
}

bool regGetStr(HKEY root, const std::wstring& path, const wchar_t* name, std::wstring& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[1024] = {};
    DWORD sz = sizeof(buf), type = 0;
    bool ok = RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS &&
              (type == REG_SZ || type == REG_EXPAND_SZ);
    if (ok) out = buf;
    RegCloseKey(k);
    return ok;
}

bool regSetDword(HKEY root, const std::wstring& path, const wchar_t* name, DWORD v) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    bool ok = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

bool regDeleteValue(HKEY root, const std::wstring& path, const wchar_t* name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    bool ok = RegDeleteValueW(k, name) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

bool regDeleteKeyTree(HKEY root, const std::wstring& path) {
    std::wstring p = path;
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, p.c_str(), 0, KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    LONG rc = RegDeleteTreeW(root, p.c_str());
    return rc == ERROR_SUCCESS;
}

// ------------------------------------------------------------------ process
static std::string oemToUtf8(const std::string& s) {
    if (s.empty()) return s;
    int n = MultiByteToWideChar(CP_OEMCP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_OEMCP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return wstr2utf8(w);
}

CmdRes run(const std::wstring& cmdline, int timeoutMs) {
    CmdRes r;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return r;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    // A GUI process has no console handles, so give the child a valid NUL stdin.
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nul;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::wstring cmd = cmdline;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW |
                                                                          CREATE_UNICODE_ENVIRONMENT,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return r;
    }
    r.started = true;

    std::string acc;
    double t0 = nowMs();
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char tmp[4096];
            DWORD got = 0;
            if (ReadFile(rd, tmp, std::min<DWORD>(avail, sizeof(tmp)), &got, nullptr) && got)
                acc.append(tmp, got);
        } else {
            if (WaitForSingleObject(pi.hProcess, 25) == WAIT_OBJECT_0) {
                // drain
                for (;;) {
                    char tmp[4096];
                    DWORD got = 0;
                    if (!ReadFile(rd, tmp, sizeof(tmp), &got, nullptr) || got == 0) break;
                    acc.append(tmp, got);
                }
                break;
            }
            if (nowMs() - t0 > timeoutMs) {
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.code = (int)code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    r.out = oemToUtf8(acc);
    return r;
}

bool elevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD sz = sizeof(el);
    bool ok = GetTokenInformation(tok, TokenElevation, &el, sz, &sz) && el.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

bool relaunchAsAdmin() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    HINSTANCE inst = ShellExecuteW(nullptr, L"runas", path, nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)inst > 32;
}

std::wstring activeAdapterRegKey() {
    net::Adapter a;
    if (!detectAdapter(a) || a.guid.empty()) return {};
    return nicClassKeyForGuid(a.guid);
}

// ------------------------------------------------------------------ misc
ProxyInfo proxyInfo() {
    ProxyInfo p;
    std::wstring base = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    DWORD en = 0;
    if (regGetDword(HKEY_CURRENT_USER, base, L"ProxyEnable", en)) p.enabled = en != 0;
    regGetStr(HKEY_CURRENT_USER, base, L"ProxyServer", p.server);
    regGetStr(HKEY_CURRENT_USER, base, L"AutoConfigURL", p.pac);
    if (!p.pac.empty()) p.enabled = true;
    return p;
}

bool disableUserProxy() {
    std::wstring base = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    bool ok = regSetDword(HKEY_CURRENT_USER, base, L"ProxyEnable", 0);
    regDeleteValue(HKEY_CURRENT_USER, base, L"AutoConfigURL");

    // Refresh WinINet settings without pulling in wininet.h (it collides with winhttp.h).
    HMODULE inet = LoadLibraryW(L"wininet.dll");
    if (inet) {
        typedef BOOL(WINAPI * PfnSetOption)(LPVOID, DWORD, LPVOID, DWORD);
        auto fn = (PfnSetOption)GetProcAddress(inet, "InternetSetOptionW");
        if (fn) {
            const DWORD kSettingsChanged = 39;
            const DWORD kRefresh = 37;
            fn(nullptr, kSettingsChanged, nullptr, 0);
            fn(nullptr, kRefresh, nullptr, 0);
        }
        FreeLibrary(inet);
    }
    return ok;
}

uint64_t tcpRetrans(uint64_t& outSegs) {
    MIB_TCPSTATS st{};
    if (GetTcpStatisticsEx(&st, AF_INET) != NO_ERROR) {
        outSegs = 0;
        return 0;
    }
    outSegs = st.dwOutSegs;
    return st.dwRetransSegs;
}

std::wstring hostsFile() {
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    return std::wstring(win) + L"\\System32\\drivers\\etc\\hosts";
}

std::wstring pendingRepairsPath() {
    wchar_t tmp[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = n > 0 ? std::wstring(tmp, n) : std::wstring(L".\\");
    return dir + L"internet-connection-utility-pending.txt";
}

// A second, machine-wide copy: %TEMP% only survives when the UAC prompt is answered by
// the same account. Credential elevation starts the app as another user with another
// %TEMP%, so the queue would be silently lost without this.
static std::wstring machinePendingPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir = std::wstring(buf, n) + L"\\InternetConnectionUtility";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\pending-repairs.txt";
}

static bool writeRepairList(const std::wstring& path, const std::string& body) {
    if (path.empty()) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(h, body.data(), (DWORD)body.size(), &wrote, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

static std::vector<std::string> readRepairList(const std::wstring& path) {
    std::vector<std::string> ids;
    if (path.empty()) return ids;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ids;
    std::string body;
    char buf[512];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) body.append(buf, got);
    CloseHandle(h);

    size_t start = 0;
    while (start < body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string::npos) end = body.size();
        std::string line = trim(body.substr(start, end - start));
        if (!line.empty()) ids.push_back(line);
        start = end + 1;
    }
    return ids;
}

bool savePendingRepairs(const std::vector<std::string>& ids) {
    if (ids.empty()) {
        clearPendingRepairs();
        return true;
    }
    std::string body;
    for (auto& id : ids) body += id + "\n";
    bool userCopy = writeRepairList(pendingRepairsPath(), body);
    bool machineCopy = writeRepairList(machinePendingPath(), body);
    return userCopy || machineCopy;
}

std::vector<std::string> loadPendingRepairs() {
    std::vector<std::string> ids = readRepairList(pendingRepairsPath());
    if (ids.empty()) ids = readRepairList(machinePendingPath());
    return ids;
}

void clearPendingRepairs() {
    DeleteFileW(pendingRepairsPath().c_str());
    std::wstring m = machinePendingPath();
    if (!m.empty()) DeleteFileW(m.c_str());
}

std::wstring computerName() {
    wchar_t b[256] = {};
    DWORD n = 256;
    GetComputerNameW(b, &n);
    return b;
}

std::wstring osVersion() {
    struct Osvi {
        ULONG sz, major, minor, build, platform;
        WCHAR csd[128];
    } vi{};
    vi.sz = sizeof(vi);
    auto fn = (LONG(WINAPI*)(void*))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (fn) fn(&vi);
    wchar_t b[128];
    if (vi.major >= 10)
        swprintf(b, 128, L"Windows 10/11  build %lu", vi.build);
    else
        swprintf(b, 128, L"Windows %lu.%lu  build %lu", vi.major, vi.minor, vi.build);
    return b;
}

} // namespace net
