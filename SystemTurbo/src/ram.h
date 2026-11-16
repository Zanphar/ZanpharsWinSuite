// ============================================================================
//  ram.h — memory/CPU probes, top-process enumeration and release passes.
// ============================================================================

#pragma once
#include "common.h"

namespace stc::ram {

struct Stats {
    unsigned long long Total = 0;         // installed physical memory, bytes
    unsigned long long Avail = 0;         // currently free, bytes
    double             LoadPercent = 0.0; // 0..100, OS-computed
};

Stats Probe();

struct ProcUsage {
    std::wstring       Name;         // executable name without extension
    unsigned long long WorkingSet = 0; // bytes, summed per image name
};

// The `count` heaviest images, summed per executable name.
std::vector<ProcUsage> TopProcesses(int count);

// Empty working sets of the heaviest processes (and this process) and, when
// allowed, asks the kernel to purge standby lists. Returns the growth of
// available physical memory caused by the pass (bytes; 0 when nothing moved).
unsigned long long Release(bool allowPurge);

}  // namespace stc::ram
