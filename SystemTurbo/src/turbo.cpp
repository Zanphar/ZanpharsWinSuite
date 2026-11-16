// ============================================================================
//  turbo.cpp — the Turbo Boost engine implementation.
// ============================================================================

#include "turbo.h"
#include "common.h"
#include "ram.h"
#include "log.h"

#include <objbase.h>

namespace stc {

namespace {

// Small set of safe, visible background services a boost pass parks. Every
// entry must tolerate being stopped and restarted at any time.
struct ServiceTweak {
    const wchar_t* Name;    // SCM key name
    const wchar_t* Title;   // human-readable label for the log
};

inline constexpr ServiceTweak kServices[] = {
    { L"WSearch",       L"Windows Search" },
    { L"SysMain",       L"SysMain (Superfetch)" },
    { L"Spooler",       L"Print Spooler" },
    { L"bthserv",       L"Bluetooth Support" },
    { L"DPS",           L"Diagnostic Policy" },
    { L"Fax",           L"Fax" },
    { L"WMPNetworkSvc", L"WMP Network Sharing" },
};

// Registry values a boost pass turns off. All DWORD, all restored by value.
struct RegTweak {
    const wchar_t* Key;
    const wchar_t* Value;
};

inline constexpr RegTweak kRegTweaks[] = {
    { L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
      L"AppCaptureEnabled" },
    { L"System\\GameConfigStore", L"GameDVR_Enabled" },
};

// High-performance power plan GUID.
inline constexpr wchar_t kHighPerformancePlan[] =
    L"{8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c}";

std::wstring CurrentActivePlan() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Power", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return L"";
    }
    wchar_t buf[64] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    const LRESULT rc = RegGetValueW(HKEY_LOCAL_MACHINE,
                                    L"SYSTEM\\CurrentControlSet\\Control\\Power",
                                    L"ActivePowerScheme", RRF_RT_REG_SZ,
                                    &type, buf, &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS ? buf : L"";
}

bool SetActivePlan(const std::wstring& guidText) {
    GUID guid;
    if (FAILED(CLSIDFromString(guidText.c_str(), &guid))) return false;
    return PowerSetActiveScheme(nullptr, &guid) == ERROR_SUCCESS;
}

LONG SetDwordValue(HKEY root, const std::wstring& sub, const std::wstring& name,
                   DWORD data) {
    HKEY key = nullptr;
    const LRESULT rc = RegCreateKeyExW(root, sub.c_str(), 0, nullptr,
                                       REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                       nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) return rc;
    const LRESULT set = RegSetValueExW(key, name.c_str(), 0, REG_DWORD,
                                       (const BYTE*)&data, sizeof(data));
    RegCloseKey(key);
    return set;
}

LONG DeleteValue(HKEY root, const std::wstring& sub, const std::wstring& name) {
    HKEY key = nullptr;
    const LRESULT rc = RegOpenKeyExW(root, sub.c_str(), 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) return rc;
    const LRESULT del = RegDeleteValueW(key, name.c_str());
    RegCloseKey(key);
    return del;
}

bool ReadDwordValue(HKEY root, const std::wstring& sub, const std::wstring& name,
                    DWORD& data) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD size = sizeof(data), type = 0;
    const LRESULT rc = RegQueryValueExW(key, name.c_str(), 0, &type,
                                        (BYTE*)&data, &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && type == REG_DWORD;
}

}  // namespace

// ---------------------------------------------------------------------------
// Engine implementation
// ---------------------------------------------------------------------------

struct Engine::Impl {
    CRITICAL_SECTION lock;
    DataDirs         dirs;
    HWND             notify  = nullptr;
    bool             elevated = false;

    Config    cfg;
    BoostRestore restore;                     // latest captured restore data
    bool      active    = false;
    unsigned long long lastFreed = 0;         // cumulative freed bytes

    // CPU delta bookkeeping for GetSystemTimes.
    unsigned long long idlePrev = 0, kernelPrev = 0, userPrev = 0;
    bool haveCpu = false;
    double cpuPercent = 0.0;

    HANDLE stopEvent = nullptr;     // signalled on Shutdown
    HANDLE worker    = nullptr;     // instrumentation thread

    // --- helpers ------------------------------------------------------------

    void PostRefresh() {
        if (notify) PostMessageW(notify, WM_APP_REFRESH, 0, 0);
    }

    unsigned long long CaptureCpu() {
        FILETIME idle, kernel, user;
        if (!GetSystemTimes(&idle, &kernel, &user)) return 0;
        auto to64 = [](const FILETIME& ft) {
            ULARGE_INTEGER v;
            v.LowPart = ft.dwLowDateTime;
            v.HighPart = ft.dwHighDateTime;
            return v.QuadPart;
        };
        const unsigned long long i = to64(idle), k = to64(kernel), u = to64(user);
        if (haveCpu) {
            const unsigned long long kd = k - kernelPrev;
            if (kd) {
                // kernel time includes idle; CPU busy share of the interval.
                cpuPercent = 100.0 * (1.0 - double(i - idlePrev) / double(kd));
                if (cpuPercent < 0) cpuPercent = 0;
                if (cpuPercent > 100) cpuPercent = 100;
            }
        }
        idlePrev = i; kernelPrev = k; userPrev = u;
        haveCpu = true;
        return 1;
    }

    unsigned ParkedCountLocked() const { return (unsigned)restore.Services.size(); }

    // --- tweak groups ---------------------------------------------------------

    // Applies one service tweak; records original state once.
    void ApplyService(const ServiceTweak& svc, BoostRestore& br,
                      std::wstring& problems) {
        ServiceRecord* rec = nullptr;
        for (auto& r : br.Services) {
            if (_wcsicmp(r.Name.c_str(), svc.Name) == 0) { rec = &r; break; }
        }

        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!scm) {
            problems += Fmt(L"%ls (SCM error %lu) ", svc.Title,
                            GetLastError());
            return;
        }
        SC_HANDLE sc = OpenServiceW(scm, svc.Name, SERVICE_QUERY_STATUS |
                                    SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG |
                                    SERVICE_STOP | SERVICE_START);
        if (!sc) {
            problems += Fmt(L"%ls (not present) ", svc.Title);
            CloseServiceHandle(scm);
            return;
        }

        QUERY_SERVICE_CONFIGW* cfgBuf = nullptr;   // read start type
        DWORD need = 0;
        QueryServiceConfigW(sc, nullptr, 0, &need);
        if (need) {
            cfgBuf = (QUERY_SERVICE_CONFIGW*)LocalAlloc(LPTR, need);
            if (cfgBuf && !QueryServiceConfigW(sc, cfgBuf, need, &need)) {
                LocalFree(cfgBuf);
                cfgBuf = nullptr;
            }
        }
        SERVICE_STATUS ss = {};
        const BOOL running = QueryServiceStatus(sc, &ss) &&
                             ss.dwCurrentState == SERVICE_RUNNING;

        if (!rec) {
            ServiceRecord add;
            add.Name = svc.Name;
            add.StartType = cfgBuf ? cfgBuf->dwStartType : SERVICE_DEMAND_START;
            add.WasRunning = running != FALSE;
            br.Services.push_back(add);
            rec = &br.Services.back();
            log::Info(Fmt(L"Parking %ls (start %lu, running %d)",
                          svc.Name, rec->StartType, rec->WasRunning ? 1 : 0));
        }

        if (ss.dwCurrentState == SERVICE_RUNNING) {
            if (!ControlService(sc, SERVICE_STOP, &ss))
                problems += Fmt(L"%ls (stop error %lu) ", svc.Title, GetLastError());
        }
        ChangeServiceConfigW(sc, SERVICE_NO_CHANGE, SERVICE_DEMAND_START,
                             SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr);

        if (cfgBuf) LocalFree(cfgBuf);
        CloseServiceHandle(sc);
        CloseServiceHandle(scm);
    }

    void ApplyRegTweak(const RegTweak& t, BoostRestore& br, std::wstring& problems) {
        for (const auto& r : br.Regs) {
            if (_wcsicmp(r.Key.c_str(), t.Key) == 0 &&
                _wcsicmp(r.Value.c_str(), t.Value) == 0) {
                return;   // already recorded, do not overwrite the original
            }
        }
        RegRecord rec;
        rec.Key   = t.Key;
        rec.Value = t.Value;
        DWORD stored = 0;
        rec.Existed = ReadDwordValue(HKEY_CURRENT_USER, t.Key, t.Value, stored);
        if (rec.Existed) {
            rec.Type = REG_DWORD;
            rec.Data = Fmt(L"%lu", stored);
        }
        const LONG rc = SetDwordValue(HKEY_CURRENT_USER, t.Key, t.Value, 0);
        if (rc == ERROR_SUCCESS) {
            br.Regs.push_back(rec);
            log::Info(Fmt(L"Game DVR tweak applied to %ls\\%ls", t.Key, t.Value));
        } else {
            problems += Fmt(L"registry %ls (%ld) ", t.Value, rc);
        }
    }

    void ApplyRamPassLocked() {
        const unsigned long long freed = ram::Release(true);
        if (freed) {
            lastFreed += freed;
            SaveLastFreed(dirs, lastFreed);
            log::Info(Fmt(L"RAM release freed %ls on boost", FormatBytes(freed).c_str()));
        }
        PostRefresh();
    }

    // --- restore ------------------------------------------------------------

    void RestoreService(const ServiceRecord& rec, std::wstring& problems) {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!scm) {
            problems += Fmt(L"SCM error %lu ", GetLastError());
            return;
        }
        SC_HANDLE sc = OpenServiceW(scm, rec.Name.c_str(),
                                    SERVICE_CHANGE_CONFIG | SERVICE_START);
        if (sc) {
            SERVICE_STATUS ss = {};
            QueryServiceStatus(sc, &ss);
            ChangeServiceConfigW(sc, SERVICE_NO_CHANGE, rec.StartType,
                                 SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr);
            if (ss.dwCurrentState == SERVICE_STOPPED && rec.WasRunning) {
                StartServiceW(sc, 0, nullptr);
            }
            log::Info(Fmt(L"Restored %ls", rec.Name.c_str()));
            CloseServiceHandle(sc);
        }
        CloseServiceHandle(scm);
    }

    void RestoreReg(const RegRecord& rec, std::wstring& problems) {
        if (rec.Existed) {
            DWORD data = (DWORD)wcstoul(rec.Data.c_str(), nullptr, 10);
            const LONG rc = SetDwordValue(HKEY_CURRENT_USER, rec.Key, rec.Value,
                                          data);
            if (rc != ERROR_SUCCESS)
                problems += Fmt(L"registry %ls (%ld) ", rec.Value.c_str(), rc);
        } else {
            DeleteValue(HKEY_CURRENT_USER, rec.Key, rec.Value);
        }
    }

    // --- worker -------------------------------------------------------------

    static DWORD WINAPI Worker(LPVOID self) {
        static_cast<Impl*>(self)->WorkerTick();
        return 0;
    }

    void WorkerTick() {
        DWORD timeout = 0;   // snapshot immediately, then every 2 seconds
        for (;;) {
            const DWORD wait = WaitForSingleObject(stopEvent, timeout);
            if (wait == WAIT_OBJECT_0) return;
            timeout = 2000;

            Config localCfg;
            {
                EnterCriticalSection(&lock);
                localCfg = cfg;
                LeaveCriticalSection(&lock);
            }

            SnapshotStatus();   // refresh instrumentation for the next Snapshot

            if (active && elevated && localCfg.AutoRelease &&
                (localCfg.TweakMask & MaskRamOnBoost)) {
                const ram::Stats p = ram::Probe();
                if (p.LoadPercent >= kAutoReleaseLoad) {
                    EnterCriticalSection(&lock);
                    ApplyRamPassLocked();
                    LeaveCriticalSection(&lock);
                }
            }
        }
    }

    void SnapshotStatus() {
        EnterCriticalSection(&lock);
        CaptureCpu();
        const ram::Stats m = ram::Probe();
        memInfo.Total = m.Total;
        memInfo.Avail = m.Avail;
        memInfo.LoadPercent = m.LoadPercent;
        procs.clear();
        for (const auto& p : ram::TopProcesses(6))
            procs.push_back({p.Name, p.WorkingSet});
        LeaveCriticalSection(&lock);
    }

    MemoryInfo           memInfo;
    std::vector<ProcItem> procs;
};

// ---------------------------------------------------------------------------
// Public engine API
// ---------------------------------------------------------------------------

Engine::Engine() : impl_(new Impl) {
    InitializeCriticalSection(&impl_->lock);
}
Engine::~Engine() {
    Shutdown();
    DeleteCriticalSection(&impl_->lock);
    delete impl_;
}

Engine& Engine::Instance() {
    static Engine engine;
    return engine;
}

bool Engine::Prepare(const DataDirs& dirs, HWND notify, bool elevated) {
    Impl* d = impl_;
    d->dirs = dirs;
    d->notify = notify;
    d->elevated = elevated;
    d->cfg = LoadConfig(dirs);
    if (!(d->cfg.TweakMask & MaskAll)) d->cfg.TweakMask = MaskAll;   // first run

    d->lastFreed = LoadLastFreed(dirs);
    d->restore = LoadBoost(dirs);
    const bool wasActive = LoadBoostActive(dirs);

    d->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    d->worker = CreateThread(nullptr, 0, Impl::Worker, d, 0, nullptr);
    if (!d->worker) return false;

    if (!elevated) return true;
    // A boost left active from a previous run keeps its original restore
    // records; re-apply in place so everything is consistent again.
    if (wasActive) {
        log::Info(L"Resuming persisted boost");
        const std::wstring err = Boost();
        if (!err.empty()) log::Warn(Fmt(L"Boost resume: %ls", err.c_str()));
    } else if (!d->restore.Empty()) {
        // Crash leftovers with the boost switched off: undo them.
        log::Warn(L"Found stale boost records; restoring");
        const std::wstring err = TurboOff();
        if (!err.empty()) log::Warn(Fmt(L"Stale restore: %ls", err.c_str()));
    }
    return true;
}

void Engine::Shutdown() {
    Impl* d = impl_;
    if (d->stopEvent) {
        SetEvent(d->stopEvent);
        if (d->worker) {
            WaitForSingleObject(d->worker, 5000);
            CloseHandle(d->worker);
            d->worker = nullptr;
        }
        CloseHandle(d->stopEvent);
        d->stopEvent = nullptr;
    }
}

void Engine::SetNotify(HWND window) { impl_->notify = window; }

void Engine::SetOptions(unsigned tweakMask, bool autoRelease) {
    Impl* d = impl_;
    EnterCriticalSection(&d->lock);
    d->cfg.TweakMask = tweakMask & MaskAll;
    d->cfg.AutoRelease = autoRelease;
    SaveConfig(d->dirs, d->cfg);
    LeaveCriticalSection(&d->lock);
    d->PostRefresh();
}

Status Engine::Snapshot() {
    Impl* d = impl_;
    Status st;
    st.Elevated = d->elevated;
    EnterCriticalSection(&d->lock);
    st.Turbo = d->active && d->elevated;
    st.TweakMask = d->cfg.TweakMask;
    st.ParkedCount = (unsigned)d->restore.Services.size();
    st.LastFreedBytes = d->lastFreed;
    st.AutoRelease = d->cfg.AutoRelease;
    st.Mem = d->memInfo;
    st.Procs = d->procs;
    LeaveCriticalSection(&d->lock);
    return st;
}

std::wstring Engine::Boost() {
    Impl* d = impl_;
    if (!d->elevated)
        return L"Administrator rights are required to apply turbo tweaks.";

    unsigned mask = 0;
    {
        EnterCriticalSection(&d->lock);
        mask = d->cfg.TweakMask;
        LeaveCriticalSection(&d->lock);
    }

    // Working copy of the restore data; records are captured before changes.
    BoostRestore br = LoadBoost(d->dirs);
    std::wstring problems;

    if (mask & MaskPowerPlan) {
        if (br.PowerPlan.empty()) {
            const std::wstring current = CurrentActivePlan();
            if (!current.empty()) br.PowerPlan = current;
        }
        if (!SetActivePlan(kHighPerformancePlan))
            problems += L"power plan (apply failed) ";
        else
            log::Info(L"Power plan set to High performance");
    }
    if (mask & MaskServices) {
        for (const auto& svc : kServices) d->ApplyService(svc, br, problems);
    }
    if (mask & MaskGameDVR) {
        for (const auto& t : kRegTweaks) d->ApplyRegTweak(t, br, problems);
    }
    if (mask & MaskRamOnBoost) {
        d->ApplyRamPassLocked();
    }

    SaveBoost(d->dirs, br);
    {
        EnterCriticalSection(&d->lock);
        d->restore = br;
        d->active = true;
        LeaveCriticalSection(&d->lock);
    }
    SaveBoostActive(d->dirs, true);

    if (problems.empty()) {
        log::Info(L"Turbo boost applied");
        return L"";
    }
    log::Warn(Fmt(L"Boost finished with problems: %ls", problems.c_str()));
    return L"Some tweaks did not fully apply:\n" + problems;
}

std::wstring Engine::TurboOff() {
    Impl* d = impl_;
    if (!d->elevated)
        return L"Administrator rights are required to undo turbo tweaks.";

    const BoostRestore br = LoadBoost(d->dirs);
    std::wstring problems;

    if (!br.PowerPlan.empty()) {
        if (!SetActivePlan(br.PowerPlan))
            problems += L"power plan (restore failed) ";
        else
            log::Info(L"Power plan restored");
    }
    for (const auto& r : br.Regs) d->RestoreReg(r, problems);
    for (const auto& s : br.Services) d->RestoreService(s, problems);

    ClearBoost(d->dirs);
    {
        EnterCriticalSection(&d->lock);
        d->restore = BoostRestore{};
        d->active = false;
        d->lastFreed = 0;
        LeaveCriticalSection(&d->lock);
    }
    SaveBoostActive(d->dirs, false);
    SaveLastFreed(d->dirs, 0);
    d->PostRefresh();

    if (problems.empty()) {
        log::Info(L"Turbo boost removed");
        return L"";
    }
    log::Warn(Fmt(L"Restore finished with problems: %ls", problems.c_str()));
    return L"Some tweaks did not fully restore:\n" + problems;
}

std::wstring Engine::FreeMemory() {
    Impl* d = impl_;
    if (!d->elevated)
        return L"Administrator rights are required to release standby memory.";
    unsigned freed = 0;
    {
        EnterCriticalSection(&d->lock);
        const unsigned long long bytes = ram::Release(true);
        if (bytes) {
            d->lastFreed += bytes;
            SaveLastFreed(d->dirs, d->lastFreed);
            freed = (unsigned)(bytes / (1024 * 1024));
        }
        LeaveCriticalSection(&d->lock);
    }
    log::Info(Fmt(L"Manual RAM release freed %lu MB", freed));
    d->PostRefresh();
    return L"";
}

}  // namespace stc
