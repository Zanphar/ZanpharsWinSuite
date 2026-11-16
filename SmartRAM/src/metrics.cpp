// metrics.cpp — lightweight RAM/CPU sampler.
#include "metrics.h"

static ULONG64 FileTimeU64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

MemStats SysMonitor::Tick() {
    MemStats m {};

    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m.totalPhys = ms.ullTotalPhys;
        m.availPhys = ms.ullAvailPhys;
        m.usagePct  = (DWORD)ms.dwMemoryLoad;
    }

    // CPU: whole-system percentage from GetSystemTimes deltas.
    FILETIME idle {}, kernel {}, user {};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        if (havePrev_) {
            ULONG64 dKernel = FileTimeU64(kernel) - FileTimeU64(prevKernel_);
            ULONG64 dUser   = FileTimeU64(user)   - FileTimeU64(prevUser_);
            ULONG64 dIdle   = FileTimeU64(idle)   - FileTimeU64(prevIdle_);
            // Kernel time already includes idle time on Windows.
            if (dKernel + dUser > 0)
                cpu_ = (DWORD)((dKernel + dUser - dIdle) * 100 / (dKernel + dUser));
        }
        prevIdle_ = idle; prevKernel_ = kernel; prevUser_ = user;
        havePrev_ = true;
    }
    m.cpuPct = cpu_;

    PERFORMANCE_INFORMATION pi = {};
    pi.cb = sizeof(pi);
    if (GetPerformanceInfo(&pi, sizeof(pi)))
        procs_ = (DWORD)pi.ProcessCount;
    m.procCount = procs_;

    return m;
}
