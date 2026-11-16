// dns.cpp - the DNS Protect engine: read every adapter's DNS, enforce a provider
// on all of them, put the machine back on DHCP DNS, and snap back drift.
#include "extras.h"

#include <algorithm>
#include <cctype>

#include "core.h"

namespace {

const wchar_t* kInterfacesKey =
    L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces";
const wchar_t* kNetworkKey =
    L"SYSTEM\\CurrentControlSet\\Control\\Network\\"
    L"{4D36E972-E325-11CE-BFC1-08002BE10318}";

std::wstring Trim(const std::wstring& text) {
    size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::wstring Normalize(std::wstring servers) {
    // "8.8.8.8, 8.8.4.4" and "8.8.8.8,8.8.4.4" must compare equal.
    std::wstring out;
    for (wchar_t c : servers) {
        if (c == L' ' || c == L'\t') continue;
        out.push_back(c);
    }
    return out;
}

bool ReadText(HKEY root, const std::wstring& key, const std::wstring& name, std::wstring& out) {
    RegValue value;
    std::wstring error;
    if (!ReadRegValue(root, key, name, 0, value, error)) {
        out.clear();
        return false;
    }
    out = Trim(value.data);
    return !out.empty();
}

std::wstring FriendlyName(const std::wstring& guid) {
    std::wstring name;
    if (ReadText(HKEY_LOCAL_MACHINE, std::wstring(kNetworkKey) + L"\\" + guid + L"\\Connection",
                 L"Name", name))
        return name;
    return guid;
}

bool HasConfiguration(const std::wstring& key, const std::wstring& override,
                      const std::wstring& dhcpServers, std::wstring& ip) {
    if (!override.empty() || !dhcpServers.empty()) {
        if (ip.empty()) ReadText(HKEY_LOCAL_MACHINE, key, L"DhcpIPAddress", ip);
        return true;
    }
    if (!ReadText(HKEY_LOCAL_MACHINE, key, L"DhcpIPAddress", ip)) return false;
    return Normalize(ip) != L"0.0.0.0";
}

void FlushDnsCache() {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    wchar_t cmd[MAX_PATH * 2] = {};
    GetSystemDirectoryW(cmd, MAX_PATH);
    std::wstring command = std::wstring(cmd) + L"\\ipconfig.exe /flushdns";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    if (CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 4000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

bool WriteServers(const std::wstring& guid, const std::wstring& servers, std::wstring& error) {
    std::wstring key = std::wstring(kInterfacesKey) + L"\\" + guid;
    if (servers.empty())
        return DeleteRegValue(HKEY_LOCAL_MACHINE, key, L"NameServer", 0, error);
    RegValue value;
    value.type = REG_SZ;
    value.data = servers;
    return WriteRegValue(HKEY_LOCAL_MACHINE, key, L"NameServer", 0, value, error);
}

std::wstring ReverseProviderServers(const DnsProvider& provider) {
    if (!provider.primary || !*provider.primary) return L"";
    std::wstring servers = provider.primary;
    if (provider.secondary && *provider.secondary) {
        servers += L",";
        servers += provider.secondary;
    }
    return servers;
}

}  // namespace

const std::vector<DnsProvider>& DnsProviders() {
    static const std::vector<DnsProvider> providers = {
        {L"system", L"Automatic (system)", L"", L""},
        {L"google", L"Google", L"8.8.8.8", L"8.8.4.4"},
        {L"cloudflare", L"Cloudflare", L"1.1.1.1", L"1.0.0.1"},
        {L"opendns", L"OpenDNS", L"208.67.222.222", L"208.67.220.220"},
        {L"quad9", L"Quad9", L"9.9.9.9", L"149.112.112.112"},
    };
    return providers;
}

std::vector<DnsAdapter> LoadDnsAdapters() {
    std::vector<DnsAdapter> adapters;
    HKEY interfaces = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kInterfacesKey, 0, KEY_READ | KEY_WOW64_64KEY,
                      &interfaces) != ERROR_SUCCESS)
        return adapters;

    for (DWORD index = 0;; ++index) {
        wchar_t guid[256] = {};
        DWORD size = 255;
        LONG rc = RegEnumKeyExW(interfaces, index, guid, &size, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;

        std::wstring key = std::wstring(kInterfacesKey) + L"\\" + guid;
        std::wstring overrideServers;
        std::wstring dhcpServers;
        ReadText(HKEY_LOCAL_MACHINE, key, L"NameServer", overrideServers);
        ReadText(HKEY_LOCAL_MACHINE, key, L"DhcpNameServer", dhcpServers);

        std::wstring ip;
        if (!HasConfiguration(key, overrideServers, dhcpServers, ip)) continue;
        if (Normalize(ip) == L"0.0.0.0") continue;

        DnsAdapter adapter;
        adapter.guid = guid;
        adapter.name = FriendlyName(guid);
        adapter.override = overrideServers;
        if (!overrideServers.empty()) {
            adapter.current = overrideServers;
        } else if (!dhcpServers.empty()) {
            adapter.current = L"Automatic (DHCP " + dhcpServers + L")";
        } else {
            adapter.current = L"Automatic";
        }
        adapters.push_back(adapter);
    }
    RegCloseKey(interfaces);
    return adapters;
}

bool ApplyDns(const std::vector<DnsAdapter>& adapters, const DnsProvider& provider,
              std::wstring& error) {
    error.clear();
    if (adapters.empty()) {
        error = L"No configured network adapter was found.";
        return false;
    }
    const std::wstring servers = ReverseProviderServers(provider);
    for (const auto& adapter : adapters) {
        std::wstring failure;
        if (!WriteServers(adapter.guid, servers, failure) && error.empty())
            error = adapter.name + L": " + failure;
    }
    FlushDnsCache();
    return error.empty();
}

bool RestoreSystemDns(const std::vector<DnsAdapter>& adapters, std::wstring& error) {
    error.clear();
    for (const auto& adapter : adapters) {
        std::wstring failure;
        if (!WriteServers(adapter.guid, L"", failure) && error.empty())
            error = adapter.name + L": " + failure;
    }
    FlushDnsCache();
    return error.empty();
}

uint32_t DnsWatchdogTick(const std::wstring& primary, const std::wstring& secondary) {
    if (primary.empty()) return 0;
    std::wstring expected = secondary.empty() ? primary : primary + L"," + secondary;
    uint32_t changed = 0;
    for (const auto& adapter : LoadDnsAdapters()) {
        std::wstring key = std::wstring(kInterfacesKey) + L"\\" + adapter.guid;
        std::wstring currentServers;
        ReadText(HKEY_LOCAL_MACHINE, key, L"NameServer", currentServers);
        if (Normalize(currentServers) == Normalize(expected)) continue;
        std::wstring ignored;
        if (WriteServers(adapter.guid, expected, ignored)) ++changed;
    }
    if (changed > 0) FlushDnsCache();
    return changed;
}
