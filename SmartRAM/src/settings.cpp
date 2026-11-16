// settings.cpp — configuration persistence.
#include "settings.h"

#include <shlobj.h>

static const wchar_t kSectionWidget[]   = L"widget";
static const wchar_t kSectionBehavior[] = L"behavior";
static const wchar_t kRunKey[]          = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunValue[]        = L"Smart RAM";

std::wstring Settings::iniPath() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        dir = p;
        CoTaskMemFree(p);
    } else {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        dir = temp;
    }
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\SmartRAM";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.ini";
}

bool Settings::Load() {
    const std::wstring path = iniPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    posX = (long)GetPrivateProfileIntW(kSectionWidget, L"x", 0, path.c_str());
    posY = (long)GetPrivateProfileIntW(kSectionWidget, L"y", 0, path.c_str());
    posRemembered = GetPrivateProfileIntW(kSectionWidget, L"remember", 0, path.c_str()) != 0;

    alwaysTop      = GetPrivateProfileIntW(kSectionBehavior, L"alwaysTop", 1, path.c_str()) != 0;
    smartCleanPct  = GetPrivateProfileIntW(kSectionBehavior, L"smartCleanPct", 85, path.c_str());
    cleanWhenIdle  = GetPrivateProfileIntW(kSectionBehavior, L"cleanWhenIdle", 0, path.c_str()) != 0;
    cleanClipboard = GetPrivateProfileIntW(kSectionBehavior, L"cleanClipboard", 0, path.c_str()) != 0;
    startWithWin   = GetPrivateProfileIntW(kSectionBehavior, L"startWithWin", 0, path.c_str()) != 0;
    closeToTray    = GetPrivateProfileIntW(kSectionBehavior, L"closeToTray", 1, path.c_str()) != 0;
    deepClean      = GetPrivateProfileIntW(kSectionBehavior, L"deepClean", 0, path.c_str()) != 0;

    if (smartCleanPct > 95) smartCleanPct = 95;
    if (smartCleanPct > 0 && smartCleanPct < 50) smartCleanPct = 50;
    return true;
}

static bool WriteString(const std::wstring& ini, const wchar_t* sect,
                        const wchar_t* key, const std::wstring& val) {
    return WritePrivateProfileStringW(sect, key, val.c_str(), ini.c_str()) != FALSE;
}

bool Settings::Save() const {
    const std::wstring path = iniPath();
    bool ok = WriteString(path, kSectionWidget, L"x", std::to_wstring((long long)posX));
    ok &= WriteString(path, kSectionWidget, L"y", std::to_wstring((long long)posY));
    ok &= WriteString(path, kSectionWidget, L"remember", posRemembered ? L"1" : L"0");
    ok &= WriteString(path, kSectionBehavior, L"alwaysTop", alwaysTop ? L"1" : L"0");
    ok &= WriteString(path, kSectionBehavior, L"smartCleanPct",
                      std::to_wstring((unsigned long long)smartCleanPct));
    ok &= WriteString(path, kSectionBehavior, L"cleanWhenIdle", cleanWhenIdle ? L"1" : L"0");
    ok &= WriteString(path, kSectionBehavior, L"cleanClipboard", cleanClipboard ? L"1" : L"0");
    ok &= WriteString(path, kSectionBehavior, L"startWithWin", startWithWin ? L"1" : L"0");
    ok &= WriteString(path, kSectionBehavior, L"closeToTray", closeToTray ? L"1" : L"0");
    ok &= WriteString(path, kSectionBehavior, L"deepClean", deepClean ? L"1" : L"0");
    return ok;
}

void Settings::ApplyStartWithWindows(bool on) {
    startWithWin = on;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}
