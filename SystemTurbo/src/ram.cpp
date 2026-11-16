// ============================================================================
//  ram.cpp — memory instrumentation and release passes.
//
//  Working sets are emptied with EmptyWorkingSet and the standby list is
//  purged through NtSetSystemInformation(SystemMemoryListInformation), the
//  same classes of calls RAM-cleaner style utilities use. Both affect cached
//  and working pages only; no process that refuses the call is hurt.
// ============================================================================

#include "ram.h"
#include "log.h"

namespace stc::ram {

namespace {

// The standby-list purge needs SeProfileSingleProcessPrivilege.
bool EnableProfilePrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeProfileSingleProcessPrivilege", &luid)) {
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp),
                                          nullptr, nullptr);
    CloseHandle(token);
    return ok && GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

// ntdll!NtSetSystemInformation — not part of the SDK import set.
using NtSetSystemInformationFn = LONG(NTAPI*)(ULONG infoClass, PVOID info,
                                              ULONG length);

constexpr ULONG kSystemMemoryListInformation = 80;
constexpr ULONG kMemoryPurgeStandbyList      = 4;

bool TryPurgeStandbyList() {
    if (!EnableProfilePrivilege()) return false;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    const auto ntSet = reinterpret_cast<NtSetSystemInformationFn>(
        GetProcAddress(ntdll, "NtSetSystemInformation"));
    if (!ntSet) return false;
    ULONG command = kMemoryPurgeStandbyList;
    return ntSet(kSystemMemoryListInformation, &command, sizeof(command)) == 0;
}

std::wstring BaseName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Trims every openable process that currently holds more than `minBytes` of
// working set. Returns how many processes accepted the trim.
int TrimLargeWorkingSets(unsigned long long minBytes) {
    int trimmed = 0;
    std::vector<DWORD> pids(1024, 0);
    DWORD bytes = static_cast<DWORD>(pids.size() * sizeof(DWORD));
    DWORD needed = 0;
    if (!EnumProcesses(pids.data(), bytes, &needed)) return 0;
    const int count = static_cast<int>(needed / sizeof(DWORD));

    for (int i = 0; i < count; ++i) {
        const DWORD pid = pids[i];
        if (pid == 0 || pid == 4) continue;  // idle & system are untouchable
        HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA,
                               FALSE, pid);
        if (!p) continue;
        PROCESS_MEMORY_COUNTERS mc = {};
        mc.cb = sizeof(mc);
        if (GetProcessMemoryInfo(p, &mc, sizeof(mc)) &&
            mc.WorkingSetSize >= minBytes) {
            if (EmptyWorkingSet(p)) ++trimmed;
        }
        CloseHandle(p);
    }
    return trimmed;
}

}  // namespace

Stats Probe() {
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    Stats p;
    p.Total       = ms.ullTotalPhys;
    p.Avail       = ms.ullAvailPhys;
    p.LoadPercent = ms.dwMemoryLoad;
    return p;
}

std::vector<ProcUsage> TopProcesses(int count) {
    std::vector<ProcUsage> out;

    std::vector<DWORD> pids(1024, 0);
    DWORD bytes = static_cast<DWORD>(pids.size() * sizeof(DWORD));
    DWORD needed = 0;
    if (!EnumProcesses(pids.data(), bytes, &needed)) return out;
    const int n = static_cast<int>(needed / sizeof(DWORD));

    std::vector<ProcUsage> found;
    for (int i = 0; i < n; ++i) {
        const DWORD pid = pids[i];
        if (pid == 0 || pid == 4) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                               FALSE, pid);
        if (!p) continue;   // SYSTEM-protected processes are simply absent

        wchar_t path[MAX_PATH] = {};
        PROCESS_MEMORY_COUNTERS mc = {};
        mc.cb = sizeof(mc);
        if (GetProcessImageFileNameW(p, path, MAX_PATH) &&
            GetProcessMemoryInfo(p, &mc, sizeof(mc)) && mc.WorkingSetSize > 0) {
            found.push_back({BaseName(path), mc.WorkingSetSize});
        }
        CloseHandle(p);
    }

    std::sort(found.begin(), found.end(),
              [](const ProcUsage& a, const ProcUsage& b) {
                  return a.WorkingSet > b.WorkingSet;
              });

    for (const ProcUsage& item : found) {
        auto hit = std::find_if(out.begin(), out.end(),
                                [&](const ProcUsage& p) {
                                    return _wcsicmp(p.Name.c_str(), item.Name.c_str()) == 0;
                                });
        if (hit != out.end()) {
            hit->WorkingSet += item.WorkingSet;   // merge instances per image
            continue;
        }
        out.push_back(item);
        if (static_cast<int>(out.size()) >= count) break;
    }
    return out;
}

unsigned long long Release(bool allowPurge) {
    const Stats before = Probe();

    // Trim this process too — the UI itself is usually a sizeable consumer
    // that has merely grown over time.
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    // 16 MB and up: smaller holders barely pay for the pass.
    TrimLargeWorkingSets(16ull * 1024 * 1024);
    if (allowPurge) TryPurgeStandbyList();

    Sleep(120);   // let the kernel finish moving pages

    const Stats after = Probe();
    if (after.Avail <= before.Avail) return 0;
    return after.Avail - before.Avail;
}

}  // namespace stc::ram
