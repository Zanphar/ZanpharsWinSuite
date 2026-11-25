// ============================================================================
//  tuning.cpp — adapters, the tuning values, the apply/restore engine and the
//  self-test.
// ============================================================================

// winsock2.h must come before windows.h, and iphlpapi.h needs winsock2 for
// GetAdaptersAddresses; tuning.h's windows.h include is guarded, so this order
// is safe and gives the adapter types their definitions.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "tuning.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

namespace itw {
namespace {

constexpr DWORD kReadAccess = KEY_READ | KEY_WOW64_64KEY;
// Creating a key needs create rights on the parent as well as set rights on the
// new key; the .NET-free path is otherwise Access denied on a fresh key.
constexpr DWORD kWriteAccess = KEY_SET_VALUE | KEY_CREATE_SUB_KEY | KEY_WOW64_64KEY;

constexpr wchar_t kThrottleKey[] =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile";
constexpr wchar_t kThrottleValue[] = L"NetworkThrottlingIndex";
constexpr DWORD kThrottleOff = 0xFFFFFFFFu;

constexpr wchar_t kIpv6Key[] = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters";
constexpr wchar_t kIpv6Value[] = L"DisabledComponents";
constexpr DWORD kIpv6Off = 0xFF;

constexpr wchar_t kSchedulerValue[] = L"SystemResponsiveness";
constexpr DWORD kSchedulerNone = 0;

constexpr wchar_t kTcpipParametersKey[] = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters";
constexpr wchar_t kTimeWaitValue[] = L"TcpTimedWaitDelay";
constexpr DWORD kTimeWaitSeconds = 30;

constexpr wchar_t kInterfacesKey[] =
    L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\";

// Pre-apply copies of the adapter's own DNS values, kept so Restore can put the
// DHCP-provided list back instead of leaving the adapter without one.
constexpr wchar_t kDnsBackupRoot[] =
    L"SOFTWARE\\Zanphars WinSuite\\InternetTuningWizard\\Adapters";

constexpr wchar_t kSelftestKey[] =
    L"Software\\ZanpharsWinSuite\\InternetTuningWizard\\selftest";

std::wstring Describe(LSTATUS status) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(status), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text;
    if (length > 0 && buffer) {
        text.assign(buffer, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
            text.pop_back();
    }
    if (buffer) LocalFree(buffer);
    if (text.empty()) text = L"Windows error " + std::to_wstring(status);
    return text;
}

// ---------------------------------------------------------------------------
//  Registry primitives
// ---------------------------------------------------------------------------

bool ReadDword(HKEY root, const std::wstring& key, const wchar_t* name, DWORD* out) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, kReadAccess, &handle) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(sizeof(DWORD));
    DWORD data = 0;
    const LSTATUS status =
        RegQueryValueExW(handle, name, nullptr, &type, reinterpret_cast<BYTE*>(&data), &size);
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS || type != REG_DWORD || size < sizeof(DWORD)) return false;
    *out = data;
    return true;
}

bool ReadString(HKEY root, const std::wstring& key, const wchar_t* name, std::wstring* out) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, kReadAccess, &handle) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = 0;
    LSTATUS status = RegQueryValueExW(handle, name, nullptr, &type, nullptr, &size);
    if (status != ERROR_SUCCESS || type != REG_SZ || size < sizeof(wchar_t)) {
        RegCloseKey(handle);
        return false;
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(handle, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()),
                              &size);
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS) return false;
    out->assign(buffer.data());
    return true;
}

bool WriteDword(HKEY root, const std::wstring& key, const wchar_t* name, DWORD value,
                std::wstring* error) {
    HKEY handle = nullptr;
    const LSTATUS opened =
        RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, kWriteAccess, nullptr, &handle, nullptr);
    if (opened != ERROR_SUCCESS) {
        if (error) *error = Describe(opened);
        return false;
    }
    const LSTATUS status = RegSetValueExW(handle, name, 0, REG_DWORD,
                                          reinterpret_cast<const BYTE*>(&value), sizeof value);
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS) {
        if (error) *error = Describe(status);
        return false;
    }
    return true;
}

bool WriteString(HKEY root, const std::wstring& key, const wchar_t* name,
                 const std::wstring& value, std::wstring* error) {
    HKEY handle = nullptr;
    const LSTATUS opened =
        RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, kWriteAccess, nullptr, &handle, nullptr);
    if (opened != ERROR_SUCCESS) {
        if (error) *error = Describe(opened);
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = RegSetValueExW(handle, name, 0, REG_SZ,
                                          reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS) {
        if (error) *error = Describe(status);
        return false;
    }
    return true;
}

// Missing key or value counts as success: the default is what we want back.
bool DeleteValue(HKEY root, const std::wstring& key, const wchar_t* name, std::wstring* error) {
    HKEY handle = nullptr;
    const LSTATUS opened = RegOpenKeyExW(root, key.c_str(), 0, kWriteAccess, &handle);
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return true;
    if (opened != ERROR_SUCCESS) {
        if (error) *error = Describe(opened);
        return false;
    }
    const LSTATUS status = RegDeleteValueW(handle, name);
    RegCloseKey(handle);
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) return true;
    if (error) *error = Describe(status);
    return false;
}

std::wstring InterfaceKey(const std::wstring& guid) {
    return std::wstring(kInterfacesKey) + guid;
}

std::wstring DnsBackupKey(const std::wstring& guid) {
    return std::wstring(kDnsBackupRoot) + L"\\" + guid;
}

// Windows keeps a manual list in NameServer and the DHCP-provided list in
// DhcpNameServer, preferring NameServer. A DHCP adapter that reconnects can be
// repopulated from the DHCP side (and some Wi-Fi profiles keep a cached
// ProfileNameServer), so the wizard writes both and remembers what was there.
bool SaveDnsOriginals(HKEY root, const std::wstring& interfaceKey,
                      const std::wstring& backupKey, std::wstring* error) {
    DWORD saved = 0;
    if (ReadDword(root, backupKey, L"Saved", &saved) && saved == 1) return true;

    std::wstring nameServer;
    std::wstring dhcpNameServer;
    const bool hadName = ReadString(root, interfaceKey, L"NameServer", &nameServer);
    const bool hadDhcp = ReadString(root, interfaceKey, L"DhcpNameServer", &dhcpNameServer);

    bool ok = WriteDword(root, backupKey, L"Saved", 1, error);
    if (ok) ok = WriteDword(root, backupKey, L"HadNameServer", hadName ? 1 : 0, error);
    if (ok) ok = WriteDword(root, backupKey, L"HadDhcpNameServer", hadDhcp ? 1 : 0, error);
    if (ok && hadName) ok = WriteString(root, backupKey, L"NameServer", nameServer, error);
    if (ok && hadDhcp) ok = WriteString(root, backupKey, L"DhcpNameServer", dhcpNameServer, error);
    return ok;
}

// Puts both values back exactly as they were before the first apply. With no
// backup there is nothing of ours to put back: a restore clears the static and
// DHCP-side values so Windows learns them again, while a plain apply with DNS
// left on automatic only clears the static override.
bool RestoreDnsValues(HKEY root, const std::wstring& interfaceKey,
                      const std::wstring& backupKey, bool restoreDefaults,
                      bool* restoredPrevious, std::wstring* error) {
    if (restoredPrevious) *restoredPrevious = false;
    DWORD saved = 0;
    const bool haveBackup = ReadDword(root, backupKey, L"Saved", &saved) && saved == 1;
    if (!haveBackup) {
        bool ok = DeleteValue(root, interfaceKey, L"NameServer", error);
        if (restoreDefaults) ok = DeleteValue(root, interfaceKey, L"DhcpNameServer", error) && ok;
        return ok;
    }

    if (restoredPrevious) *restoredPrevious = true;
    bool ok = true;
    DWORD had = 0;
    std::wstring text;
    if (ReadDword(root, backupKey, L"HadNameServer", &had) && had == 1 &&
        ReadString(root, backupKey, L"NameServer", &text)) {
        ok = WriteString(root, interfaceKey, L"NameServer", text, error) && ok;
    } else {
        ok = DeleteValue(root, interfaceKey, L"NameServer", error) && ok;
    }
    text.clear();
    if (ReadDword(root, backupKey, L"HadDhcpNameServer", &had) && had == 1 &&
        ReadString(root, backupKey, L"DhcpNameServer", &text)) {
        ok = WriteString(root, interfaceKey, L"DhcpNameServer", text, error) && ok;
    } else {
        ok = DeleteValue(root, interfaceKey, L"DhcpNameServer", error) && ok;
    }

    const LSTATUS removed = RegDeleteTreeW(root, backupKey.c_str());
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
        if (error) *error = Describe(removed);
        ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
//  Adapters
// ---------------------------------------------------------------------------

std::wstring Wide(const char* text) {
    if (!text) return std::wstring();
    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (length <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), length);
    return out;
}

std::wstring SockaddrText(const SOCKET_ADDRESS& address) {
    if (!address.lpSockaddr || address.iSockaddrLength == 0) return std::wstring();
    wchar_t host[64] = {};
    if (GetNameInfoW(address.lpSockaddr, address.iSockaddrLength, host, 64, nullptr, 0,
                     NI_NUMERICHOST) != 0)
        return std::wstring();
    return host;
}

std::vector<Adapter> Enumerate(bool onlyUp) {
    std::vector<Adapter> out;
    ULONG size = 24 * 1024;
    std::vector<BYTE> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR) return out;

    for (auto* entry = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); entry;
         entry = entry->Next) {
        if (entry->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (!entry->FriendlyName || !*entry->FriendlyName) continue;
        if (onlyUp && entry->OperStatus != IfOperStatusUp) continue;

        Adapter adapter;
        adapter.guid = Wide(entry->AdapterName);
        if (adapter.guid.empty()) continue;
        adapter.name = entry->FriendlyName;
        if (entry->Description && *entry->Description) adapter.model = entry->Description;
        adapter.wifi = entry->IfType == IF_TYPE_IEEE80211;
        // Every connected adapter is listed, including VPN/tunnel adapters and
        // Hyper-V switches; the kind only decides the label on the row.
        if (adapter.wifi) adapter.kind = L"Wi-Fi";
        else if (entry->IfType == IF_TYPE_ETHERNET_CSMACD) adapter.kind = L"Ethernet";
        else if (entry->IfType == IF_TYPE_PPP || entry->IfType == IF_TYPE_TUNNEL)
            adapter.kind = L"VPN";
        else adapter.kind = L"Other";

        for (auto* unicast = entry->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            const sockaddr* sa = unicast->Address.lpSockaddr;
            if (sa && sa->sa_family == AF_INET) {
                adapter.address = SockaddrText(unicast->Address);
                if (!adapter.address.empty()) break;
            }
        }
        for (auto* dns = entry->FirstDnsServerAddress; dns; dns = dns->Next) {
            const std::wstring server = SockaddrText(dns->Address);
            if (!server.empty()) adapter.dns.push_back(server);
        }

        std::wstring configured;
        if (ReadString(HKEY_LOCAL_MACHINE, InterfaceKey(adapter.guid), L"NameServer",
                       &configured) &&
            !configured.empty()) {
            adapter.dns.clear();
            size_t at = 0;
            while (at <= configured.size()) {
                const size_t comma = configured.find(L',', at);
                std::wstring one = configured.substr(
                    at, comma == std::wstring::npos ? std::wstring::npos : comma - at);
                while (!one.empty() && one.front() == L' ') one.erase(one.begin());
                while (!one.empty() && one.back() == L' ') one.pop_back();
                if (!one.empty()) adapter.dns.push_back(one);
                if (comma == std::wstring::npos) break;
                at = comma + 1;
            }
        }

        out.push_back(adapter);
    }

    std::stable_sort(out.begin(), out.end(), [](const Adapter& a, const Adapter& b) {
        if (a.wifi != b.wifi) return !a.wifi;
        return a.name < b.name;
    });
    return out;
}

}  // namespace

std::vector<Adapter> Adapters() {
    std::vector<Adapter> list = Enumerate(true);
    if (list.empty()) list = Enumerate(false);
    return list;
}

const Adapter* FindAdapter(const std::vector<Adapter>& list, const std::wstring& guid) {
    for (const Adapter& adapter : list)
        if (adapter.guid == guid) return &adapter;
    return nullptr;
}

const std::vector<DnsProvider>& DnsProviders() {
    static const std::vector<DnsProvider> providers = {
        {L"System default", L"", L"the servers this network hands out"},
        {L"Cloudflare", L"1.1.1.1,1.0.0.1", L"1.1.1.1 and 1.0.0.1"},
        {L"Google", L"8.8.8.8,8.8.4.4", L"8.8.8.8 and 8.8.4.4"},
        {L"Quad9", L"9.9.9.9,149.112.112.112", L"9.9.9.9 and 149.112.112.112"},
    };
    return providers;
}

// ---------------------------------------------------------------------------
//  Current state
// ---------------------------------------------------------------------------

Current ReadState(const std::wstring& adapter) {
    Current state;
    DWORD value = 0;
    state.throttle = ReadDword(HKEY_LOCAL_MACHINE, kThrottleKey, kThrottleValue, &value) &&
                     value == kThrottleOff;
    state.ipv6 = ReadDword(HKEY_LOCAL_MACHINE, kIpv6Key, kIpv6Value, &value) && value == kIpv6Off;
    state.scheduler = ReadDword(HKEY_LOCAL_MACHINE, kThrottleKey, kSchedulerValue, &value) &&
                      value == kSchedulerNone;
    state.timeWait = ReadDword(HKEY_LOCAL_MACHINE, kTcpipParametersKey, kTimeWaitValue, &value) &&
                     value == kTimeWaitSeconds;

    if (!adapter.empty()) {
        const std::wstring key = InterfaceKey(adapter);
        DWORD ack = 0;
        DWORD nodelay = 0;
        state.nagle = ReadDword(HKEY_LOCAL_MACHINE, key, L"TcpAckFrequency", &ack) && ack == 1 &&
                      ReadDword(HKEY_LOCAL_MACHINE, key, L"TCPNoDelay", &nodelay) && nodelay == 1;

        std::wstring nameServer;
        // NameServer is the manual list Windows prefers; DhcpNameServer is what
        // a DHCP adapter falls back to, so read it when there is no override.
        if (!ReadString(HKEY_LOCAL_MACHINE, key, L"NameServer", &nameServer) ||
            nameServer.empty()) {
            ReadString(HKEY_LOCAL_MACHINE, key, L"DhcpNameServer", &nameServer);
        }
        if (!nameServer.empty()) {
            const std::vector<DnsProvider>& providers = DnsProviders();
            for (size_t i = 0; i < providers.size(); ++i) {
                if (nameServer == providers[i].servers) {
                    state.dns = static_cast<int>(i);
                    break;
                }
            }
            size_t at = 0;
            while (at <= nameServer.size()) {
                const size_t comma = nameServer.find(L',', at);
                state.servers.push_back(nameServer.substr(
                    at, comma == std::wstring::npos ? std::wstring::npos : comma - at));
                if (comma == std::wstring::npos) break;
                at = comma + 1;
            }
        }
    }
    return state;
}

// ---------------------------------------------------------------------------
//  Apply and restore
// ---------------------------------------------------------------------------

// Writes only the switches the plan selects; a switch left off is left alone.
// Restore sets restoreDefaults, which turns every "off" into a removal.
Report Apply(const Plan& plan) {
    Report report;
    auto record = [&report](bool ok, std::wstring text) {
        report.lines.push_back({ok, std::move(text)});
        if (ok) ++report.changed; else ++report.failed;
    };

    std::wstring error;

    if (plan.throttle) {
        const bool ok = WriteDword(HKEY_LOCAL_MACHINE, kThrottleKey, kThrottleValue, kThrottleOff,
                                   &error);
        record(ok, ok ? L"Network throttling is off for the whole PC."
                      : L"Network throttling: " + error);
    } else if (plan.restoreDefaults) {
        const bool ok = DeleteValue(HKEY_LOCAL_MACHINE, kThrottleKey, kThrottleValue, &error);
        record(ok, ok ? L"Network throttling is back to the Windows default."
                      : L"Network throttling: " + error);
    }

    if (plan.ipv6) {
        const bool ok = WriteDword(HKEY_LOCAL_MACHINE, kIpv6Key, kIpv6Value, kIpv6Off, &error);
        record(ok, ok ? L"IPv6 is off for the whole PC; a restart finishes it."
                      : L"IPv6: " + error);
    } else if (plan.restoreDefaults) {
        const bool ok = DeleteValue(HKEY_LOCAL_MACHINE, kIpv6Key, kIpv6Value, &error);
        record(ok, ok ? L"IPv6 is back to the Windows default."
                      : L"IPv6: " + error);
    }

    if (plan.scheduler) {
        const bool ok =
            WriteDword(HKEY_LOCAL_MACHINE, kThrottleKey, kSchedulerValue, kSchedulerNone, &error);
        record(ok, ok ? L"The multimedia scheduler no longer reserves a CPU share."
                      : L"Multimedia reserve: " + error);
    } else if (plan.restoreDefaults) {
        const bool ok = DeleteValue(HKEY_LOCAL_MACHINE, kThrottleKey, kSchedulerValue, &error);
        record(ok, ok ? L"The multimedia CPU reserve is back to the Windows default."
                      : L"Multimedia reserve: " + error);
    }

    if (plan.timeWait) {
        const bool ok = WriteDword(HKEY_LOCAL_MACHINE, kTcpipParametersKey, kTimeWaitValue,
                                   kTimeWaitSeconds, &error);
        record(ok, ok ? L"Closed connections are released after 30 seconds, not two minutes."
                      : L"TIME_WAIT: " + error);
    } else if (plan.restoreDefaults) {
        const bool ok = DeleteValue(HKEY_LOCAL_MACHINE, kTcpipParametersKey, kTimeWaitValue, &error);
        record(ok, ok ? L"TIME_WAIT is back to the Windows default." : L"TIME_WAIT: " + error);
    }

    if (!plan.adapter.empty()) {
        const std::wstring key = InterfaceKey(plan.adapter);
        if (plan.nagle) {
            const bool ok = WriteDword(HKEY_LOCAL_MACHINE, key, L"TcpAckFrequency", 1, &error) &&
                            WriteDword(HKEY_LOCAL_MACHINE, key, L"TCPNoDelay", 1, &error) &&
                            WriteDword(HKEY_LOCAL_MACHINE, key, L"TcpDelAckTicks", 0, &error);
            record(ok, ok ? L"Nagle and delayed ACK are off on this connection."
                          : L"Nagle: " + error);
        } else if (plan.restoreDefaults) {
            const bool ok =
                DeleteValue(HKEY_LOCAL_MACHINE, key, L"TcpAckFrequency", &error) &&
                DeleteValue(HKEY_LOCAL_MACHINE, key, L"TCPNoDelay", &error) &&
                DeleteValue(HKEY_LOCAL_MACHINE, key, L"TcpDelAckTicks", &error);
            record(ok, ok ? L"Nagle is back to the Windows default."
                          : L"Nagle: " + error);
        }

        const std::vector<DnsProvider>& providers = DnsProviders();
        const std::wstring backup = DnsBackupKey(plan.adapter);
        if (plan.dns > 0 && plan.dns < static_cast<int>(providers.size())) {
            const std::wstring value = providers[static_cast<size_t>(plan.dns)].servers;
            bool ok = SaveDnsOriginals(HKEY_LOCAL_MACHINE, key, backup, &error);
            // Both sides: NameServer wins while it is there, DhcpNameServer keeps
            // the choice if Windows repopulates the adapter from its lease.
            if (ok) ok = WriteString(HKEY_LOCAL_MACHINE, key, L"NameServer", value, &error);
            if (ok) ok = WriteString(HKEY_LOCAL_MACHINE, key, L"DhcpNameServer", value, &error);
            record(ok, ok ? std::wstring(L"DNS is set to ") + providers[static_cast<size_t>(plan.dns)].label +
                                L" on this connection, including its DHCP value; it takes effect "
                                L"the next time it connects."
                          : L"DNS: " + error);
        } else {
            bool restoredPrevious = false;
            const bool ok = RestoreDnsValues(HKEY_LOCAL_MACHINE, key, backup,
                                             plan.restoreDefaults, &restoredPrevious, &error);
            // A saved pair means the adapter goes back to what it had (often a
            // static server list), not to automatic; saying otherwise sends the
            // reader looking for a DHCP address that is not there.
            const std::wstring text =
                restoredPrevious
                    ? L"DNS on this connection is back to how it was before the wizard changed it."
                    : L"DNS is back to automatic on this connection.";
            record(ok, ok ? text : L"DNS: " + error);
        }
    }

    FlushDns();
    return report;
}

Report Restore(const std::wstring& adapter) {
    Plan plan;   // every flag false, every value removed
    plan.adapter = adapter;
    plan.restoreDefaults = true;
    return Apply(plan);
}

bool FlushDns() {
    using FlushFn = BOOL(WINAPI*)();
    HMODULE dnsapi = LoadLibraryW(L"dnsapi.dll");
    if (!dnsapi) return false;
    auto flush = reinterpret_cast<FlushFn>(
        reinterpret_cast<void*>(GetProcAddress(dnsapi, "DnsFlushResolverCache")));
    const bool ok = flush && flush() != FALSE;
    FreeLibrary(dnsapi);
    return ok;
}

// ---------------------------------------------------------------------------
//  Plan text
// ---------------------------------------------------------------------------

std::wstring EncodePlan(const Plan& plan) {
    std::wstring text = plan.adapter;
    text += L'|' + std::to_wstring(plan.dns) + L'|' + (plan.nagle ? L"1" : L"0") + L'|' +
            (plan.throttle ? L"1" : L"0") + L'|' + (plan.ipv6 ? L"1" : L"0") + L'|' +
            (plan.scheduler ? L"1" : L"0") + L'|' + (plan.timeWait ? L"1" : L"0");
    return text;
}

bool DecodePlan(const std::wstring& text, Plan* plan) {
    std::vector<std::wstring> parts;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t bar = text.find(L'|', at);
        parts.push_back(text.substr(at, bar == std::wstring::npos ? std::wstring::npos : bar - at));
        if (bar == std::wstring::npos) break;
        at = bar + 1;
    }
    // Five fields is the original handoff; seven adds the two later switches.
    if (parts.size() != 5 && parts.size() != 7) return false;
    plan->adapter = parts[0];
    plan->dns = _wtoi(parts[1].c_str());
    plan->nagle = parts[2] == L"1";
    plan->throttle = parts[3] == L"1";
    plan->ipv6 = parts[4] == L"1";
    plan->scheduler = parts.size() == 7 && parts[5] == L"1";
    plan->timeWait = parts.size() == 7 && parts[6] == L"1";
    if (plan->dns < 0 || plan->dns >= static_cast<int>(DnsProviders().size())) plan->dns = 0;
    return true;
}

std::vector<std::wstring> Describe(const Plan& plan) {
    std::vector<std::wstring> lines;
    if (plan.throttle)
        lines.push_back(L"Network throttling off (whole PC) - media playback stops capping traffic");
    if (plan.ipv6) lines.push_back(L"IPv6 off (whole PC) - needs a restart, some VPNs need IPv6");
    if (plan.scheduler)
        lines.push_back(L"Multimedia CPU reserve off (whole PC) - foreground work gets that share");
    if (plan.timeWait)
        lines.push_back(L"TIME_WAIT capped at 30 seconds (whole PC) - closed ports come back sooner");
    if (!plan.adapter.empty() && plan.nagle)
        lines.push_back(L"Nagle and delayed ACK off on the chosen connection");
    if (!plan.adapter.empty()) {
        const std::vector<DnsProvider>& providers = DnsProviders();
        if (plan.dns > 0 && plan.dns < static_cast<int>(providers.size())) {
            const DnsProvider& provider = providers[static_cast<size_t>(plan.dns)];
            lines.push_back(std::wstring(L"DNS on the chosen connection: ") + provider.label +
                            L" (" + provider.detail + L")");
        } else {
            lines.push_back(
                L"DNS on the chosen connection: automatic - removes this adapter's override");
        }
    }
    return lines;
}

// ---------------------------------------------------------------------------
//  Result file
// ---------------------------------------------------------------------------

std::wstring DefaultResultPath() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + L"InternetTuningWizard-" +
           std::to_wstring(GetCurrentProcessId()) + L".txt";
}

bool WriteResultFile(const std::wstring& path, const Report& report) {
    std::wstring text;
    for (const Line& line : report.lines)
        text += (line.ok ? L"ok\t" : L"fail\t") + line.text + L"\r\n";
    text += L"summary\t" + std::to_wstring(report.changed) + L"\t" +
            std::to_wstring(report.failed) + L"\r\n";

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes,
                        nullptr, nullptr);

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote, nullptr) &&
                    wrote == utf8.size();
    CloseHandle(file);
    return ok;
}

bool ReadResultFile(const std::wstring& path, Report* report) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 256 * 1024) {
        CloseHandle(file);
        return false;
    }
    std::string utf8(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = ReadFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &read, nullptr) &&
                    read == utf8.size();
    CloseHandle(file);
    if (!ok) return false;

    const int chars = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                          nullptr, 0);
    std::wstring text(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), text.data(), chars);

    report->lines.clear();
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find(L'\n', at);
        std::wstring row = text.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
        at = end == std::wstring::npos ? text.size() : end + 1;
        while (!row.empty() && (row.back() == L'\r' || row.back() == L'\n')) row.pop_back();
        if (row.empty()) continue;

        const size_t tab = row.find(L'\t');
        if (tab == std::wstring::npos) continue;
        const std::wstring kind = row.substr(0, tab);
        const std::wstring rest = row.substr(tab + 1);
        if (kind == L"ok" || kind == L"fail") {
            report->lines.push_back({kind == L"ok", rest});
        } else if (kind == L"summary") {
            const size_t second = rest.find(L'\t');
            if (second != std::wstring::npos) {
                report->changed = _wtoi(rest.substr(0, second).c_str());
                report->failed = _wtoi(rest.substr(second + 1).c_str());
            }
        }
    }
    return !report->lines.empty();
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof elevation;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

const wchar_t* CopyrightText() { return L"\u00A9 2026-2027 Charles McDonald"; }
const wchar_t* VersionText() { return L"1.0.0.0"; }

// ---------------------------------------------------------------------------
//  Self-test
// ---------------------------------------------------------------------------

std::vector<TestResult> RunSelfTest() {
    std::vector<TestResult> results;

    // --- the provider table: four entries, unique labels, valid server text.
    {
        std::wstring problem;
        const std::vector<DnsProvider>& providers = DnsProviders();
        if (providers.size() < 4) problem = L"fewer than four providers";
        else if (providers.front().servers == nullptr || *providers.front().servers != L'\0')
            problem = L"the automatic entry carries servers";
        for (size_t i = 1; i < providers.size() && problem.empty(); ++i) {
            const std::wstring servers = providers[i].servers;
            if (servers.find(L'.') == std::wstring::npos)
                problem = std::wstring(L"\\\"") + providers[i].label + L"\\\" has no address";
            for (size_t j = 0; j < i; ++j)
                if (servers == providers[j].servers)
                    problem = std::wstring(L"\\\"") + providers[i].label + L"\\\" repeats another entry";
        }
        results.push_back({L"the DNS provider list is complete and distinct", problem.empty(), problem});
    }

    // --- plan text: what the elevated copy receives must decode to the plan.
    {
        std::wstring problem;
        Plan plan;
        plan.adapter = L"{11111111-2222-3333-4444-555555555555}";
        plan.dns = 2;
        plan.nagle = true;
        plan.throttle = true;
        plan.ipv6 = false;
        Plan back;
        if (!DecodePlan(EncodePlan(plan), &back)) problem = L"the plan did not decode";
        else if (back.adapter != plan.adapter || back.dns != plan.dns || back.nagle != plan.nagle ||
                 back.throttle != plan.throttle || back.ipv6 != plan.ipv6)
            problem = L"the decoded plan differs from the original";
        else if (DecodePlan(L"too|few", &back)) problem = L"a malformed plan was accepted";
        results.push_back({L"a plan survives the trip to the elevated copy", problem.empty(), problem});
    }

    // --- the summary names every change, and nothing when nothing is chosen.
    {
        std::wstring problem;
        Plan plan;
        plan.adapter = L"{x}";
        plan.nagle = true;
        plan.throttle = true;
        plan.ipv6 = true;
        plan.scheduler = true;
        plan.timeWait = true;
        plan.dns = 1;
        const std::vector<std::wstring> lines = Describe(plan);
        Plan quiet;
        quiet.adapter = L"{x}";
        const std::vector<std::wstring> quietLines = Describe(quiet);
        if (lines.size() != 6) problem = L"a full plan described as " + std::to_wstring(lines.size()) + L" lines";
        else if (!Describe(Plan()).empty()) problem = L"an empty plan described something";
        else if (quietLines.size() != 1 || quietLines[0].find(L"removes") == std::wstring::npos)
            problem = L"a plan with nothing switched on did not describe the one removal";
        results.push_back({L"the review lists exactly the changes selected", problem.empty(), problem});
    }

    // --- the registry helpers round-trip on a scratch key, then clean up.
    {
        std::wstring problem;
        std::wstring error;
        const std::wstring key = kSelftestKey;
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ZanpharsWinSuite");
        if (!WriteDword(HKEY_CURRENT_USER, key, L"Number", 0xFFFFFFFFu, &error))
            problem = L"writing a DWORD failed: " + error;
        else if (!WriteString(HKEY_CURRENT_USER, key, L"Text", L"1.1.1.1,1.0.0.1", &error))
            problem = L"writing a string failed: " + error;
        else {
            DWORD number = 0;
            std::wstring text;
            if (!ReadDword(HKEY_CURRENT_USER, key, L"Number", &number) || number != 0xFFFFFFFFu)
                problem = L"the DWORD did not read back";
            else if (!ReadString(HKEY_CURRENT_USER, key, L"Text", &text) || text != L"1.1.1.1,1.0.0.1")
                problem = L"the string did not read back";
            else if (!DeleteValue(HKEY_CURRENT_USER, key, L"Number", &error) ||
                     ReadDword(HKEY_CURRENT_USER, key, L"Number", &number))
                problem = L"deleting the DWORD failed";
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ZanpharsWinSuite");
        results.push_back({L"the registry engine writes, reads and removes values", problem.empty(),
                           problem});
    }

    // --- DNS: both registry sides are written and restore puts the lease back.
    {
        std::wstring problem;
        std::wstring error;
        const std::wstring interfaceKey = std::wstring(kSelftestKey) + L"\\iface";
        const std::wstring backupKey = std::wstring(kSelftestKey) + L"\\backup";
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ZanpharsWinSuite");
        if (!WriteString(HKEY_CURRENT_USER, interfaceKey, L"DhcpNameServer", L"192.168.1.1",
                         &error)) {
            problem = L"seeding the DHCP value failed: " + error;
        } else if (!SaveDnsOriginals(HKEY_CURRENT_USER, interfaceKey, backupKey, &error)) {
            problem = L"saving the originals failed: " + error;
        } else if (!WriteString(HKEY_CURRENT_USER, interfaceKey, L"NameServer",
                                L"1.1.1.1,1.0.0.1", &error) ||
                   !WriteString(HKEY_CURRENT_USER, interfaceKey, L"DhcpNameServer",
                                L"1.1.1.1,1.0.0.1", &error)) {
            problem = L"writing the chosen DNS failed: " + error;
        } else {
            std::wstring nameServer;
            std::wstring dhcpNameServer;
            if (!ReadString(HKEY_CURRENT_USER, interfaceKey, L"NameServer", &nameServer) ||
                nameServer != L"1.1.1.1,1.0.0.1") {
                problem = L"NameServer did not read back";
            } else if (!ReadString(HKEY_CURRENT_USER, interfaceKey, L"DhcpNameServer",
                                   &dhcpNameServer) ||
                       dhcpNameServer != L"1.1.1.1,1.0.0.1") {
                problem = L"the DHCP-side value was not written";
            } else if (!RestoreDnsValues(HKEY_CURRENT_USER, interfaceKey, backupKey, true, nullptr,
                                         &error)) {
                problem = L"restoring the originals failed: " + error;
            } else if (ReadString(HKEY_CURRENT_USER, interfaceKey, L"NameServer", &nameServer)) {
                problem = L"the static value survived the restore";
            } else if (!ReadString(HKEY_CURRENT_USER, interfaceKey, L"DhcpNameServer",
                                   &dhcpNameServer) ||
                       dhcpNameServer != L"192.168.1.1") {
                problem = L"the DHCP-provided value was not put back";
            }
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ZanpharsWinSuite");
        results.push_back({L"the DNS write pins the DHCP value and restore puts the lease back",
                           problem.empty(), problem});
    }

    // --- adapters: at least one, each with a GUID shaped like one.
    {
        std::wstring problem;
        const std::vector<Adapter> list = Adapters();
        if (list.empty()) problem = L"no physical adapter was found";
        for (const Adapter& adapter : list) {
            if (adapter.guid.size() < 3 || adapter.guid.front() != L'{' ||
                adapter.guid.back() != L'}') {
                problem = std::wstring(L"\\\"") + adapter.name + L"\\\" has no GUID";
                break;
            }
        }
        results.push_back({L"this PC reports at least one usable connection", problem.empty(), problem});
    }

    // --- the values themselves are the documented data, not a guess.
    {
        std::wstring problem;
        if (kThrottleOff != 0xFFFFFFFFu) problem = L"the throttling value is not 0xFFFFFFFF";
        else if (kIpv6Off != 0xFF) problem = L"the IPv6 mask is not 0xFF";
        else if (kSchedulerNone != 0) problem = L"the multimedia reserve is not zero";
        else if (kTimeWaitSeconds != 30) problem = L"TIME_WAIT is not 30 seconds";
        results.push_back({L"the tuning values match Microsoft's documented data", problem.empty(),
                           problem});
    }

    return results;
}

}  // namespace itw
