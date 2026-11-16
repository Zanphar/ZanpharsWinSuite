// app.h — application context wiring single instance, settings, and windows.
#pragma once
#include "settings.h"
#include "metrics.h"

class WidgetWindow;
class SettingsWindow;

struct AppContext {
    Settings settings;
    SysMonitor monitor;
    WidgetWindow* widget = nullptr;
    SettingsWindow* settingsWin = nullptr;

    void OpenSettings();               // lazily creates/shows the settings card
    void ToggleSettings();             // show/hide the settings card (tray double-click)
    void BroadcastSettingsChanged();   // nudges widget to re-apply config
};

AppContext& AppCtx();
UINT AppTaskbarCreatedMsg();           // RegisterWindowMessage(L"TaskbarCreated")

// Context/tray menu commands
enum MenuCmd {
    kMenuShow = 10, kMenuClean = 1, kMenuTop, kMenuSmart, kMenuIdle, kMenuClip,
    kMenuStartup, kMenuSettings, kMenuExit, kMenuDeepClean, kMenuCloseToTray = 11,
};

int  AppRun(HINSTANCE hInst);          // entry point driver (single instance aware)
