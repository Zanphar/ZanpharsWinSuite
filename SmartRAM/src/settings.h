// settings.h — configuration persisted to an INI file in %LOCALAPPDATA%.
#pragma once
#include "util.h"

struct Settings {
    // widget position
    bool  posRemembered = false;
    long  posX = 0, posY = 0;

    // behavior
    bool alwaysTop     = true;
    UINT smartCleanPct = 85;      // 0 = smart clean off
    bool cleanWhenIdle = false;
    bool cleanClipboard= false;
    bool startWithWin  = false;
    bool closeToTray   = true;    // pressing the card's ✕ hides it; exits only from the tray menu
    bool deepClean     = false;   // aggressive clean: standby/modified lists, system working set, file cache

    bool Load();
    bool Save() const;

    static std::wstring iniPath();
    void ApplyStartWithWindows(bool on);   //HKCU Run key + keeps field in sync
};
