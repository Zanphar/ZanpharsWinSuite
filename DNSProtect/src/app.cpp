// ============================================================================
//  app.cpp — process entry: single instance, elevation, wiring.
// ============================================================================

#include "common.h"
#include "config.h"
#include "dns.h"
#include "log.h"
#include "ui.h"

namespace {

using namespace dzp;

// ---------------------------------------------------------------------------
// Single instance awareness
// ---------------------------------------------------------------------------

class SingleInstance {
 public:
    explicit SingleInstance(const wchar_t* mutexName)
        : mutex_(CreateMutexW(nullptr, TRUE, mutexName)) {}
    ~SingleInstance() {
        if (owned_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    bool TryOwn() {
        return mutex_ != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
    }
    // Brings an already running instance to the front.
    static void ActivateExisting(const wchar_t* className) {
        if (const HWND prev = FindWindowW(className, nullptr)) {
            ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
        }
    }
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

 private:
    HANDLE mutex_ = nullptr;
    bool   owned_ = false;
};

// ---------------------------------------------------------------------------
// Elevation
// ---------------------------------------------------------------------------

bool IsElevated() {
    BOOL isAdmin = FALSE;
    PSID group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &isAdmin);
        FreeSid(group);
    }
    return isAdmin != FALSE;
}

// Re-launches itself with a UAC prompt. Never returns on success.
void RelaunchAsAdmin() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = GetCommandLineW();   // this app ignores its command line
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) ExitProcess(0);
    // declined -> continue unelevated (inspect-only mode)
    log::Warn(L"UAC prompt declined; running without administrator rights");
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int cmdShow) {
    SingleInstance gate(ui::kMutexName);
    if (!gate.TryOwn()) {
        SingleInstance::ActivateExisting(ui::kWindowClass);
        return 0;
    }

    const bool elevated = IsElevated();
    if (!elevated) RelaunchAsAdmin();

    const DataDirs dirs = ResolveDataDirs();
    log::Init(dirs.data / L"dnsprotect.log");
    log::Info(Fmt(L"DNS Protect starting (%ls)",
                  elevated ? L"elevated" : L"unelevated"));

    if (!Engine::Instance().Prepare(dirs, nullptr, elevated)) {
        log::Warn(L"Failed to start the protection engine thread");
        return 1;
    }

    const int rc = ui::Run(instance, cmdShow);
    Engine::Instance().Shutdown();
    return rc;
}
