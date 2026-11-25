// Internet Connection Utility
// Copyright (c) 2026-2027 Charles McDonald. BSD 3-Clause License.
// Part of Zanphar's WinSuite; see LICENSE.md in the suite root.

#include "doctor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

namespace {

const UINT WM_NP_REFRESH = WM_APP + 21;

std::wstring clockNow() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t b[32];
    swprintf(b, 32, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return b;
}

std::wstring speedText(ULONG64 bps) {
    if (bps == 0) return L"\u2014";
    if (bps >= 1000000000ULL) return net::fmt1(bps / 1.0e9) + L" Gbps";
    if (bps >= 1000000ULL) return net::fmt1(bps / 1.0e6) + L" Mbps";
    return net::fmt0(bps / 1000.0) + L" Kbps";
}

std::wstring trimW(std::wstring s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring firstLines(const std::string& out, size_t maxLen = 240) {
    std::wstring w = net::utf82wstr(out);
    std::wstring acc;
    std::wstringstream ss(w);
    std::wstring line;
    int n = 0;
    while (std::getline(ss, line) && n < 3) {
        line = trimW(line);
        if (line.empty()) continue;
        if (!acc.empty()) acc += L" \u00b7 ";
        acc += line;
        n++;
    }
    if (acc.size() > maxLen) acc = acc.substr(0, maxLen) + L"\u2026";
    if (acc.empty()) acc = L"no output";
    return acc;
}

struct FixOutcome {
    bool ok = false;
    std::wstring msg;
    bool reboot = false;
};

std::string readFileBytes(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(h);
    return out;
}

bool writeFileBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

int yearFromDate(const std::wstring& d) {
    int year = 0;
    std::wstring cur;
    for (wchar_t c : d) {
        if (c == L'-' || c == L'/' || c == L'.') {
            if (cur.size() == 4) year = _wtoi(cur.c_str());
            cur.clear();
        } else if (iswdigit(c)) {
            cur.push_back(c);
        }
    }
    if (cur.size() == 4) year = _wtoi(cur.c_str());
    return year;
}

// ---------------------------------------------------------------- fixes
bool runOk(const std::wstring& cmd, int timeout, std::wstring& detail) {
    net::CmdRes r = net::run(cmd, timeout);
    if (!r.started) {
        detail = L"command could not be started";
        return false;
    }
    if (r.code != 0) {
        detail = firstLines(r.out, 180);
        if (detail.empty() || detail == L"no output") detail = L"exit code " + std::to_wstring(r.code);
        return false;
    }
    detail = firstLines(r.out, 180);
    return true;
}

// Largest IP MTU the current path accepts, measured with DF pings (0 when filtered).
// Used both by the check and by the MTU repair, so a repair queued before any scan
// still targets the real path instead of falling back to a blind 1500.
int measurePathMtu() {
    auto fits = [](int payload) {
        net::CmdRes c = net::run(L"ping -4 -n 1 -f -l " + std::to_wstring(payload) +
                                     L" -w 1200 1.1.1.1",
                                 5000);
        return c.code == 0;
    };
    if (fits(1472)) return 1472 + 28;
    if (!fits(1200)) return 0;
    int lo = 1200, hi = 1471, best = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fits(mid)) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best ? best + 28 : 0;
}

FixOutcome applyFixImpl(const std::string& id, bool adminMode, ULONG pathMtu) {
    FixOutcome o;
    net::Adapter ad;
    net::detectAdapter(ad);
    std::wstring name = ad.name.empty() ? L"Ethernet" : ad.name;

    auto needAdmin = [&]() -> bool {
        if (!adminMode) {
            o.msg = L"Administrator rights are required \u2014 restart Internet Connection Utility as administrator.";
            return true;
        }
        return false;
    };

    std::wstring detail;

    if (id == "fix_flush_dns") {
        net::CmdRes r = net::run(L"ipconfig /flushdns", 15000);
        o.ok = r.started;
        o.msg = L"DNS resolver cache cleared.";
        return o;
    }
    if (id == "fix_register_dns") {
        if (needAdmin()) return o;
        o.ok = runOk(L"ipconfig /registerdns", 30000, detail);
        o.msg = o.ok ? L"DNS registration refreshed for this adapter." : detail;
        return o;
    }
    if (id == "fix_renew") {
        if (needAdmin()) return o;
        runOk(L"ipconfig /release", 20000, detail);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        bool ok = runOk(L"ipconfig /renew", 40000, detail);
        runOk(L"ipconfig /flushdns", 12000, detail);
        o.ok = ok;
        o.msg = ok ? L"DHCP lease released and renewed; addresses refreshed."
                   : L"Lease renewal failed: " + detail;
        return o;
    }
    if (id == "fix_dns_public") {
        if (needAdmin()) return o;
        std::wstring q = L"\"";
        bool ok1 = runOk(L"netsh interface ipv4 set dnsservers name=" + q + name + q +
                             L" static 1.1.1.1 primary",
                         20000, detail);
        runOk(L"netsh interface ipv4 add dnsservers name=" + q + name + q + L" 8.8.8.8 index=2",
              15000, detail);
        runOk(L"ipconfig /flushdns", 12000, detail);
        o.ok = ok1;
        o.msg = ok1 ? L"Adapter DNS set to Cloudflare 1.1.1.1 with 8.8.8.8 fallback."
                    : L"Could not change DNS servers: " + detail;
        return o;
    }
    if (id == "fix_dns_dhcp") {
        if (needAdmin()) return o;
        std::wstring q = L"\"";
        o.ok = runOk(L"netsh interface ipv4 set dnsservers name=" + q + name + q + L" source=dhcp",
                     20000, detail);
        runOk(L"ipconfig /flushdns", 12000, detail);
        o.msg = o.ok ? L"DNS servers now obtained automatically from DHCP." : detail;
        return o;
    }
    if (id == "fix_winsock") {
        if (needAdmin()) return o;
        o.reboot = true;
        o.ok = runOk(L"netsh winsock reset", 25000, detail);
        o.msg = o.ok ? L"Winsock catalog reset. Restart Windows to complete the repair."
                     : detail;
        return o;
    }
    if (id == "fix_tcpip") {
        if (needAdmin()) return o;
        o.reboot = true;
        o.ok = runOk(L"netsh int ip reset", 30000, detail);
        o.msg = o.ok ? L"TCP/IP stack reset. Restart Windows to complete the repair." : detail;
        return o;
    }
    if (id == "fix_autotuning") {
        if (needAdmin()) return o;
        // The check reads Tcp1323Opts, so the repair must actually rewrite it;
        // netsh alone only changes the auto-tuning level and left the fault behind.
        const std::wstring tcpKey = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters";
        bool regOk = net::regSetDword(HKEY_LOCAL_MACHINE, tcpKey, L"Tcp1323Opts", 3);
        bool offOk = net::regDeleteValue(HKEY_LOCAL_MACHINE, tcpKey, L"DisableTaskOffload");
        bool netshOk = runOk(L"netsh int tcp set global autotuninglevel=normal", 20000, detail);
        runOk(L"netsh int tcp set global rss=enabled", 15000, detail);
        o.ok = regOk || netshOk;
        if (o.ok) {
            o.msg = L"Window scaling re-enabled (Tcp1323Opts=3) and auto-tuning set to normal";
            if (offOk) o.msg += L"; task offload restored";
            o.msg += L".";
        } else {
            o.msg = detail;
        }
        return o;
    }
    if (id == "fix_winhttp_proxy") {
        if (needAdmin()) return o;
        o.ok = runOk(L"netsh winhttp reset proxy", 20000, detail);
        o.msg = o.ok ? L"WinHTTP proxy configuration reset to direct access." : detail;
        return o;
    }
    if (id == "fix_user_proxy") {
        o.ok = net::disableUserProxy();
        o.msg = o.ok ? L"User proxy and PAC auto-config disabled." : L"Could not update proxy settings.";
        return o;
    }
    if (id == "fix_nic_power") {
        if (needAdmin()) return o;
        std::wstring key = net::activeAdapterRegKey();
        if (key.empty()) {
            o.msg = L"Could not locate the adapter's driver key.";
            return o;
        }
        o.ok = net::regSetDword(HKEY_LOCAL_MACHINE, key, L"PnPCapabilities", 24);
        o.ok = net::regSetDword(HKEY_LOCAL_MACHINE, key, L"*SelectiveSuspend", 0) || o.ok;
        o.msg = o.ok ? L"Power-down on the network adapter disabled (applies after restart)."
                     : L"Registry write was refused.";
        return o;
    }
    if (id == "fix_wifi_power") {
        if (needAdmin()) return o;
        const wchar_t* sub = L"19cbb8fa-5279-450e-9fac-8a3d5fedd0c1";
        const wchar_t* set = L"12bbebe6-58d6-4636-95bb-3217ef867c1a";
        bool ok = runOk(L"powercfg /setacvalueindex SCHEME_CURRENT " + std::wstring(sub) + L" " +
                            set + L" 0",
                        15000, detail);
        runOk(L"powercfg /setdcvalueindex SCHEME_CURRENT " + std::wstring(sub) + L" " + set + L" 0",
              15000, detail);
        runOk(L"powercfg /setactive SCHEME_CURRENT", 15000, detail);
        o.ok = ok;
        o.msg = ok ? L"Wi-Fi adapter power saving set to maximum performance."
                   : L"Power plan could not be changed: " + detail;
        return o;
    }
    if (id == "fix_services") {
        if (needAdmin()) return o;
        const wchar_t* svcs[] = {L"Dnscache", L"Dhcp", L"NlaSvc", L"WlanSvc", L"Netman"};
        int fixed = 0, failed = 0;
        for (auto s : svcs) {
            net::SvcState st = net::serviceState(s);
            if (!st.exists || st.state == SERVICE_RUNNING) continue;
            net::serviceAuto(s);
            if (net::serviceStart(s)) fixed++;
            else failed++;
        }
        o.ok = failed == 0;
        if (fixed > 0) o.msg = L"Started " + std::to_wstring(fixed) + L" stopped network service(s).";
        else if (failed > 0) o.msg = L"Services are disabled and could not be started.";
        else o.msg = L"All required network services are already running.";
        return o;
    }
    if (id == "fix_hosts") {
        if (needAdmin()) return o;
        std::wstring path = net::hostsFile();
        wchar_t stamp[64];
        SYSTEMTIME st{};
        GetLocalTime(&st);
        swprintf(stamp, 64, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
        std::wstring backup = path + L".internet-connection-utility-" + stamp + L".bak";
        bool copied = CopyFileW(path.c_str(), backup.c_str(), FALSE) != 0;

        std::string body = "# Hosts file restored by Internet Connection Utility\r\n"
                           "# The previous file was saved in the same folder as a .bak file.\r\n"
                           "\r\n"
                           "127.0.0.1  localhost\r\n"
                           "::1        localhost\r\n";
        if (!writeFileBytes(path, body)) {
            o.msg = L"Could not write the hosts file.";
            return o;
        }
        o.ok = copied;
        o.msg = copied ? L"Hosts file reset; the original was backed up next to it."
                       : L"Hosts file rewritten (backup could not be created).";
        return o;
    }
    if (id == "fix_metric") {
        if (needAdmin()) return o;
        if (ad.ifIndex == 0) {
            o.msg = L"No active interface was found.";
            return o;
        }
        MIB_IPINTERFACE_ROW row{};
        row.Family = AF_INET;
        row.InterfaceIndex = ad.ifIndex;
        if (GetIpInterfaceEntry(&row) != NO_ERROR) {
            o.msg = L"Could not read interface metrics.";
            return o;
        }
        row.UseAutomaticMetric = TRUE;
        row.Metric = 0;
        o.ok = SetIpInterfaceEntry(&row) == NO_ERROR;
        o.msg = o.ok ? L"Interface metric reset to automatic." : L"Metric change was refused.";
        return o;
    }
    if (id == "fix_mtu") {
        if (needAdmin()) return o;
        // Match the path, not a fixed 1500: on PPPoE/VPN paths a 1500-byte adapter
        // MTU is exactly the mismatch this repair is meant to remove.
        ULONG measured = pathMtu;
        if (measured < 1280) measured = (ULONG)measurePathMtu();
        bool known = measured >= 1280;
        ULONG target = (known && measured <= 1500) ? measured : 1500;
        std::wstring q = L"\"";
        o.ok = runOk(L"netsh interface ipv4 set subinterface " + q + name + q + L" mtu=" +
                         std::to_wstring(target) + L" store=persistent",
                     20000, detail);
        if (!o.ok) {
            o.msg = detail;
        } else if (known) {
            o.msg = L"Adapter MTU set to " + std::to_wstring(target) +
                    L" bytes to match the measured path.";
        } else {
            o.msg = L"Path MTU could not be measured (ICMP filtered); adapter MTU set to 1500.";
        }
        return o;
    }
    if (id == "fix_enable_adapter") {
        if (needAdmin()) return o;
        std::wstring q = L"\"";
        o.ok = runOk(L"netsh interface set interface name=" + q + name + q + L" admin=enable", 20000,
                     detail);
        o.msg = o.ok ? L"Adapter re-enabled." : detail;
        return o;
    }
    if (id == "fix_restart_adapter") {
        if (needAdmin()) return o;
        std::wstring q = L"\"";
        runOk(L"netsh interface set interface name=" + q + name + q + L" admin=disable", 20000, detail);
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));
        o.ok = runOk(L"netsh interface set interface name=" + q + name + q + L" admin=enable", 20000,
                     detail);
        o.msg = o.ok ? L"Adapter restarted; the connection will recover within a few seconds."
                     : detail;
        return o;
    }
    if (id == "fix_reconnect_wifi") {
        if (needAdmin()) return o;
        net::Wifi w;
        net::wifiCurrent(w);
        if (w.profile.empty()) {
            o.msg = L"No saved Wi-Fi profile is active.";
            return o;
        }
        runOk(L"netsh wlan disconnect", 15000, detail);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        o.ok = runOk(L"netsh wlan connect name=\"" + w.profile + L"\"", 20000, detail);
        o.msg = o.ok ? L"Reconnected to " + w.ssid + L"." : detail;
        return o;
    }

    o.msg = L"Unknown repair.";
    return o;
}

} // namespace

const wchar_t* sevLabel(Sev s) {
    switch (s) {
        case Sev::Pass: return L"PASS";
        case Sev::Info: return L"INFO";
        case Sev::Warn: return L"WARNING";
        case Sev::Critical: return L"CRITICAL";
        case Sev::Running: return L"RUNNING";
        default: return L"SKIPPED";
    }
}

int sevColor(Sev s) {
    switch (s) {
        case Sev::Pass: return 0x34D399;
        case Sev::Info: return 0x60A5FA;
        case Sev::Warn: return 0xFBBF24;
        case Sev::Critical: return 0xF87171;
        case Sev::Running: return 0x8FA3BF;
        default: return 0x64748B;
    }
}

// ---------------------------------------------------------------- lifecycle
Doctor::Doctor() {
    st_.adminMode = net::elevated();
    st_.issues.reserve(48);
    st_.logs.reserve(200);

    st_.fixes = {
        {"fix_flush_dns", L"Flush DNS resolver cache",
         L"Clears stale and poisoned DNS answers that cause slow or failed name resolution.",
         L"ipconfig /flushdns", false, false, false},
        {"fix_renew", L"Renew DHCP lease",
         L"Releases and re-requests the IP address, gateway and DNS servers from the router.",
         L"ipconfig /release  \u2192  ipconfig /renew", true, true, false},
        {"fix_dns_public", L"Use Cloudflare + Google DNS",
         L"Points this adapter at 1.1.1.1 (Cloudflare) with 8.8.8.8 fallback.",
         L"netsh interface ipv4 set dnsservers \u2026 1.1.1.1", true, true, false},
        {"fix_dns_dhcp", L"Return DNS to automatic",
         L"Restores DNS servers supplied by your router over DHCP.",
         L"netsh interface ipv4 set dnsservers source=dhcp", true, false, false},
        {"fix_autotuning", L"Restore TCP auto-tuning",
         L"Re-enables receive-window scaling and RSS, which throttled throughput when disabled.",
         L"netsh int tcp set global autotuninglevel=normal", true, false, false},
        {"fix_nic_power", L"Stop the adapter sleeping",
         L"Disables \u201callow the computer to turn off this device\u201d, a common cause of random drops.",
         L"Registry: PnPCapabilities = 24", true, false, false},
        {"fix_wifi_power", L"Wi-Fi maximum performance",
         L"Turns off wireless power saving in the active power plan for lower latency and fewer drops.",
         L"powercfg /setacvalueindex \u2026 wireless 0", true, false, false},
        {"fix_metric", L"Reset interface metric",
         L"Returns route preference to automatic so a stale VPN or virtual adapter stops winning.",
         L"SetIpInterfaceEntry(UseAutomaticMetric)", true, false, false},
        {"fix_mtu", L"Reset MTU to 1500",
         L"Repairs a mis-sized MTU that makes pages hang while small requests succeed.",
         L"netsh interface ipv4 set subinterface mtu=1500", true, true, false},
        {"fix_services", L"Start network services",
         L"Starts stopped services the network stack depends on (DNS client, DHCP, NLA, WLAN).",
         L"sc config \u2026 start= auto  /  sc start \u2026", true, false, false},
        {"fix_user_proxy", L"Disable user proxy",
         L"Clears a leftover proxy or PAC script that sends traffic to a dead server.",
         L"HKCU Internet Settings: ProxyEnable = 0", false, false, false},
        {"fix_winhttp_proxy", L"Reset WinHTTP proxy",
         L"Removes a machine-wide proxy that only Windows services and apps use.",
         L"netsh winhttp reset proxy", true, false, false},
        {"fix_enable_adapter", L"Enable adapter",
         L"Brings a administratively disabled network adapter back online.",
         L"netsh interface set interface admin=enable", true, false, false},
        {"fix_restart_adapter", L"Restart adapter",
         L"Cycles the NIC driver, clearing a wedged link without a reboot (drops the link briefly).",
         L"netsh interface set interface admin=disable/enable", true, true, false},
        {"fix_reconnect_wifi", L"Reconnect Wi-Fi",
         L"Disconnects and rejoins the saved wireless profile to recover from a stuck association.",
         L"netsh wlan disconnect  \u2192  connect", true, true, false},
        {"fix_hosts", L"Reset hosts file",
         L"Backs up and restores the hosts file when entries are blackholing traffic.",
         L"Restore %windir%\\System32\\drivers\\etc\\hosts", true, true, false},
        {"fix_winsock", L"Reset Winsock catalog",
         L"Rebuilds the Windows socket catalog when a broken layered provider corrupts all traffic.",
         L"netsh winsock reset  (requires restart)", true, true, true},
        {"fix_tcpip", L"Reset TCP/IP stack",
         L"Rewrites the IP stack configuration to defaults. Last resort for persistent failures.",
         L"netsh int ip reset  (requires restart)", true, true, true},
        {"fix_register_dns", L"Re-register DNS",
         L"Refreshes this machine's DNS registration with the local resolver.",
         L"ipconfig /registerdns", true, false, false},
    };
}

void Doctor::log(const std::wstring& text, int level) {
    std::lock_guard<std::mutex> lk(mu_);
    LogEntry e;
    e.time = clockNow();
    e.text = text;
    e.level = level;
    st_.logs.push_back(e);
    if (st_.logs.size() > 400) st_.logs.erase(st_.logs.begin());
}

AppState Doctor::state() {
    std::lock_guard<std::mutex> lk(mu_);
    return st_;
}

bool Doctor::busy() {
    std::lock_guard<std::mutex> lk(mu_);
    return st_.scanning || st_.fixing;
}

void Doctor::emit(HWND h) {
    if (h) PostMessageW(h, WM_NP_REFRESH, 0, 0);
}

void Doctor::setPhase(const std::wstring& p, float progress) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.phase = p;
        st_.progress = progress;
    }
}

void Doctor::cancel() {
    std::lock_guard<std::mutex> lk(mu_);
    st_.cancelled = true;
}

void Doctor::startScan(HWND notify) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (st_.scanning || st_.fixing) return;
        st_.scanning = true;
        st_.cancelled = false;
        st_.progress = 0.f;
        st_.phase = L"Probing network stack";
        st_.issues.clear();
        st_.metrics.clear();
        st_.spark.clear();
        st_.hasResult = false;
    }
    std::thread([this, notify]() {
        scanRunning_ = true;
        scanWorker();
        scanRunning_ = false;
        emit(notify);
    }).detach();
    emit(notify);
}

void Doctor::runFix(const std::string& fixId, HWND notify) {
    runFixList({fixId}, notify);
}

void Doctor::runFixList(const std::vector<std::string>& ids, HWND notify) {
    if (ids.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (st_.scanning || st_.fixing) return;
        st_.fixing = true;
        st_.phase = ids.size() > 1 ? L"Applying repairs" : L"Applying repair";
    }
    std::vector<std::string> queue = ids;
    std::thread([this, notify, queue]() { fixWorker(queue, notify); }).detach();
    emit(notify);
}

void Doctor::fixWorker(std::vector<std::string> ids, HWND notify) {
    bool admin = net::elevated();
    ULONG pathMtu = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.adminMode = admin;
        st_.fixing = true;
        st_.phase = L"Applying repairs";
        st_.progress = 0.f;
        pathMtu = st_.pathMtu;
    }
    emit(notify);
    log(L"Applying " + std::to_wstring(ids.size()) + L" repair(s) in sequence.", 0);

    int okCount = 0, failCount = 0;
    int doneCount = 0;
    const float total = ids.empty() ? 1.f : static_cast<float>(ids.size());
    for (auto& id : ids) {
        std::wstring name = net::utf82wstr(id);
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& f : st_.fixes)
                if (f.id == id) name = f.name;
        }
        log(L"Applying: " + name, 0);
        FixOutcome o = applyFixImpl(id, admin, pathMtu);
        ++doneCount;
        setPhase(L"Applying repairs", doneCount / total);
        log(name + L" \u2014 " + o.msg, o.ok ? 1 : 3);
        if (o.ok) okCount++;
        else failCount++;
        if (o.reboot)
            log(L"\u201c" + name + L"\u201d needs a restart before it takes full effect.", 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    // Re-scan so the user sees the real post-repair state, then report honestly.
    scanWorker();

    std::vector<std::wstring> unresolved;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& is : st_.issues) {
            if (is.sev != Sev::Warn && is.sev != Sev::Critical) continue;
            if (std::find(ids.begin(), ids.end(), is.fixId) != ids.end())
                unresolved.push_back(is.title);
        }
    }
    if (!unresolved.empty()) {
        std::wstring list;
        for (size_t k = 0; k < unresolved.size(); ++k) {
            if (k) list += L", ";
            list += unresolved[k];
        }
        log(L"Still failing after repair: " + list, 2);
    }

    std::wstring note;
    int level = 1;
    if (failCount == 0 && unresolved.empty()) {
        note = ids.size() == 1
                   ? L"Repair applied successfully."
                   : L"All " + std::to_wstring(ids.size()) + L" repairs applied successfully.";
    } else if (okCount == 0) {
        note = L"Repair failed \u2014 see the activity log for the reason.";
        level = 3;
    } else {
        note = std::to_wstring(okCount) + L" applied, " + std::to_wstring(failCount) + L" failed";
        if (!unresolved.empty())
            note += L", " + std::to_wstring(unresolved.size()) + L" check(s) still failing";
        level = 2;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.fixing = false;
        st_.fixNote = note;
        st_.fixLevel = level;
        st_.fixCounter++;
    }
    emit(notify);
}

void Doctor::fixAll(HWND notify, bool skipAdminFixes) {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (st_.scanning || st_.fixing) return;
        st_.fixing = true;
        for (auto& i : st_.issues) {
            if (i.fixId.empty() || !(i.sev == Sev::Critical || i.sev == Sev::Warn)) continue;
            if (skipAdminFixes) {
                bool needsAdmin = false;
                for (auto& f : st_.fixes)
                    if (f.id == i.fixId && f.admin) needsAdmin = true;
                if (needsAdmin) continue;
            }
            if (std::find(ids.begin(), ids.end(), i.fixId) == ids.end()) ids.push_back(i.fixId);
        }
    }
    if (ids.empty()) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            st_.fixing = false;
        }
        log(L"Nothing to repair \u2014 no failing check has an automatic fix.", 1);
        emit(notify);
        return;
    }
    std::thread([this, notify, ids]() { fixWorker(ids, notify); }).detach();
    emit(notify);
}

// ---------------------------------------------------------------- scan
void Doctor::scanWorker() {
    bool adminMode = net::elevated();
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.adminMode = adminMode;
        st_.scanning = true;
        st_.cancelled = false;
        st_.progress = 0.02f;
        st_.issues.clear();
        st_.metrics.clear();
        st_.spark.clear();
        st_.hasResult = false;
        st_.nPass = st_.nWarn = st_.nCrit = 0;
    }
    net::init();
    log(L"Diagnostic run started on " + net::computerName(), 0);

    auto add = [&](Issue i) {
        i.tested = true;
        {
            std::lock_guard<std::mutex> lk(mu_);
            st_.issues.push_back(std::move(i));
        }
    };
    // ------------------------------------------------------------ stack
    {
        Issue i;
        i.id = "stack";
        i.title = L"TCP/IP protocol stack";
        if (!net::init()) {
            i.sev = Sev::Critical;
            i.detail = L"Winsock could not initialize \u2014 no socket can be created.";
            i.advice = L"The Windows socket layer is damaged or a layered service provider failed to "
                       L"load. Resetting the Winsock catalog rebuilds it.";
            i.steps = {L"Reset the Winsock catalog.", L"Restart Windows.",
                       L"If it persists, run a full TCP/IP reset as well."};
            i.fixId = "fix_winsock";
            i.fixLabel = L"Reset Winsock";
            i.admin = i.risky = i.reboot = true;
            i.metric = L"FAILED";
        } else {
            i.sev = Sev::Pass;
            i.detail = L"Winsock 2.2 initialized and the stack accepted socket requests.";
            i.metric = L"OK";
        }
        add(i);
    }

    setPhase(L"Reading adapter configuration", 0.07f);
    net::Adapter ad;
    bool haveAd = net::detectAdapter(ad);
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.adapter = ad;
    }

    // ------------------------------------------------------------ adapter
    {
        Issue i;
        i.id = "adapter";
        i.title = L"Active network adapter";
        if (!haveAd) {
            i.sev = Sev::Critical;
            i.detail = L"No usable network adapter was found.";
            i.advice = L"Every connection starts with a working NIC. Check Device Manager for a "
                       L"disabled device, a missing driver, or airplane mode.";
            i.steps = {L"Open Device Manager \u2192 Network adapters.",
                       L"Enable any disabled adapter and update its driver.",
                       L"Turn off airplane mode if it is active."};
            i.metric = L"NONE";
        } else {
            i.sev = Sev::Pass;
            i.detail = ad.name + (ad.desc.empty() ? L"" : L"  \u2014  " + ad.desc);
            i.advice = L"Address " + net::utf82wstr(ad.ipv4) + L"/" +
                       net::utf82wstr(ad.mask) + L", gateway " +
                       (ad.gateway.empty() ? L"none" : net::utf82wstr(ad.gateway));
            i.metric = speedText(ad.speedBps);
        }
        add(i);
    }

    // ------------------------------------------------------------ link
    {
        Issue i;
        i.id = "link";
        i.title = L"Link / media state";
        if (!haveAd) {
            i.sev = Sev::Skip;
            i.detail = L"No adapter to inspect.";
        } else if (!ad.up) {
            i.sev = Sev::Critical;
            i.detail = ad.name + L" is disconnected or disabled.";
            i.advice = L"The adapter reports no carrier. A disabled adapter, unplugged cable, or "
                       L"powered-down radio produces exactly this state.";
            i.steps = {L"Reconnect the cable or re-seat the adapter.",
                       L"Enable the adapter in Network Connections.",
                       L"Restart the adapter driver."};
            i.fixId = "fix_enable_adapter";
            i.fixLabel = L"Enable adapter";
            i.admin = true;
            i.metric = L"DOWN";
        } else if (!ad.isWifi && ad.speedBps > 0 && ad.speedBps < 100000000ULL) {
            i.sev = Sev::Warn;
            i.detail = L"Link negotiated at " + speedText(ad.speedBps) + L" \u2014 below Fast Ethernet.";
            i.advice = L"A slow link is usually a damaged cable, a bad port, or duplex mismatch. "
                       L"It caps everything you do, and packet loss rises under load.";
            i.steps = {L"Replace the Ethernet cable (Cat 5e or better).",
                       L"Try a different switch or router port.",
                       L"Check for a duplex mismatch on managed switches."};
            i.metric = speedText(ad.speedBps);
        } else {
            i.sev = Sev::Pass;
            i.detail = L"Link is up" + (ad.isWifi ? L" (wireless)" : L" at " + speedText(ad.speedBps));
            i.metric = ad.isWifi ? L"UP" : speedText(ad.speedBps);
        }
        add(i);
    }

    // ------------------------------------------------------------ addressing
    {
        Issue i;
        i.id = "ipv4";
        i.title = L"IPv4 address & subnet";
        if (!haveAd || ad.ipv4.empty()) {
            i.sev = Sev::Critical;
            i.detail = L"No IPv4 address is assigned to the active adapter.";
            i.advice = L"Without an address nothing can be routed. The DHCP request likely never "
                       L"reached a server, or the adapter is still acquiring.";
            i.steps = {L"Renew the DHCP lease.", L"Check the router is powered and serving DHCP."};
            i.fixId = "fix_renew";
            i.fixLabel = L"Renew lease";
            i.admin = i.risky = true;
            i.metric = L"NONE";
        } else if (ad.apipa) {
            i.sev = Sev::Critical;
            i.detail = L"Self-assigned APIPA address " + net::utf82wstr(ad.ipv4) + L" (169.254.0.0/16).";
            i.advice = L"Windows assigns 169.254.x.x only when DHCP fails. You can reach the local "
                       L"segment but never the internet.";
            i.steps = {L"Renew the DHCP lease.",
                       L"Verify the router's DHCP pool is not exhausted.",
                       L"Check for a static address that conflicts with the subnet."};
            i.fixId = "fix_renew";
            i.fixLabel = L"Renew lease";
            i.admin = i.risky = true;
            i.metric = L"APIPA";
        } else if (ad.mask == "255.255.255.255" || ad.mask == "255.255.255.254") {
            i.sev = Sev::Warn;
            i.detail = L"Address " + net::utf82wstr(ad.ipv4) + L" has a /31 or /32 mask.";
            i.advice = L"A single-host subnet cannot reach a gateway, so only link-local peers work.";
            i.metric = L"/32";
        } else {
            i.sev = Sev::Pass;
            i.detail = net::utf82wstr(ad.ipv4) + L"  /  " + net::utf82wstr(ad.mask) +
                       L"   (" + (ad.dhcp ? L"DHCP" : L"static") + L")";
            i.metric = net::utf82wstr(ad.ipv4);
        }
        add(i);
    }

    {
        Issue i;
        i.id = "dhcp";
        i.title = L"DHCP lease health";
        if (!haveAd || !ad.dhcp) {
            i.sev = Sev::Info;
            i.detail = L"Static addressing is configured on this adapter.";
            i.advice = L"No lease to expire. Confirm the static gateway and DNS entries are correct "
                       L"if you see failures after a network change.";
            i.metric = L"STATIC";
        } else if (ad.leaseHoursLeft >= 0 && ad.leaseHoursLeft < 2) {
            i.sev = Sev::Warn;
            i.detail = L"DHCP lease expires in " + net::fmt1(ad.leaseHoursLeft) + L" hour(s).";
            i.advice = L"A lease that is about to expire can drop the address mid-session. Renew it "
                       L"now to avoid an interruption.";
            i.fixId = "fix_renew";
            i.fixLabel = L"Renew lease";
            i.admin = i.risky = true;
            i.metric = net::fmt0(ad.leaseHoursLeft) + L"h";
        } else if (ad.leaseHoursLeft >= 0) {
            i.sev = Sev::Pass;
            i.detail = L"Lease valid for another " + net::fmt1(ad.leaseHoursLeft) + L" hour(s).";
            i.metric = net::fmt0(ad.leaseHoursLeft) + L"h";
        } else {
            i.sev = Sev::Info;
            i.detail = L"DHCP addressing in use; lease expiry unavailable.";
            i.metric = L"DHCP";
        }
        add(i);
    }

    // ------------------------------------------------------------ gateway
    setPhase(L"Testing the default gateway", 0.14f);
    net::PingStats gwPing;
    bool gwValid = haveAd && !ad.gateway.empty() && net::isIpLiteral(ad.gateway);
    {
        Issue i;
        i.id = "gateway";
        i.title = L"Default gateway";
        if (!haveAd) {
            i.sev = Sev::Skip;
            i.detail = L"No adapter to inspect.";
        } else if (ad.gateway.empty()) {
            i.sev = Sev::Critical;
            i.detail = L"No default gateway is configured.";
            i.advice = L"Nothing outside your subnet can be reached without a default route. This is "
                       L"typical of a failed DHCP exchange or an incomplete static configuration.";
            i.steps = {L"Renew the DHCP lease.",
                       L"Or set the gateway to your router's LAN address manually."};
            i.fixId = "fix_renew";
            i.fixLabel = L"Renew lease";
            i.admin = i.risky = true;
            i.metric = L"NONE";
        } else if (!gwValid) {
            i.sev = Sev::Info;
            i.detail = L"Gateway " + net::utf82wstr(ad.gateway) + L" (non-IPv4, not probed).";
            i.metric = L"IPv6";
        } else {
            gwPing = net::pingSeries(ad.gateway, 4, 90, 800);
            if (!gwPing.reachable) {
                i.sev = Sev::Critical;
                i.detail = L"Gateway " + net::utf82wstr(ad.gateway) + L" does not answer (4/4 lost).";
                i.advice = L"Your PC cannot reach its own router. The problem is local: cable, "
                           L"Wi-Fi association, the router itself, or a driver/firewall blocking ICMP "
                           L"and TCP alike.";
                i.steps = {L"Confirm the router's LAN light is on for this port.",
                           L"Power-cycle the router for 30 seconds.",
                           L"Restart the network adapter.",
                           L"Try another cable or port."};
                i.fixId = "fix_restart_adapter";
                i.fixLabel = L"Restart adapter";
                i.admin = i.risky = true;
                i.metric = L"UNREACHABLE";
            } else if (gwPing.lossPct > 0) {
                i.sev = Sev::Warn;
                i.detail = L"Gateway lost " + std::to_wstring(gwPing.lossPct) + L"% of packets, " +
                           net::fmt1(gwPing.avgMs) + L" ms average.";
                i.advice = L"Loss on the very first hop points at the local link: cabling, Wi-Fi "
                           L"interference, or a congested router, not your ISP.";
                i.steps = {L"Move closer to the access point or use a cable.",
                           L"Replace suspect cabling.",
                           L"Restart the router if it has been up for months."};
                i.metric = net::fmt1(gwPing.avgMs) + L" ms";
            } else {
                i.sev = Sev::Pass;
                i.detail = L"Router " + net::utf82wstr(ad.gateway) + L" answered 4/4 probes in " +
                           net::fmt1(gwPing.avgMs) + L" ms.";
                i.metric = net::fmt1(gwPing.avgMs) + L" ms";
            }
        }
        add(i);
    }

    setPhase(L"Inspecting routes and interface health", 0.20f);

    // ------------------------------------------------------------ duplicate IP
    {
        Issue i;
        i.id = "dupip";
        i.title = L"Address conflicts";
        auto dups = net::duplicateIps();
        if (!dups.empty()) {
            i.sev = Sev::Critical;
            std::wstring list;
            for (auto& d : dups) {
                if (!list.empty()) list += L", ";
                list += net::utf82wstr(d);
            }
            i.detail = L"The same address is live on more than one interface: " + list;
            i.advice = L"A duplicate address produces intermittent, hard-to-explain failures because "
                       L"replies are delivered to the wrong host.";
            i.steps = {L"Release and renew the address.", L"Remove the conflicting static address.",
                       L"Check for a second DHCP server on the network."};
            i.fixId = "fix_renew";
            i.fixLabel = L"Renew lease";
            i.admin = i.risky = true;
            i.metric = L"CONFLICT";
        } else {
            i.sev = Sev::Pass;
            i.detail = L"No duplicate IPv4 address is active on this machine.";
            i.metric = L"OK";
        }
        add(i);
    }

    // ------------------------------------------------------------ routes
    {
        Issue i;
        i.id = "routes";
        i.title = L"Default routes & metric";
        int routes = net::defaultRouteCount();
        if (routes > 1) {
            i.sev = Sev::Warn;
            i.detail = std::to_wstring(routes) +
                       L" default routes are installed (VPN or virtual adapter present).";
            i.advice = L"Multiple default routes force Windows to pick one by metric. When the "
                       L"wrong one wins, traffic silently goes into a dead tunnel.";
            i.steps = {L"Disconnect unused VPN or virtual adapters.",
                       L"Reset the interface metric to automatic if a stale route remains."};
            i.metric = std::to_wstring(routes) + L" routes";
        } else if (routes == 0) {
            i.sev = Sev::Info;
            i.detail = L"No IPv4 default route is currently installed.";
            i.metric = L"NONE";
        } else {
            i.sev = Sev::Pass;
            i.detail = L"Exactly one IPv4 default route, metric " + std::to_wstring(ad.metric) +
                       (ad.autoMetric ? L" (automatic)" : L" (manual)") + L".";
            i.metric = L"1 route";
        }
        add(i);
    }

    // ------------------------------------------------------------ interface errors
    {
        Issue i;
        i.id = "iferrors";
        i.title = L"Interface errors & discards";
        uint64_t ie = 0, oe = 0, id = 0, od = 0;
        bool ok = haveAd && net::interfaceStats(ad.ifIndex, ie, oe, id, od);
        if (!ok) {
            i.sev = Sev::Skip;
            i.detail = L"Interface counters are unavailable.";
        } else if (ie + oe > 0) {
            i.sev = (ie + oe > 200) ? Sev::Warn : Sev::Info;
            i.detail = std::to_wstring(ie) + L" receive / " + std::to_wstring(oe) +
                       L" transmit errors, " + std::to_wstring(id + od) + L" discards since boot.";
            i.advice = L"CRC and receive errors are physical: bad cable, failing port, or a driver "
                       L"bug. Discards usually mean a saturated link or buffer exhaustion.";
            i.steps = {L"Replace the cable and try another switch port.",
                       L"Update or roll back the NIC driver.",
                       L"Disable unused offload features if errors persist."};
            i.metric = std::to_wstring(ie + oe) + L" errors";
        } else {
            i.sev = Sev::Pass;
            i.detail = L"No interface errors or discards recorded since boot.";
            i.metric = L"CLEAN";
        }
        add(i);
    }

    // ------------------------------------------------------------ virtual adapter
    {
        Issue i;
        i.id = "virtual";
        i.title = L"Virtual / VPN capture";
        if (haveAd && ad.isVirtual) {
            i.sev = Sev::Warn;
            i.detail = L"The active route is going through " + ad.name + L".";
            i.advice = L"VPN and virtual adapters add encapsulation and a second default route. "
                       L"Disconnects and MTU stalls are common while one is active.";
            i.steps = {L"Disconnect the VPN and re-test.",
                       L"Reset the interface metric if the tunnel left a stale route."};
            i.metric = L"VIRTUAL";
        } else {
            i.sev = Sev::Pass;
            i.detail = L"Traffic is using a physical adapter, not a tunnel.";
            i.metric = L"PHYSICAL";
        }
        add(i);
    }

    // ------------------------------------------------------------ dns servers
    setPhase(L"Validating DNS servers", 0.30f);
    std::vector<net::DnsProbe> probes;
    double dnsBest = -1;
    {
        Issue i;
        i.id = "dnscfg";
        i.title = L"Configured DNS servers";
        if (!haveAd || ad.dns.empty()) {
            i.sev = Sev::Critical;
            i.detail = L"No DNS servers are configured for this adapter.";
            i.advice = L"Without a resolver, names never become addresses: pages fail while IP "
                       L"addresses still work.";
            i.steps = {L"Set DNS to automatic (DHCP) or to a public resolver.",
                       L"Flush the resolver cache afterwards."};
            i.fixId = "fix_dns_public";
            i.fixLabel = L"Set public DNS";
            i.admin = i.risky = true;
            i.metric = L"NONE";
        } else {
            std::wstring list;
            for (size_t k = 0; k < ad.dns.size(); ++k) {
                if (k) list += L", ";
                list += net::utf82wstr(ad.dns[k]);
            }
            i.sev = Sev::Pass;
            i.detail = list;
            i.advice = L"Resolvers are tried in order. A dead first entry adds a timeout to every "
                       L"new name lookup.";
            i.metric = std::to_wstring(ad.dns.size()) + L" server(s)";
        }
        add(i);
    }

    {
        Issue i;
        i.id = "dnsalive";
        i.title = L"DNS server responsiveness";
        if (!haveAd || ad.dns.empty()) {
            i.sev = Sev::Skip;
            i.detail = L"No resolver to probe.";
        } else {
            int alive = 0;
            int v4Total = 0, v6Count = 0;
            std::wstring list;
            for (auto& srv : ad.dns) {
                // Our probe is IPv4 only; do not report an IPv6 resolver as dead.
                if (srv.find(':') != std::string::npos) {
                    v6Count++;
                    continue;
                }
                v4Total++;
                net::DnsProbe p = net::probeDns(srv, "www.msftconnecttest.com", 1600);
                probes.push_back(p);
                if (!list.empty()) list += L"   \u00b7   ";
                list += net::utf82wstr(srv);
                if (p.responded) {
                    alive++;
                    list += L"  " + net::fmt0(p.ms) + L" ms";
                    if (dnsBest < 0 || p.ms < dnsBest) dnsBest = p.ms;
                } else {
                    list += L"  no answer";
                }
            }
            if (v6Count > 0) {
                if (!list.empty()) list += L"   \u00b7   ";
                list += std::to_wstring(v6Count) + L" IPv6 resolver(s) via the system stack";
            }
            if (v4Total == 0) {
                i.sev = Sev::Info;
                i.detail = list + L"  \u2014  no IPv4 resolver is configured, so direct probing "
                                   L"is skipped.";
                i.metric = L"IPv6";
            } else if (alive == 0) {
                i.sev = Sev::Critical;
                i.detail = L"None of the configured resolvers answered a direct UDP query.";
                i.advice = L"DNS is dead, so every name lookup fails or hangs. Either the servers "
                           L"are unreachable (firewall, VPN, captive portal) or the resolver service "
                           L"is broken.";
                i.steps = {L"Flush the DNS cache.",
                           L"Switch this adapter to 1.1.1.1 and retest.",
                           L"Reset Winsock if no server answers at all."};
                i.fixId = "fix_dns_public";
                i.fixLabel = L"Set public DNS";
                i.admin = i.risky = true;
                i.metric = L"NO ANSWER";
            } else if (alive < v4Total) {
                i.sev = Sev::Warn;
                i.detail = list;
                i.advice = L"At least one resolver is silent. Windows waits for it before trying the "
                           L"next, which shows up as multi-second stalls on new connections.";
                i.fixId = "fix_dns_public";
                i.fixLabel = L"Use public DNS";
                i.admin = i.risky = true;
                i.metric = std::to_wstring(alive) + L"/" + std::to_wstring(v4Total);
            } else {
                i.sev = dnsBest > 250 ? Sev::Warn : Sev::Pass;
                i.detail = list;
                if (dnsBest > 250)
                    i.advice = L"Every resolver answers, but slowly. High DNS latency delays the "
                               L"start of every connection even when throughput is fine.";
                i.metric = net::fmt0(dnsBest) + L" ms";
            }
        }
        add(i);
    }

    // ------------------------------------------------------------ resolution
    {
        Issue i;
        i.id = "resolve";
        i.title = L"Name resolution";
        const char* names[] = {"www.google.com", "www.cloudflare.com", "www.microsoft.com"};
        int okCount = 0;
        double sum = 0;
        std::wstring sample;
        for (auto n : names) {
            net::ResolveRes r = net::resolve(n, 2500);
            if (r.ok) {
                okCount++;
                sum += r.ms;
                if (sample.empty() && !r.v4.empty())
                    sample = net::utf82wstr(n) + L" \u2192 " + net::utf82wstr(r.v4[0]);
            }
        }
        double avg = okCount ? sum / okCount : 0;
        if (okCount == 0) {
            i.sev = Sev::Critical;
            i.detail = L"None of 3 well-known names could be resolved.";
            i.advice = L"If DNS servers answer raw queries but Windows still cannot resolve, the "
                       L"resolver cache or IP stack is at fault, not the servers.";
            i.steps = {L"Flush the DNS cache.", L"Reset Winsock if it persists."};
            i.fixId = "fix_flush_dns";
            i.fixLabel = L"Flush DNS";
            i.metric = L"FAILED";
        } else if (okCount < 3) {
            i.sev = Sev::Warn;
            i.detail = std::to_wstring(okCount) + L" of 3 names resolved (\u00f8 " + net::fmt0(avg) +
                       L" ms).";
            i.advice = L"Partial resolution usually means a flaky resolver or a DNS filter blocking "
                       L"specific domains.";
            i.fixId = "fix_flush_dns";
            i.fixLabel = L"Flush DNS";
            i.metric = std::to_wstring(okCount) + L"/3";
        } else {
            i.sev = avg > 300 ? Sev::Warn : Sev::Pass;
            i.detail = L"All 3 names resolved in an average of " + net::fmt0(avg) + L" ms.  " + sample;
            i.metric = net::fmt0(avg) + L" ms";
        }
        add(i);
    }

    // ------------------------------------------------------------ internet icmp
    setPhase(L"Measuring latency, jitter and loss", 0.42f);
    net::PingStats inet = net::pingSeries("1.1.1.1", 8, 110, 900);
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.spark = inet.samples;
    }

    // ------------------------------------------------------------ retrans sampling setup
    uint64_t seg0 = 0, ret0 = net::tcpRetrans(seg0);

    {
        Issue i;
        i.id = "icmp";
        i.title = L"Internet reachability (ICMP)";
        if (!inet.reachable) {
            i.sev = Sev::Critical;
            i.detail = L"8 of 8 probes to 1.1.1.1 were lost.";
            i.advice = L"Either the internet path is down or ICMP is filtered. The HTTPS test below "
                       L"tells the two apart.";
            i.steps = {L"Confirm the gateway is reachable first.",
                       L"Power-cycle the router and modem.",
                       L"Check the ISP for an outage."};
            i.metric = L"100% loss";
        } else {
            i.sev = inet.lossPct >= 10 || inet.avgMs > 200 ? Sev::Warn : Sev::Pass;
            i.detail = net::fmt1(inet.avgMs) + L" ms average, " + net::fmt1(inet.minMs) + L"\u2013" +
                       net::fmt1(inet.maxMs) + L" ms range, " +
                       std::to_wstring(inet.lossPct) + L"% loss.";
            if (i.sev == Sev::Warn)
                i.advice = L"Packet loss and high round-trip time make calls stutter and downloads "
                           L"stall. They are usually congestion, Wi-Fi interference, or a saturated "
                           L"upstream.";
            i.metric = net::fmt1(inet.avgMs) + L" ms";
        }
        add(i);
    }

    {
        Issue i;
        i.id = "loss";
        i.title = L"Packet loss & jitter";
        if (!inet.reachable) {
            i.sev = Sev::Critical;
            i.detail = L"No sample could be collected.";
            i.metric = L"\u2014";
        } else {
            if (inet.lossPct >= 10 || inet.jitterMs > 45) i.sev = Sev::Critical;
            else if (inet.lossPct > 0 || inet.jitterMs > 18) i.sev = Sev::Warn;
            else i.sev = Sev::Pass;
            i.detail = std::to_wstring(inet.lossPct) + L"% loss, " + net::fmt1(inet.jitterMs) +
                       L" ms jitter over 8 probes.";
            if (i.sev != Sev::Pass)
                i.advice = L"Jitter above ~20 ms breaks real-time audio and video. Combined with loss "
                           L"it usually means Wi-Fi interference, bufferbloat, or a congested link.";
            i.metric = net::fmt1(inet.jitterMs) + L" ms";
        }
        add(i);
    }

    // ------------------------------------------------------------ tcp + https
    setPhase(L"Testing TLS egress and captive portal", 0.55f);
    net::TcpRes tcp = net::tcpConnect("1.1.1.1", 443, 3000);
    {
        Issue i;
        i.id = "tcp443";
        i.title = L"TCP handshake (port 443)";
        if (tcp.ok) {
            i.sev = tcp.ms > 400 ? Sev::Warn : Sev::Pass;
            i.detail = L"Connected to 1.1.1.1:443 in " + net::fmt0(tcp.ms) + L" ms.";
            if (tcp.ms > 400)
                i.advice = L"Slow handshakes point to latency or a middlebox inspecting traffic.";
            i.metric = net::fmt0(tcp.ms) + L" ms";
        } else {
            i.sev = Sev::Critical;
            i.detail = L"Could not open a TCP connection to 1.1.1.1:443.";
            i.advice = L"ICMP may be blocked, but a failed TCP handshake means real egress filtering: "
                       L"firewall, proxy, VPN kill-switch, or ISP interference.";
            i.steps = {L"Check Windows Firewall for a blocking outbound rule.",
                       L"Disable any VPN or proxy and retest.",
                       L"Reset the Winsock catalog if every port fails."};
            i.fixId = "fix_winsock";
            i.fixLabel = L"Reset Winsock";
            i.admin = i.risky = i.reboot = true;
            i.metric = L"BLOCKED";
        }
        add(i);
    }

    net::HttpRes https = net::httpGet(L"www.gstatic.com", L"/generate_204", true, 7000, 64);
    {
        Issue i;
        i.id = "https";
        i.title = L"HTTPS egress";
        if (https.ok && (https.status == 204 || https.status == 200)) {
            i.sev = https.ms > 1500 ? Sev::Warn : Sev::Pass;
            i.detail = L"TLS handshake and HTTP " + std::to_wstring(https.status) + L" in " +
                       net::fmt0(https.ms) + L" ms.";
            i.metric = net::fmt0(https.ms) + L" ms";
        } else {
            i.sev = Sev::Critical;
            i.detail = L"HTTPS request failed" +
                       (https.status ? L" (HTTP " + std::to_wstring(https.status) + L")" : L"") +
                       (https.err.empty() ? L"." : L" \u2014 " + net::utf82wstr(https.err) + L".");
            i.advice = L"Name resolution may succeed while TLS fails: certificate interception, a "
                       L"stale proxy, a firewall rule, or a captive portal all produce this.";
            i.steps = {L"Open a browser and check for a sign-in page.",
                       L"Reset the WinHTTP proxy.",
                       L"Check the system clock \u2014 a wrong time breaks TLS."};
            i.fixId = "fix_winhttp_proxy";
            i.fixLabel = L"Reset proxy";
            i.admin = true;
            i.metric = L"FAILED";
        }
        add(i);
    }

    {
        Issue i;
        i.id = "portal";
        i.title = L"Captive portal detection";
        net::HttpRes cap =
            net::httpGet(L"www.msftconnecttest.com", L"/connecttest.txt", false, 6000, 256);
        bool expected = cap.ok && cap.body.find("Microsoft Connect Test") != std::string::npos;
        if (expected) {
            i.sev = Sev::Pass;
            i.detail = L"Connectivity probe returned the expected content.";
            i.metric = L"CLEAN";
        } else if (cap.ok && cap.status == 200) {
            i.sev = Sev::Warn;
            i.detail = L"Probe returned unexpected content \u2014 a sign-in or filter page is being "
                       L"served.";
            i.advice = L"A captive portal or content filter rewrites your traffic until you "
                       L"authenticate. Nothing else works properly until it is cleared.";
            i.steps = {L"Open http://neverssl.com in a browser and follow the sign-in page.",
                       L"Disconnect and reconnect to the network afterwards."};
            i.metric = L"PORTAL";
        } else {
            i.sev = Sev::Info;
            i.detail = L"Probe could not be completed; portal state is unknown.";
            i.metric = L"UNKNOWN";
        }
        add(i);
    }

    // ------------------------------------------------------------ throughput
    setPhase(L"Measuring throughput", 0.68f);
    net::HttpRes dl = net::httpDownload(L"speed.cloudflare.com", L"/__down?bytes=2000000", true,
                                        9000, 2000000);
    {
        Issue i;
        i.id = "speed";
        i.title = L"Download throughput";
        if (dl.ok && dl.mbps > 0) {
            if (dl.mbps >= 25) i.sev = Sev::Pass;
            else if (dl.mbps >= 5) i.sev = Sev::Warn;
            else i.sev = Sev::Critical;
            i.detail = L"Downloaded " + net::fmt1(dl.bytes / 1.0e6) + L" MB at " +
                       net::fmt1(dl.mbps) + L" Mbps.";
            if (i.sev != Sev::Pass)
                i.advice = L"Low throughput with low latency means shaping, a congested Wi-Fi "
                           L"channel, or a link negotiating at a slow rate. Low throughput with "
                           L"loss means a failing path.";
            i.metric = net::fmt1(dl.mbps) + L" Mbps";
        } else {
            i.sev = Sev::Skip;
            i.detail = L"Throughput probe unavailable (endpoint blocked or too slow).";
            i.metric = L"\u2014";
        }
        add(i);
    }

    // ------------------------------------------------------------ retransmits
    {
        Issue i;
        i.id = "retrans";
        i.title = L"TCP retransmission rate";
        uint64_t seg1 = 0, ret1 = net::tcpRetrans(seg1);
        uint64_t dSeg = seg1 > seg0 ? seg1 - seg0 : 0;
        uint64_t dRet = ret1 > ret0 ? ret1 - ret0 : 0;
        if (dSeg < 25) {
            i.sev = Sev::Skip;
            i.detail = L"Not enough traffic in this run to judge retransmissions.";
            i.metric = L"\u2014";
        } else {
            double pct = (double)dRet * 100.0 / (double)dSeg;
            if (pct > 8) i.sev = Sev::Critical;
            else if (pct > 2.5) i.sev = Sev::Warn;
            else i.sev = Sev::Pass;
            i.detail = std::to_wstring(dRet) + L" of " + std::to_wstring(dSeg) +
                       L" segments retransmitted (" + net::fmt1(pct) + L"%).";
            if (i.sev != Sev::Pass)
                i.advice = L"Retransmissions mean packets are being dropped in flight. Above 2.5% "
                           L"throughput collapses even though a speed test may look acceptable.";
            i.metric = net::fmt1(pct) + L"%";
        }
        add(i);
    }

    // ------------------------------------------------------------ mtu
    setPhase(L"Probing path MTU", 0.76f);
    if (inet.reachable) {
        int pathMtuNow = measurePathMtu();
        Issue i;
        i.id = "mtu";
        i.title = L"Path MTU / fragmentation";
        if (pathMtuNow == 0) {
            i.sev = Sev::Info;
            i.detail = L"Fragmentation probe was filtered; path MTU could not be measured.";
            i.advice = L"ICMP is blocked on this path. Adapter MTU is " +
                       std::to_wstring(ad.mtu) + L" bytes.";
            i.metric = L"\u2014";
        } else {
            int mtu = pathMtuNow;
            {
                std::lock_guard<std::mutex> lk(mu_);
                st_.pathMtu = (ULONG)mtu;
            }
            std::wstring detail = L"Largest unfragmented packet: " + std::to_wstring(mtu) + L" bytes.";
            if (ad.mtu > (ULONG)mtu) {
                detail += L"  The adapter advertises " + std::to_wstring(ad.mtu) + L".";
            }
            if (mtu < 1280) {
                i.sev = Sev::Critical;
                i.detail = detail;
                i.advice = L"A path MTU this small (PPPoE, VPN, or a broken tunnel) causes the classic "
                           L"symptom: small requests work, large pages and uploads hang forever.";
                i.steps = {L"Set the adapter MTU to the measured path MTU.",
                           L"Restart the VPN, which often left an MTU clamp behind."};
                i.fixId = "fix_mtu";
                i.fixLabel = L"Match MTU";
                i.admin = i.risky = true;
                i.metric = std::to_wstring(mtu) + L" B";
            } else if (ad.mtu > (ULONG)mtu) {
                i.sev = Sev::Warn;
                i.detail = detail;
                i.advice = L"The adapter sends packets larger than the path can carry, so large "
                           L"transfers stall while small ones succeed. Match the adapter to the path.";
                i.steps = {L"Set the adapter MTU to the measured path MTU.",
                           L"Re-test after changing VPN or PPPoE settings."};
                i.fixId = "fix_mtu";
                i.fixLabel = L"Match MTU";
                i.admin = i.risky = true;
                i.metric = std::to_wstring(mtu) + L" B";
            } else {
                i.sev = Sev::Pass;
                i.detail = detail + L"  Adapter MTU matches the path.";
                i.metric = std::to_wstring(mtu) + L" B";
            }
        }
        add(i);
    }

    // ------------------------------------------------------------ tcp tuning
    {
        Issue i;
        i.id = "autotuning";
        i.title = L"TCP receive window auto-tuning";
        const std::wstring tcpKey = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters";
        DWORD tcp1323 = 3;
        bool has = net::regGetDword(HKEY_LOCAL_MACHINE, tcpKey, L"Tcp1323Opts", tcp1323);
        DWORD taskOff = 0;
        bool hasOff = net::regGetDword(HKEY_LOCAL_MACHINE, tcpKey, L"DisableTaskOffload", taskOff);
        if (has && tcp1323 == 0) {
            i.sev = Sev::Critical;
            i.detail = L"Tcp1323Opts = 0 \u2014 window scaling is disabled.";
            i.advice = L"With scaling off, throughput collapses on any link with real latency: a "
                       L"50 ms path caps near 20 Mbps no matter how fast the plan is.";
            i.steps = {L"Restore the auto-tuning level to normal.",
                       L"Restart Windows for the change to apply."};
            i.fixId = "fix_autotuning";
            i.fixLabel = L"Restore tuning";
            i.admin = true;
            i.metric = L"DISABLED";
        } else if (hasOff && taskOff != 0) {
            i.sev = Sev::Warn;
            i.detail = L"DisableTaskOffload = 1 \u2014 all TCP/IP processing is on the CPU.";
            i.advice = L"Task offload disabled raises CPU load and latency on fast links. It is "
                       L"sometimes set by \u201ctweaking\u201d tools and rarely helps.";
            i.fixId = "fix_autotuning";
            i.fixLabel = L"Restore tuning";
            i.admin = true;
            i.metric = L"OFFLOAD OFF";
        } else {
            i.sev = Sev::Pass;
            i.detail = L"Window scaling and offload are at Windows defaults.";
            i.metric = L"OPTIMAL";
        }
        add(i);
    }

    // ------------------------------------------------------------ nic power
    {
        Issue i;
        i.id = "nicpower";
        i.title = L"Adapter power management";
        std::wstring key = net::activeAdapterRegKey();
        DWORD pnp = 0;
        bool has = !key.empty() && net::regGetDword(HKEY_LOCAL_MACHINE, key, L"PnPCapabilities", pnp);
        if (has && (pnp & 0x18) != 0) {
            i.sev = Sev::Pass;
            i.detail = L"Windows is not allowed to power down this adapter.";
            i.metric = L"SAFE";
        } else {
            i.sev = Sev::Warn;
            i.detail = L"Windows may switch the adapter off to save power.";
            i.advice = L"This is the classic cause of random, unexplained disconnects: the NIC is "
                       L"suspended under idle traffic and takes seconds to wake, dropping the "
                       L"connection.";
            i.steps = {L"Device Manager \u2192 adapter \u2192 Power Management.",
                       L"Clear \u201cAllow the computer to turn off this device\u201d.",
                       L"Or use the repair below."};
            i.fixId = "fix_nic_power";
            i.fixLabel = L"Disable power-down";
            i.admin = true;
            i.metric = L"ENABLED";
        }
        add(i);
    }

    // ------------------------------------------------------------ wifi
    net::Wifi wifi;
    net::wifiCurrent(wifi);
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.wifi = wifi;
    }
    std::vector<net::WifiNeighbor> neighbors;
    net::wifiNeighbors(neighbors);
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.neighbors = neighbors;
    }

    if (wifi.supported) {
        {
            Issue i;
            i.id = "wifimedia";
            i.title = L"Wi-Fi radio & association";
            if (!wifi.radioOn) {
                i.sev = Sev::Critical;
                i.detail = L"The wireless radio is switched off (hardware or software).";
                i.advice = L"Airplane mode or the physical switch disables the radio, so no network "
                           L"is visible at all.";
                i.steps = {L"Turn off airplane mode (Win+A).", L"Check the laptop's radio switch."};
                i.metric = L"OFF";
            } else if (!wifi.connected) {
                i.sev = ad.isWifi ? Sev::Critical : Sev::Info;
                i.detail = L"Wi-Fi is not associated" +
                           (wifi.stateText.empty() ? L"." : L" (" + wifi.stateText + L").");
                i.advice = L"Without an association there is no wireless link. Ethernet may still be "
                           L"carrying traffic.";
                i.steps = {L"Join a network from the system tray.",
                           L"Forget and re-add the profile if authentication keeps failing."};
                i.fixId = "fix_reconnect_wifi";
                i.fixLabel = L"Reconnect Wi-Fi";
                i.admin = i.risky = true;
                i.metric = L"IDLE";
            } else {
                i.sev = Sev::Pass;
                i.detail = L"Associated with \u201c" + wifi.ssid + L"\u201d  \u00b7  " + wifi.phy +
                           L"  \u00b7  " + wifi.auth + L" / " + wifi.cipher;
                i.metric = L"CONNECTED";
            }
            add(i);
        }

        if (wifi.connected) {
            Issue i;
            i.id = "wifisignal";
            i.title = L"Wi-Fi signal strength";
            if (wifi.signalPct < 40) {
                i.sev = Sev::Critical;
                i.detail = L"Signal quality " + std::to_wstring(wifi.signalPct) + L"%  (" +
                           std::to_wstring(wifi.rssi) + L" dBm).";
                i.advice = L"Below ~40% the radio down-shifts to fragile rates; retries and drops "
                           L"follow. This single factor explains most \u201crandom disconnect\u201d "
                           L"reports.";
                i.steps = {L"Move closer to the access point or remove obstructions.",
                           L"Switch to the 5 GHz band if available.",
                           L"Reposition the router's antennas."};
                i.metric = std::to_wstring(wifi.signalPct) + L"%";
            } else if (wifi.signalPct < 65) {
                i.sev = Sev::Warn;
                i.detail = L"Signal quality " + std::to_wstring(wifi.signalPct) + L"%  (" +
                           std::to_wstring(wifi.rssi) + L" dBm).";
                i.advice = L"Workable, but with little headroom. Interference or distance will push "
                           L"this into drop-outs.";
                i.metric = std::to_wstring(wifi.signalPct) + L"%";
            } else {
                i.sev = Sev::Pass;
                i.detail = L"Signal quality " + std::to_wstring(wifi.signalPct) + L"%  (" +
                           std::to_wstring(wifi.rssi) + L" dBm), link rate " +
                           net::fmt0(wifi.rxKbps / 1000.0) + L" Mbps.";
                i.metric = std::to_wstring(wifi.signalPct) + L"%";
            }
            add(i);
        }

        if (wifi.connected) {
            Issue i;
            i.id = "wifiband";
            i.title = L"Wi-Fi band & channel";
            {
                int sameChannel = 0, band24 = 0;
                for (auto& n : neighbors) {
                    if (n.band5 || n.band6) continue;
                    band24++;
                    if (n.channel == wifi.channel) sameChannel++;
                }
                std::wstring ch = L"channel " + std::to_wstring(wifi.channel) +
                                  (wifi.band6 ? L" (6 GHz)" : wifi.band5 ? L" (5 GHz)" : L" (2.4 GHz)");
                if (wifi.band5 || wifi.band6) {
                    i.sev = sameChannel >= 3 ? Sev::Warn : Sev::Pass;
                    i.detail = ch + L"; " + std::to_wstring(neighbors.size()) +
                               L" nearby networks, " + std::to_wstring(sameChannel) +
                               L" sharing this channel.";
                    if (i.sev == Sev::Warn)
                        i.advice = L"Several neighbours on the same 5 GHz channel cause airtime "
                                   L"contention. Pick another channel on the router.";
                    i.metric = wifi.band6 ? L"6 GHz" : L"5 GHz";
                } else {
                    i.sev = Sev::Warn;
                    i.detail = ch + L"; " + std::to_wstring(band24) + L" neighbouring 2.4 GHz networks, " +
                               std::to_wstring(sameChannel) + L" on the same channel.";
                    i.advice = L"2.4 GHz has only three clean channels and is shared with Bluetooth, "
                               L"microwaves and every nearby router. It is the most common source of "
                               L"interference-related disconnects.";
                    i.steps = {L"Join the 5 GHz SSID if the router broadcasts one.",
                               L"Set the router to channel 1, 6 or 11.",
                               L"Move to 80 MHz only on 5 GHz, never on 2.4 GHz."};
                    i.metric = L"2.4 GHz";
                }
            }
            add(i);
        }

        if (wifi.connected) {
            Issue i;
            i.id = "wifisec";
            i.title = L"Wi-Fi security";
            if (wifi.auth.find(L"Open") != std::wstring::npos ||
                       wifi.cipher.find(L"None") != std::wstring::npos) {
                i.sev = Sev::Critical;
                i.detail = L"The network is open \u2014 no encryption (" + wifi.auth + L").";
                i.advice = L"An open network lets anyone nearby capture and rewrite your traffic, and "
                           L"often requires a portal that expires mid-session.";
                i.steps = {L"Connect to an encrypted network.", L"Use a VPN on untrusted Wi-Fi."};
                i.metric = L"OPEN";
            } else if (wifi.cipher.find(L"WEP") != std::wstring::npos ||
                       wifi.cipher.find(L"TKIP") != std::wstring::npos) {
                i.sev = Sev::Warn;
                i.detail = L"Legacy encryption in use: " + wifi.auth + L" / " + wifi.cipher + L".";
                i.advice = L"WEP and TKIP cap the link at 54 Mbps and force fragile rates. Modern "
                           L"hardware often drops these associations.";
                i.steps = {L"Switch the router to WPA2-AES or WPA3.",
                           L"Re-add the profile after the change."};
                i.metric = L"LEGACY";
            } else {
                i.sev = Sev::Pass;
                i.detail = wifi.auth + L" with " + wifi.cipher + L".";
                i.metric = L"STRONG";
            }
            add(i);
        }

        if (wifi.connected) {
            Issue i;
            i.id = "wifipower";
            i.title = L"Wi-Fi power saving";
            net::CmdRes pc = net::run(
                L"powercfg /query SCHEME_CURRENT 19cbb8fa-5279-450e-9fac-8a3d5fedd0c1 "
                L"12bbebe6-58d6-4636-95bb-3217ef867c1a",
                12000);
            int ac = -1;
            if (pc.started) {
                std::wstring out = net::utf82wstr(pc.out);
                size_t pos = 0;
                int found = 0;
                while ((pos = out.find(L"0x", pos)) != std::wstring::npos && found < 4) {
                    std::wstring hex = out.substr(pos + 2, 8);
                    int v = (int)wcstoul(hex.c_str(), nullptr, 16);
                    if (found == 0) ac = v;
                    found++;
                    pos += 2;
                }
            }
            if (ac >= 2) {
                i.sev = Sev::Warn;
                i.detail = L"Wireless power saving is set to " +
                           std::wstring(ac == 3 ? L"maximum power saving" : L"medium power saving") +
                           L" on AC power.";
                i.advice = L"Aggressive power saving parks the radio between packets, adding latency "
                           L"and causing brief disconnects that look random.";
                i.fixId = "fix_wifi_power";
                i.fixLabel = L"Max performance";
                i.admin = true;
                i.metric = L"SAVING";
            } else if (ac >= 0) {
                i.sev = Sev::Pass;
                i.detail = L"Wireless power saving is disabled (maximum performance).";
                i.metric = L"PERFORMANCE";
            } else {
                i.sev = Sev::Info;
                i.detail = L"Power plan value could not be read.";
                i.metric = L"\u2014";
            }
            add(i);
        }
    }

    // ------------------------------------------------------------ services
    setPhase(L"Verifying core network services", 0.84f);
    {
        Issue i;
        i.id = "services";
        i.title = L"Core network services";
        struct S {
            const wchar_t* name;
            const wchar_t* label;
            int level; // 0 critical, 1 warning, 2 only if disabled
        };
        std::vector<S> list = {{L"Dnscache", L"DNS Client", 0},
                               {L"Dhcp", L"DHCP Client", 0},
                               {L"NlaSvc", L"Network Location Awareness", 1},
                               {L"Netman", L"Network Connections", 2}};
        if (wifi.supported) list.push_back({L"WlanSvc", L"WLAN AutoConfig", 1});

        std::wstring bad, good;
        int missing = 0, worst = -1;
        for (auto& s : list) {
            net::SvcState st = net::serviceState(s.name);
            if (s.level == 2) {
                // Network Connections is demand-start; stopped is normal, disabled is not.
                if (st.exists && st.startType == SERVICE_DISABLED) {
                    if (!bad.empty()) bad += L", ";
                    bad += s.label;
                    worst = std::max(worst, 1);
                    missing++;
                } else {
                    if (!good.empty()) good += L", ";
                    good += s.label;
                }
                continue;
            }
            if (!st.exists || st.state != SERVICE_RUNNING) {
                missing++;
                worst = std::max(worst, s.level);
                if (!bad.empty()) bad += L", ";
                bad += s.label;
            } else {
                if (!good.empty()) good += L", ";
                good += s.label;
            }
        }
        if (missing > 0) {
            i.sev = worst == 0 ? Sev::Critical : Sev::Warn;
            i.detail = L"Not running: " + bad + L".";
            i.advice = L"Network Location Awareness decides whether a network is public, private or "
                       L"unidentified; without it, Windows blocks sharing and captive portals. A "
                       L"stopped DNS Client also defeats caching.";
            i.steps = {L"Start the service and set it to Automatic.",
                       L"Restart Windows if the service refuses to start."};
            i.fixId = "fix_services";
            i.fixLabel = L"Start services";
            i.admin = true;
            i.metric = std::to_wstring(missing) + L" stopped";
        } else {
            i.sev = Sev::Pass;
            i.detail = L"Running: " + good + L".";
            i.metric = L"ALL RUNNING";
        }
        add(i);
    }

    // ------------------------------------------------------------ proxy
    {
        Issue i;
        i.id = "proxy";
        i.title = L"Proxy configuration";
        net::ProxyInfo p = net::proxyInfo();
        if (p.enabled) {
            i.sev = Sev::Warn;
            i.detail = L"A proxy is active" +
                       (p.server.empty() ? L"" : L": " + p.server) +
                       (p.pac.empty() ? L"" : L"  PAC: " + p.pac);
            i.advice = L"If that proxy is offline, browsers and apps fail while ping still works. "
                       L"Leftovers from old software and malware are common.";
            i.steps = {L"Settings \u2192 Network & Internet \u2192 Proxy \u2192 turn off.",
                       L"Or apply the repair below."};
            i.fixId = "fix_user_proxy";
            i.fixLabel = L"Disable proxy";
            i.metric = L"ENABLED";
        } else {
            i.sev = Sev::Pass;
            i.detail = L"No user proxy or PAC script is configured.";
            i.metric = L"DIRECT";
        }
        add(i);
    }

    // ------------------------------------------------------------ hosts
    {
        Issue i;
        i.id = "hosts";
        i.title = L"Hosts file integrity";
        std::string hostBytes = readFileBytes(net::hostsFile());
        int custom = 0;
        std::wstring first;
        if (!hostBytes.empty()) {
            std::istringstream hs(hostBytes);
            std::string line;
            while (std::getline(hs, line)) {
                std::string t = net::trim(line);
                if (t.empty() || t[0] == '#') continue;
                std::string low = t;
                std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                if (low.find("localhost") != std::string::npos) continue;
                if (low.rfind("127.0.0.1", 0) == 0 || low.rfind("::1", 0) == 0) continue;
                custom++;
                if (first.empty()) first = net::utf82wstr(t);
            }
        }
        if (custom > 0) {
            i.sev = custom > 3 ? Sev::Warn : Sev::Info;
            i.detail = std::to_wstring(custom) + L" custom mapping(s) redirect hostnames.  " + first;
            i.advice = L"Entries here override DNS and are a favourite of ad-blockers, cracked "
                       L"software and malware. A stale entry blackholes a site permanently.";
            i.steps = {L"Inspect %windir%\\System32\\drivers\\etc\\hosts.",
                       L"Remove the offending lines or reset the file."};
            i.fixId = "fix_hosts";
            i.fixLabel = L"Reset hosts";
            i.admin = i.risky = true;
            i.metric = std::to_wstring(custom) + L" entries";
        } else {
            i.sev = Sev::Pass;
            i.detail = L"No custom host mappings \u2014 resolution is handled entirely by DNS.";
            i.metric = L"CLEAN";
        }
        add(i);
    }

    // ------------------------------------------------------------ firewall
    {
        Issue i;
        i.id = "firewall";
        i.title = L"Windows Firewall";
        const std::wstring base =
            L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy";
        DWORD v = 0, on = 0, n = 0;
        if (net::regGetDword(HKEY_LOCAL_MACHINE, base + L"\\StandardProfile", L"EnableFirewall", v)) {
            n++;
            on += v ? 1 : 0;
        }
        if (net::regGetDword(HKEY_LOCAL_MACHINE, base + L"\\PublicProfile", L"EnableFirewall", v)) {
            n++;
            on += v ? 1 : 0;
        }
        if (net::regGetDword(HKEY_LOCAL_MACHINE, base + L"\\DomainProfile", L"EnableFirewall", v)) {
            n++;
            on += v ? 1 : 0;
        }
        if (n == 0) {
            i.sev = Sev::Info;
            i.detail = L"Firewall state could not be read.";
            i.metric = L"\u2014";
        } else if (on == 0) {
            i.sev = Sev::Info;
            i.detail = L"Windows Firewall is disabled on all profiles.";
            i.advice = L"Not a connectivity fault, but unprotected. A third-party firewall may still "
                       L"be filtering traffic.";
            i.metric = L"OFF";
        } else {
            i.sev = Sev::Pass;
            i.detail = std::to_wstring(on) + L" of " + std::to_wstring(n) +
                       L" firewall profile(s) enabled. Outbound traffic is allowed by default.";
            i.advice = L"If a specific application cannot connect, allow it through rather than "
                       L"disabling the firewall.";
            i.metric = std::to_wstring(on) + L"/" + std::to_wstring(n);
        }
        add(i);
    }

    // ------------------------------------------------------------ driver
    {
        Issue i;
        i.id = "driver";
        i.title = L"Adapter driver";
        if (!haveAd || ad.driverProvider.empty()) {
            i.sev = Sev::Info;
            i.detail = L"Driver metadata unavailable for this adapter.";
            i.metric = L"\u2014";
        } else {
            int year = yearFromDate(ad.driverDate);
            time_t now = time(nullptr);
            tm lt{};
            localtime_s(&lt, &now);
            int curYear = lt.tm_year + 1900;
            std::wstring detail = ad.driverProvider;
            if (!ad.driverDate.empty()) detail += L"  \u00b7  " + ad.driverDate;
            if (year > 2000 && curYear - year >= 5) {
                i.sev = Sev::Warn;
                i.detail = detail;
                i.advice = L"A driver this old predates fixes for modern Wi-Fi and power states. "
                           L"Stale drivers are a leading cause of intermittent drops.";
                i.steps = {L"Install the vendor's latest driver for this adapter.",
                           L"Prefer the vendor package over Windows Update."};
                i.metric = std::to_wstring(year);
            } else {
                i.sev = Sev::Pass;
                i.detail = detail;
                i.metric = year > 2000 ? std::to_wstring(year) : L"CURRENT";
            }
        }
        add(i);
    }

    // ------------------------------------------------------------ done
    setPhase(L"Scoring results", 0.96f);
    {
        net::Adapter fresh;
        net::detectAdapter(fresh);
        std::lock_guard<std::mutex> lk(mu_);
        st_.adapter = fresh;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        int score = 100;
        int pass = 0, warn = 0, crit = 0;
        for (auto& i : st_.issues) {
            if (i.sev == Sev::Critical) {
                score -= 15;
                crit++;
            } else if (i.sev == Sev::Warn) {
                score -= 6;
                warn++;
            } else if (i.sev == Sev::Pass) {
                pass++;
            }
        }
        if (!inet.reachable) score = std::min(score, 30);
        if (!https.ok) score = std::min(score, 40);
        score = std::max(0, std::min(100, score));
        st_.score = score;
        st_.nPass = pass;
        st_.nWarn = warn;
        st_.nCrit = crit;
        st_.scanning = false;
        st_.progress = 1.f;
        st_.phase = L"Diagnostics complete";
        st_.hasResult = true;
        if (crit > 0)
            st_.headline = L"Connection problems detected";
        else if (warn > 0)
            st_.headline = L"Usable, with warnings";
        else
            st_.headline = L"Connection is healthy";
        SYSTEMTIME stt{};
        GetLocalTime(&stt);
        wchar_t b[64];
        swprintf(b, 64, L"%02d:%02d", stt.wHour, stt.wMinute);
        st_.scannedAt = b;

        st_.metrics.clear();
        Metric m;
        m.label = L"Latency";
        m.value = inet.reachable ? net::fmt0(inet.avgMs) : L"\u2014";
        m.unit = L"ms";
        m.sev = !inet.reachable ? Sev::Critical
                                : inet.avgMs > 120 ? Sev::Warn : Sev::Pass;
        m.raw = inet.avgMs;
        st_.metrics.push_back(m);

        m = Metric{};
        m.label = L"Jitter";
        m.value = inet.reachable ? net::fmt0(inet.jitterMs) : L"\u2014";
        m.unit = L"ms";
        m.sev = !inet.reachable ? Sev::Critical : inet.jitterMs > 30 ? Sev::Warn : Sev::Pass;
        m.raw = inet.jitterMs;
        st_.metrics.push_back(m);

        m = Metric{};
        m.label = L"Loss";
        m.value = inet.reachable ? std::to_wstring(inet.lossPct) : L"100";
        m.unit = L"%";
        m.sev = inet.lossPct >= 10 ? Sev::Critical : inet.lossPct > 0 ? Sev::Warn : Sev::Pass;
        m.raw = inet.lossPct;
        st_.metrics.push_back(m);

        m = Metric{};
        m.label = L"DNS";
        m.value = dnsBest >= 0 ? net::fmt0(dnsBest) : L"\u2014";
        m.unit = L"ms";
        m.sev = dnsBest < 0 ? Sev::Critical : dnsBest > 200 ? Sev::Warn : Sev::Pass;
        m.raw = dnsBest;
        st_.metrics.push_back(m);

        m = Metric{};
        m.label = L"Download";
        m.value = dl.ok && dl.mbps > 0 ? net::fmt1(dl.mbps) : L"\u2014";
        m.unit = L"Mbps";
        m.sev = !dl.ok ? Sev::Info : dl.mbps < 10 ? Sev::Warn : Sev::Pass;
        m.raw = dl.mbps;
        st_.metrics.push_back(m);

    }

    int finalScore = 0, finalCrit = 0, finalWarn = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        finalScore = st_.score;
        finalCrit = st_.nCrit;
        finalWarn = st_.nWarn;
    }
    log(L"Diagnostics finished \u2014 score " + std::to_wstring(finalScore) + L"/100, " +
            std::to_wstring(finalCrit) + L" critical, " + std::to_wstring(finalWarn) +
            L" warnings",
        finalCrit > 0 ? 3 : finalWarn > 0 ? 2 : 1);
}
