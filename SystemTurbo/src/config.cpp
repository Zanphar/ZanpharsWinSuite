// ============================================================================
//  config.cpp — INI-backed settings and boost restore bookkeeping.
// ============================================================================

#include "config.h"
#include "common.h"
#include "log.h"

namespace stc {

namespace {

// Encodes one restore line. Records never contain '|', so it is a safe
// separator for the whitelisted tweak targets this app writes.
std::wstring Escape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) out.push_back(c == L'|' ? L'¦' : c);
    return out;
}

}  // namespace

DataDirs ResolveDataDirs() {
    DataDirs dirs;
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &raw))) {
        dirs.data = std::filesystem::path(raw) / L"SystemTurbo";
        CoTaskMemFree(raw);
    } else {
        // Last-resort fallback: local temp directory.
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        dirs.data = std::filesystem::path(tmp) / L"SystemTurbo";
    }
    std::error_code ec;
    std::filesystem::create_directories(dirs.data, ec);
    if (ec) log::Warn(Fmt(L"Could not create data directory: %ls", ec.message().c_str()));
    dirs.ini = dirs.data / L"systemturbo.ini";
    return dirs;
}

// ---------------------------------------------------------------------------
// User options
// ---------------------------------------------------------------------------

Config LoadConfig(const DataDirs& dirs) {
    Config cfg;
    cfg.TweakMask = static_cast<unsigned>(
        GetPrivateProfileIntW(L"Options", L"TweakMask", 0, dirs.ini.c_str()));
    cfg.AutoRelease = GetPrivateProfileIntW(L"Options", L"AutoRelease", 0,
                                            dirs.ini.c_str()) != 0;
    return cfg;
}

void SaveConfig(const DataDirs& dirs, const Config& cfg) {
    const wchar_t* ini = dirs.ini.c_str();
    WritePrivateProfileStringW(L"Options", L"TweakMask",
                               Fmt(L"%u", cfg.TweakMask).c_str(), ini);
    WritePrivateProfileStringW(L"Options", L"AutoRelease",
                               cfg.AutoRelease ? L"1" : L"0", ini);
}

// ---------------------------------------------------------------------------
// Boost state
// ---------------------------------------------------------------------------

BoostRestore LoadBoost(const DataDirs& dirs) {
    BoostRestore br;
    const wchar_t* ini = dirs.ini.c_str();

    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(L"Boost", L"PowerPlan", L"", buf, 1024, ini);
    br.PowerPlan = buf;

    const int svcCount = GetPrivateProfileIntW(L"Boost", L"ServiceCount", 0, ini);
    for (int i = 0; i < svcCount; ++i) {
        GetPrivateProfileStringW(L"Boost", Fmt(L"Service%d", i).c_str(), L"",
                                 buf, 1024, ini);
        if (!buf[0]) continue;
        std::wstring line = buf;
        std::vector<std::wstring> parts;
        size_t pos = 0;
        while (true) {
            const size_t delim = line.find(L'|', pos);
            parts.push_back(line.substr(pos, delim == std::wstring::npos
                                                ? std::wstring::npos
                                                : delim - pos));
            if (delim == std::wstring::npos) break;
            pos = delim + 1;
        }
        if (parts.size() < 3) continue;
        ServiceRecord rec;
        rec.Name       = parts[0];
        rec.StartType  = wcstoul(parts[1].c_str(), nullptr, 10);
        rec.WasRunning = parts[2] == L"1";
        br.Services.push_back(rec);
    }

    const int regCount = GetPrivateProfileIntW(L"Boost", L"RegCount", 0, ini);
    for (int i = 0; i < regCount; ++i) {
        GetPrivateProfileStringW(L"Boost", Fmt(L"Reg%d", i).c_str(), L"",
                                 buf, 1024, ini);
        if (!buf[0]) continue;
        std::wstring line = buf;
        std::vector<std::wstring> parts;
        size_t pos = 0;
        while (true) {
            const size_t delim = line.find(L'|', pos);
            parts.push_back(line.substr(pos, delim == std::wstring::npos
                                                ? std::wstring::npos
                                                : delim - pos));
            if (delim == std::wstring::npos) break;
            pos = delim + 1;
        }
        if (parts.size() < 5) continue;
        RegRecord rec;
        rec.Key     = parts[0];
        rec.Value   = parts[1];
        rec.Type    = wcstoul(parts[2].c_str(), nullptr, 10);
        rec.Data    = parts[3];
        rec.Existed = parts[4] == L"1";
        br.Regs.push_back(rec);
    }
    return br;
}

void SaveBoost(const DataDirs& dirs, const BoostRestore& br) {
    const wchar_t* ini = dirs.ini.c_str();
    WritePrivateProfileStringW(L"Boost", L"PowerPlan",
                               Escape(br.PowerPlan).c_str(), ini);
    WritePrivateProfileStringW(L"Boost", L"ServiceCount",
                               Fmt(L"%d", (int)br.Services.size()).c_str(), ini);
    for (size_t i = 0; i < br.Services.size(); ++i) {
        const ServiceRecord& r = br.Services[i];
        const std::wstring line =
            Escape(r.Name) + L"|" + Fmt(L"%lu", r.StartType) + L"|" +
            (r.WasRunning ? L"1" : L"0");
        WritePrivateProfileStringW(L"Boost", Fmt(L"Service%d", (int)i).c_str(),
                                   line.c_str(), ini);
    }
    WritePrivateProfileStringW(L"Boost", L"RegCount",
                               Fmt(L"%d", (int)br.Regs.size()).c_str(), ini);
    for (size_t i = 0; i < br.Regs.size(); ++i) {
        const RegRecord& r = br.Regs[i];
        const std::wstring line =
            Escape(r.Key) + L"|" + Escape(r.Value) + L"|" +
            Fmt(L"%lu", r.Type) + L"|" + Escape(r.Data) + L"|" +
            (r.Existed ? L"1" : L"0");
        WritePrivateProfileStringW(L"Boost", Fmt(L"Reg%d", (int)i).c_str(),
                                   line.c_str(), ini);
    }
}

void ClearBoost(const DataDirs& dirs) {
    // Remove the whole [Boost] section: set known keys, then the counters.
    WritePrivateProfileStringW(L"Boost", nullptr, nullptr, dirs.ini.c_str());
}

bool LoadBoostActive(const DataDirs& dirs) {
    return GetPrivateProfileIntW(L"Boost", L"Active", 0, dirs.ini.c_str()) != 0;
}

void SaveBoostActive(const DataDirs& dirs, bool active) {
    WritePrivateProfileStringW(L"Boost", L"Active", active ? L"1" : L"0",
                               dirs.ini.c_str());
}

unsigned long long LoadLastFreed(const DataDirs& dirs) {
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"Boost", L"LastFreed", L"0", buf, 64,
                             dirs.ini.c_str());
    return _wcstoui64(buf, nullptr, 10);
}

void SaveLastFreed(const DataDirs& dirs, unsigned long long bytes) {
    WritePrivateProfileStringW(L"Boost", L"LastFreed",
                               Fmt(L"%llu", bytes).c_str(), dirs.ini.c_str());
}

}  // namespace stc
