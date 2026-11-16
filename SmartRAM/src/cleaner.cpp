// cleaner.cpp — memory release engine.
#include "cleaner.h"

#include <tlhelp32.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* NtSetSystemInformationFn)(ULONG, PVOID, ULONG);

static constexpr ULONG  kSystemMemoryListInformation = 80;
static constexpr ULONG  kSystemFileCacheInformation  = 21;
static constexpr int    kEmptyWorkingSets            = 2;
static constexpr int    kFlushModifiedList           = 3;
static constexpr int    kPurgeStandbyList            = 4;
static constexpr int    kPurgeLowPriorityStandbyList = 5;

static NtSetSystemInformationFn NtSetSystemInfo() {
    static auto fn = reinterpret_cast<NtSetSystemInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                               "NtSetSystemInformation")));
    return fn;
}

static bool SetMemoryListCommand(int cmd) {
    auto fn = NtSetSystemInfo();
    return fn && NT_SUCCESS(fn(kSystemMemoryListInformation, &cmd, sizeof(cmd)));
}

static bool EnablePrivilege(const wchar_t* name) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;
    LUID luid {};
    if (!LookupPrivilegeValueW(nullptr, name, &luid)) { CloseHandle(tok); return false; }
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    bool assigned = (GetLastError() != ERROR_NOT_ALL_ASSIGNED);
    CloseHandle(tok);
    return ok && assigned;
}

static void EnableCommonPrivileges(bool deep) {
    EnablePrivilege(L"SeDebugPrivilege");
    EnablePrivilege(L"SeIncreaseWorkingSetPrivilege");
    // Required by the system-wide memory-list commands (standby purge, empty
    // working sets) and by the file-cache trim below.
    EnablePrivilege(L"SeProfileSingleProcessPrivilege");
    if (deep) EnablePrivilege(L"SeIncreaseQuotaPrivilege");
}

static ULONG64 AvailableMemory() {
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    return GlobalMemoryStatusEx(&ms) ? ms.ullAvailPhys : 0;
}

static ULONG PurgeStandbyLists(bool deep) {
    ULONG ok = 0;
    if (SetMemoryListCommand(kPurgeStandbyList)) ++ok;
    if (SetMemoryListCommand(kPurgeLowPriorityStandbyList)) ++ok;
    // Deep mode also flushes the modified page list: dirty pages are written
    // out and the list is emptied, so those pages can be reused.
    if (deep && SetMemoryListCommand(kFlushModifiedList)) ++ok;
    return ok;
}

// Deep only: ask the kernel to empty every process' working set at once (this
// also reaches processes that can't be opened individually).
static bool EmptySystemWorkingSets() { return SetMemoryListCommand(kEmptyWorkingSets); }

// Deep only: shrink the cache Windows keeps for mapped files. The native
// layout differs between architectures: on x64 PageFaultCount is a ULONG
// followed by padding before the two SIZE_T working-set limits.
struct FileCacheInfo {
    SIZE_T currentSize;
    SIZE_T peakSize;
    ULONG  pageFaultCount;
#ifdef _WIN64
    ULONG  padding;
#endif
    SIZE_T minimumWorkingSet;
    SIZE_T maximumWorkingSet;
    SIZE_T currentSizeIncludingTransitionInPages;
    SIZE_T peakSizeIncludingTransitionInPages;
    ULONG  transitionRePurposeCount;
    ULONG  flags;
};

static bool FlushSystemFileCache() {
    bool ok = false;
    if (auto fn = NtSetSystemInfo()) {
        FileCacheInfo info = {};
        info.minimumWorkingSet = (SIZE_T)-1;
        info.maximumWorkingSet = (SIZE_T)-1;
        ok = NT_SUCCESS(fn(kSystemFileCacheInformation, &info, sizeof(info)));
    }

    using SetCacheSizeFn = BOOL(WINAPI*)(SIZE_T, SIZE_T, DWORD);
    auto setSize = reinterpret_cast<SetCacheSizeFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                               "SetSystemFileCacheSize")));
    if (setSize && setSize((SIZE_T)-1, (SIZE_T)-1, 0)) ok = true;
    return ok;
}

static std::vector<DWORD> EnumerateProcs() {
    std::vector<DWORD> procs(256);
    DWORD cb = 0;
    for (;;) {
        DWORD needed = 0;
        if (!EnumProcesses(procs.data(), (DWORD)(procs.size() * sizeof(DWORD)), &needed))
            return {};
        if (needed >= procs.size() * sizeof(DWORD)) {
            procs.resize(procs.size() * 4);
            continue;
        }
        procs.resize(needed / sizeof(DWORD));
        return procs;
    }
}

static DWORD TrimAllWorkingSets() {
    std::vector<DWORD> procs = EnumerateProcs();
    const DWORD self = GetCurrentProcessId();
    DWORD n = 0;
    for (DWORD pid : procs) {
        if (pid == 0 || pid == 4 || pid == self) continue;
        HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!h) continue;
        if (EmptyWorkingSet(h)) ++n;
        CloseHandle(h);
    }
    return n;
}

static void ClearClipboardIfRequested() {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    CloseClipboard();
}

CleanResult CleanAll(bool cleanClipboard, bool deep) {
    CleanResult r;
    const ULONG64 before = AvailableMemory();

    EnableCommonPrivileges(deep);

    if (cleanClipboard) ClearClipboardIfRequested();

    r.purgeStandbyOk = PurgeStandbyLists(deep) > 0;
    r.processedProcs = TrimAllWorkingSets();
    if (deep) {
        EmptySystemWorkingSets();
        FlushSystemFileCache();
    }

    // Give the kernel a moment to settle counters (deep passes take longer).
    Sleep(deep ? 300 : 150);

    const ULONG64 after = AvailableMemory();
    if (after > before) r.releasedBytes = after - before;
    return r;
}
