// app.cpp — application context, single-instance guard, message loop.
#include "app.h"
#include "gfx.h"
#include "widget.h"
#include "settings_win.h"

#include <shellapi.h>

static AppContext* g_ctx = nullptr;
static UINT g_taskbarCreatedMsg = 0;
static HANDLE g_singletonMutex = nullptr;

AppContext& AppCtx() { return *g_ctx; }
UINT AppTaskbarCreatedMsg() { return g_taskbarCreatedMsg; }

void AppContext::OpenSettings() {
    if (!settingsWin) {
        settingsWin = new SettingsWindow();
        if (!settingsWin->EnsureCreated(GetModuleHandleW(nullptr))) {
            delete settingsWin;
            settingsWin = nullptr;
            return;
        }
        settingsWin->Repaint();
    }
    settingsWin->Reveal();
}

void AppContext::ToggleSettings() {
    if (settingsWin && IsWindowVisible(settingsWin->Hwnd())) {
        ShowWindow(settingsWin->Hwnd(), SW_HIDE);
        return;
    }
    OpenSettings();
}

void AppContext::BroadcastSettingsChanged() {
    if (widget)
        PostMessageW(widget->Hwnd(), WM_APP + 2, 0, 0);
    if (settingsWin && settingsWin->Hwnd())
        PostMessageW(settingsWin->Hwnd(), WM_APP + 2, 0, 0);
}

// --------------------------------------------------------------- run -------
int AppRun(HINSTANCE hInst) {
    static AppContext ctx;
    g_ctx = &ctx;

#ifdef SMART_RAM_MINGW
    // MinGW links a generic default manifest (no DPI awareness), so opt in
    // before any window exists. The MSVC build gets this from app.manifest.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

    if (!gfx::InitFactories()) {
        MessageBoxW(nullptr, L"Direct2D is not available on this system.",
                    L"Smart RAM", MB_ICONERROR);
        g_ctx = nullptr;
        return 1;
    }

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // single-instance guard
    g_singletonMutex = CreateMutexW(nullptr, FALSE, L"Local\\SmartRAM_Modern_1B7A21");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"SmartRAM_Widget_v1", nullptr);
        if (existing)
            PostMessageW(existing, WM_APP + 4, 0, 0);
        CloseHandle(g_singletonMutex);
        g_singletonMutex = nullptr;
        g_ctx = nullptr;
        return 0;
    }

    ctx.settings.Load();
    ctx.settings.ApplyStartWithWindows(ctx.settings.startWithWin);

    ctx.widget = new WidgetWindow();
    if (!ctx.widget->Create(hInst)) {
        MessageBoxW(nullptr, L"Unable to create the Smart RAM card window.",
                    L"Smart RAM", MB_ICONERROR);
        delete ctx.widget;
        ctx.widget = nullptr;
        g_ctx = nullptr;
        return 1;
    }
    ctx.widget->Show();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    delete ctx.settingsWin;
    delete ctx.widget;
    if (g_singletonMutex) {
        ReleaseMutex(g_singletonMutex);
        CloseHandle(g_singletonMutex);
    }
    g_ctx = nullptr;
    return 0;
}
