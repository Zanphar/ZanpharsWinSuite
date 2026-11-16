// metrics.h — lightweight RAM/CPU sampler.
#pragma once
#include "util.h"

struct MemStats {
    DWORD usagePct = 0;            // physical memory in use, 0..100
    DWORD cpuPct = 0;              // whole-system CPU, 0..100
    DWORD procCount = 0;
    ULONG64 totalPhys = 0;
    ULONG64 availPhys = 0;
};

class SysMonitor {
public:
    // Call periodically (≈1 s). First call establishes baselines.
    MemStats Tick();

private:
    FILETIME prevIdle_ {}, prevKernel_ {}, prevUser_ {};
    bool havePrev_ = false;
    DWORD cpu_ = 0;
    DWORD procs_ = 0;
};
