// agent.cpp - WinSuiteAgent, the sign-in helper for the Task Manager's saved rules.
//
// The suite writes one rule per executable into its settings file; this helper is put in
// the per-user Run key the first time a rule is saved, and then re-applies those rules on
// every sign-in and to any matching program started later in the session. It has no
// window, no tray icon and no UI: it reads the same file the suite does, applies it, and
// exits when the last rule is gone.
//
//     WinSuiteAgent.exe           stays resident, re-applying rules every few seconds
//     WinSuiteAgent.exe --once    one pass only, used by the self-test
#include <windows.h>

#include <string>
#include <vector>

#include "core.h"

static bool HasArgument(const wchar_t* wanted) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; argv && i < argc; ++i)
        if (_wcsicmp(argv[i], wanted) == 0) found = true;
    if (argv) LocalFree(argv);
    return found;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const bool once = HasArgument(L"--once");

    // One helper per session, however many times the Run key fires.
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\ZanpharsWinSuiteAgent");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(single);
        return 0;
    }

    const std::wstring settings = SuiteSettingsPath();
    if (!settings.empty()) {
        for (;;) {
            std::vector<TaskRule> rules = LoadTaskRules(settings);
            if (rules.empty()) break;  // nothing left to enforce: stop quietly
            ApplyTaskRules(rules);
            if (once) break;
            // Cheap enough to poll: the file is a few hundred bytes, and a five-second
            // window still catches a program started right after sign-in.
            Sleep(5000);
        }
    }

    if (single) CloseHandle(single);
    return 0;
}
