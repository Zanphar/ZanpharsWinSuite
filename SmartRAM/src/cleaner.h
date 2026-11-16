// cleaner.h — memory release engine (async-friendly, no UI access).
#pragma once
#include "util.h"

struct CleanResult {
    ULONG64 releasedBytes = 0;
    DWORD processedProcs = 0;
    bool purgeStandbyOk = false;
};

// Frees memory in three ways:
//  1. Purges the Windows standby list (Win8+: NtSetSystemInformation).
//  2. Trims every process' working set (EmptyWorkingSet over EnumProcesses).
//  3. Optionally clears the clipboard before measuring.
//
// deep adds the aggressive passes: flush the modified page list, empty the
// system working set, and trim the system file cache so cached file pages are
// returned to the free list instead of lingering in RAM.
CleanResult CleanAll(bool cleanClipboard, bool deep = false);
