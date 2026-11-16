// prefs.cpp - Smart RAM's options, kept in the same ini the suite already uses.
#include "extras.h"

#include <algorithm>

#include "core.h"

namespace {

std::wstring SettingsFile() {
    wchar_t base[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return L"";
    std::wstring folder = std::wstring(base) + L"\\ZanpharsWinSuite";
    CreateDirectoryW(folder.c_str(), nullptr);  // ok if it already exists
    return folder + L"\\settings.ini";
}

}  // namespace

MemoryPrefs LoadMemoryPrefs() {
    MemoryPrefs prefs;
    std::wstring path = SettingsFile();
    if (path.empty()) return prefs;
    prefs.autoClean =
        GetPrivateProfileIntW(L"Memory", L"AutoClean", prefs.autoClean ? 1 : 0, path.c_str()) != 0;
    prefs.threshold = (int)GetPrivateProfileIntW(L"Memory", L"Threshold", (UINT)prefs.threshold,
                                                 path.c_str());
    prefs.threshold = std::clamp(prefs.threshold, 50, 95);
    prefs.idleClean =
        GetPrivateProfileIntW(L"Memory", L"IdleClean", prefs.idleClean ? 1 : 0, path.c_str()) != 0;
    prefs.clearClipboard = GetPrivateProfileIntW(L"Memory", L"ClearClipboard",
                                                 prefs.clearClipboard ? 1 : 0, path.c_str()) != 0;
    prefs.confirmRegular = GetPrivateProfileIntW(L"Memory", L"ConfirmRegular",
                                                 prefs.confirmRegular ? 1 : 0, path.c_str()) != 0;
    prefs.confirmDeep = GetPrivateProfileIntW(L"Memory", L"ConfirmDeep",
                                              prefs.confirmDeep ? 1 : 0, path.c_str()) != 0;
    return prefs;
}

void SaveMemoryPrefs(const MemoryPrefs& prefs) {
    std::wstring path = SettingsFile();
    if (path.empty()) return;
    auto write = [&](const wchar_t* name, int value) {
        WritePrivateProfileStringW(L"Memory", name, std::to_wstring(value).c_str(), path.c_str());
    };
    write(L"AutoClean", prefs.autoClean ? 1 : 0);
    write(L"Threshold", prefs.threshold);
    write(L"IdleClean", prefs.idleClean ? 1 : 0);
    write(L"ClearClipboard", prefs.clearClipboard ? 1 : 0);
    write(L"ConfirmRegular", prefs.confirmRegular ? 1 : 0);
    write(L"ConfirmDeep", prefs.confirmDeep ? 1 : 0);
}

void ClearClipboardNow() {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    CloseClipboard();
}

bool SystemIdleFor(uint32_t seconds) {
    LASTINPUTINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetLastInputInfo(&info)) return false;
    DWORD idle = GetTickCount() - info.dwTime;
    return idle >= seconds * 1000;
}
