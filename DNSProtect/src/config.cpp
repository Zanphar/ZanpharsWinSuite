// ============================================================================
//  config.cpp
// ============================================================================

#include "config.h"
#include "common.h"
#include "log.h"

namespace dzp {

namespace provider {

const Provider* ById(const std::wstring& id) {
    for (const auto& p : kAll) {
        if (p.Id == id) return &p;
    }
    return nullptr;
}

int IndexOf(const std::wstring& id) {
    for (int i = 0; i < kCount; ++i) {
        if (kAll[i].Id == id) return i;
    }
    return 0;
}

}  // namespace provider

DataDirs ResolveDataDirs() {
    DataDirs dirs;
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &raw))) {
        dirs.data = std::filesystem::path(raw) / L"DNSProtect";
        CoTaskMemFree(raw);
    } else {
        // Last-resort fallback: local temp directory.
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        dirs.data = std::filesystem::path(tmp) / L"DNSProtect";
    }
    std::error_code ec;
    std::filesystem::create_directories(dirs.data, ec);
    if (ec) log::Warn(Fmt(L"Could not create data directory: %ls", ec.message().c_str()));
    dirs.ini = dirs.data / L"dnsprotect.ini";
    return dirs;
}

Config LoadConfig(const DataDirs& dirs) {
    Config cfg;
    const wchar_t* ini = dirs.ini.c_str();
    cfg.Enabled = GetPrivateProfileIntW(L"Protection", L"Enabled", 0, ini) != 0;

    wchar_t buf[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Protection", L"Provider", provider::kSystem, buf, MAX_PATH, ini);
    cfg.Provider = buf;
    if (!provider::ById(cfg.Provider) && cfg.Provider != provider::kCustom) {
        cfg.Provider = provider::kSystem;
    }

    GetPrivateProfileStringW(L"Protection", L"CustomPrimary", L"", buf, MAX_PATH, ini);
    cfg.Primary = buf;
    GetPrivateProfileStringW(L"Protection", L"CustomSecondary", L"", buf, MAX_PATH, ini);
    cfg.Secondary = buf;
    return cfg;
}

void SaveConfig(const DataDirs& dirs, const Config& cfg) {
    const wchar_t* ini = dirs.ini.c_str();
    WritePrivateProfileStringW(L"Protection", L"Enabled", cfg.Enabled ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"Protection", L"Provider", cfg.Provider.c_str(), ini);
    WritePrivateProfileStringW(L"Protection", L"CustomPrimary", cfg.Primary.c_str(), ini);
    WritePrivateProfileStringW(L"Protection", L"CustomSecondary", cfg.Secondary.c_str(), ini);
}

}  // namespace dzp
