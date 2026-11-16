// ============================================================================
//  config.h — DNS provider catalogue and persisted application settings.
// ============================================================================

#pragma once
#include <string>
#include <filesystem>

namespace dzp {

// A selectable DNS configuration. `Primary`/`Secondary` are empty for the
// "system" and "custom" entries (they carry no fixed addresses).
struct Provider {
    const wchar_t* Id;        // stable identifier stored in the settings file
    const wchar_t* Title;     // full description shown in the list
    const wchar_t* Short;     // concise label used on the status card
    const wchar_t* Primary;   // preferred DNS server
    const wchar_t* Secondary; // alternate DNS server (may be empty)
};

namespace provider {
inline constexpr wchar_t kSystem[] = L"system";
inline constexpr wchar_t kCustom[] = L"custom";

inline constexpr Provider kAll[] = {
    { kSystem,     L"Use my current system DNS settings (protect as-is)", L"System settings", L"", L"" },
    { kCustom,     L"Use custom DNS server addresses (enter below)",      L"Custom servers",  L"", L"" },
    { L"google",     L"Google Public DNS   8.8.8.8 / 8.8.4.4",             L"Google Public DNS",         L"8.8.8.8",        L"8.8.4.4" },
    { L"cloudflare", L"Cloudflare Public DNS   1.1.1.1 / 1.0.0.1",         L"Cloudflare Public DNS",     L"1.1.1.1",        L"1.0.0.1" },
    { L"opendns",    L"OpenDNS   208.67.222.222 / 208.67.220.220",         L"OpenDNS",                   L"208.67.222.222", L"208.67.220.220" },
    { L"quad9",      L"Quad9 Secure DNS   9.9.9.9 / 149.112.112.112",      L"Quad9 Secure DNS",          L"9.9.9.9",        L"149.112.112.112" },
    { L"comodo",     L"Comodo Secure DNS   8.26.56.26 / 8.20.247.20",      L"Comodo Secure DNS",         L"8.26.56.26",     L"8.20.247.20" },
    { L"verizon",    L"Verizon Public DNS   4.2.2.1 / 4.2.2.2",            L"Verizon Public DNS",        L"4.2.2.1",        L"4.2.2.2" },
    { L"advantage",  L"DNS Advantage   156.154.70.1 / 156.154.71.1",       L"DNS Advantage",             L"156.154.70.1",   L"156.154.71.1" },
};
inline constexpr int kCount = static_cast<int>(std::size(kAll));

// Null when `id` is `kSystem` or `kCustom`.
const Provider* ById(const std::wstring& id);
int             IndexOf(const std::wstring& id);
}  // namespace provider

// ---------------------------------------------------------------------------
// Persisted settings (dnsprotect.ini under %ProgramData%\DNSProtect).
// ---------------------------------------------------------------------------

struct Config {
    bool         Enabled  = false;
    std::wstring Provider = provider::kSystem;
    std::wstring Primary;      // for the "custom" provider
    std::wstring Secondary;
};

// Prepared data locations handed to the rest of the application.
struct DataDirs {
    std::filesystem::path data;   // %ProgramData%\DNSProtect
    std::filesystem::path ini;    // data / dnsprotect.ini
};

// Resolves (and creates) the per-machine data directory.
DataDirs ResolveDataDirs();

Config LoadConfig(const DataDirs& dirs);
void   SaveConfig(const DataDirs& dirs, const Config& cfg);

}  // namespace dzp
