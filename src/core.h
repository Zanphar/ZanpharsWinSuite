// Zanphar's WinSuite - core services (system info, cleanup, startup items)
#pragma once

#include <shlobj.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------- formatting
std::wstring FormatBytes(uint64_t bytes);
std::wstring FormatDuration(uint64_t seconds);
std::wstring FormatCount(uint64_t n);
std::wstring WinErrorText(DWORD code);

// ------------------------------------------------------------- system info
struct DriveInfo {
    std::wstring root;
    std::wstring label;
    std::wstring fileSystem;
    uint64_t total = 0;
    uint64_t freeBytes = 0;
};

struct SysInfo {
    std::wstring osName;
    std::wstring osBuild;
    std::wstring arch;
    std::wstring computer;
    std::wstring user;
    std::wstring cpuName;
    unsigned logicalCpus = 0;
    unsigned physicalCpus = 0;
    uint64_t ramTotal = 0;
    uint64_t ramAvail = 0;
    uint64_t uptimeSeconds = 0;
    std::vector<DriveInfo> drives;
};

// Every mounted volume: fixed, removable, optical, network or RAM disk. Includes
// the bus and model Windows reports for the underlying device where it will say.
struct StorageDevice {
    std::wstring root;
    std::wstring label;
    std::wstring fileSystem;
    std::wstring typeName;
    std::wstring bus;
    std::wstring model;
    uint64_t total = 0;
    uint64_t freeBytes = 0;
    bool accessible = false;  // media present and readable
};

std::vector<StorageDevice> CollectStorageDevices();

SysInfo CollectSystemInfo();
double SampleCpuUsagePercent();          // blocks ~120 ms, returns 0..100
std::wstring SystemReportText(const SysInfo& info, double cpuPercent);

// ----------------------------------------------------------------- memory
struct MemStats {
    uint64_t total = 0;        // installed physical memory
    uint64_t avail = 0;        // free + standby, as Windows reports it
    uint64_t committed = 0;    // commit charge
    uint64_t commitLimit = 0;  // RAM + page file
    uint64_t cached = 0;       // system cache
    uint64_t kernelPaged = 0;
    uint64_t kernelNonPaged = 0;
    uint32_t processCount = 0;
    uint32_t threadCount = 0;
    uint32_t handleCount = 0;
    uint32_t loadPercent = 0;
};

MemStats CollectMemStats();

struct ProcMem {
    std::wstring name;
    DWORD pid = 0;
    uint64_t workingSet = 0;
    uint64_t privateBytes = 0;
};

// Largest working sets first, limited to the current user session.
std::vector<ProcMem> TopMemoryProcesses(size_t maxResults);

struct MemCleanResult {
    uint32_t trimmed = 0;
    uint32_t skipped = 0;
    uint64_t reclaimed = 0;  // working-set bytes actually handed back
    bool purgedStandby = false;
    std::wstring error;
};

// Asks one process to page out its working set. Safe: nothing is closed or killed.
bool TrimProcessWorkingSet(DWORD pid, uint64_t* reclaimed, std::wstring& error);

// Regular clean: trims the working sets of this session's processes. No admin needed
// for your own apps; system processes that refuse are counted as skipped.
MemCleanResult TrimWorkingSets(bool includeOtherSessions);

// Deep clean: trims everything reachable, then purges the standby list (needs admin).
MemCleanResult DeepCleanMemory();

// True when the current token can be given SeProfileSingleProcessPrivilege.
bool DeepCleanAvailable();

// ---------------------------------------------------------------- cleanup
struct CleanGroup {
    std::wstring id;
    std::wstring name;
    std::wstring path;
    bool needsAdmin = false;
    bool personal = false;  // the user's own files (Downloads), never armed by default
    // The shell's Recycle Bin rather than a folder: measured and emptied through the
    // shell, never walked, and `path` only carries the line the row shows.
    bool recycleBin = false;
    bool exists = false;
    bool selected = true;
    bool scanned = false;
    uint64_t bytes = 0;
    uint32_t files = 0;
    std::wstring error;
};

struct CleanResult {
    uint64_t bytesFreed = 0;     // deleted outright
    uint64_t bytesRecycled = 0;  // moved to the Recycle Bin; the space returns when it is emptied
    uint32_t deleted = 0;        // files removed, including those inside a recycled folder
    uint32_t skipped = 0;
};

std::vector<CleanGroup> BuildCleanGroups();

// Measures one target. When `cancel` is set the walk stops at the next file or folder
// boundary and the row is left unmeasured - figures cleared, `scanned` false - so a
// stopped pass never presents a half-walked folder as a finished measurement.
void ScanCleanGroup(CleanGroup& group, const std::atomic<bool>* cancel = nullptr);

// Deletes regular files (and empty directories) strictly under `root`.
// Never follows reparse points, never touches anything outside `root`.
CleanResult DeleteUnderRoot(const std::wstring& root);

// The reversible alternative: every file and folder directly under `root` is sent to
// the Recycle Bin, folders whole so their contents come back together. Reparse points
// are skipped, the root itself is never touched, and any item the shell refuses is
// counted as skipped.
CleanResult RecycleUnderRoot(const std::wstring& root);

// Cleans one target the way the Cleanup page does: a personal folder (Downloads) is
// recycled so nothing is lost for good, every cache is deleted outright.
CleanResult CleanGroupContents(const CleanGroup& group);

// --------------------------------------------------------------- startup
enum class StartupScope {
    HkcuRun,
    HkcuRun32,
    HklmRun,
    HklmRun32,
    FolderUser,
    FolderCommon
};

struct StartupItem {
    std::wstring name;
    std::wstring command;
    std::wstring locationLabel;
    std::wstring sourceId;
    std::wstring valueName;     // registry value name, empty for folder items
    StartupScope scope = StartupScope::HkcuRun;
    bool enabled = true;
    bool systemItem = false;    // stored under HKLM / ProgramData
    bool folderItem = false;
};

std::vector<StartupItem> LoadStartupItems();
bool SetStartupEnabled(const StartupItem& item, bool enable, std::wstring& error);

// ------------------------------------------------------- service deactivator
// One service that Windows starts on its own and that does not belong to Windows.
// WinSuite can stop it and set it to Disabled, and put it back exactly as it was.
struct ServiceEntry {
    std::wstring name;         // SCM name, e.g. "SteamClientService"
    std::wstring display;      // friendly name shown to the user
    std::wstring imagePath;    // executable the service runs, expanded when possible
    DWORD startType = SERVICE_DEMAND_START;
    DWORD state = SERVICE_STOPPED;
    bool thirdParty = false;   // the executable lives outside the Windows folder
    bool driver = false;
    bool autoStart = false;
    bool deactivated = false;  // WinSuite disabled it and has the journal entry
    DWORD originalStart = SERVICE_AUTO_START;
    bool canDeactivate = false;
    std::wstring note;         // why it cannot be deactivated, when it cannot
};

// Auto-start, non-Microsoft services plus anything WinSuite previously disabled.
// Readable without administrator rights; changing one is not.
std::vector<ServiceEntry> LoadServices();

// True when an ImagePath points at an executable outside the Windows directory.
// Exposed so the self-test can exercise it without touching a real service.
bool IsThirdPartyServicePath(const std::wstring& imagePath);

struct ServiceChange {
    bool ok = false;
    bool adminNeeded = false;
    bool stillRunning = false;  // disabled, but Windows would not stop it now
    std::wstring error;
};

// Stops the service (if running) and sets its start type to Disabled, remembering
// the original start type so ReactivateService can restore it later.
ServiceChange DeactivateService(const ServiceEntry& entry);

// Puts the start type back and starts the service again when it used to auto-start.
ServiceChange ReactivateService(const ServiceEntry& entry);

// Registry value helpers (shared with the self-test).
struct RegValue {
    DWORD type = REG_SZ;
    std::wstring data;
};

bool ReadRegValue(HKEY root, const std::wstring& keyPath, const std::wstring& valueName,
                  DWORD extraFlags, RegValue& out, std::wstring& error);
bool WriteRegValue(HKEY root, const std::wstring& keyPath, const std::wstring& valueName,
                   DWORD extraFlags, const RegValue& value, std::wstring& error);
bool DeleteRegValue(HKEY root, const std::wstring& keyPath, const std::wstring& valueName,
                    DWORD extraFlags, std::wstring& error);

// Folder helpers
std::wstring KnownFolderPath(REFKNOWNFOLDERID id);
bool ExpandEnv(const std::wstring& in, std::wstring& out);

// ------------------------------------------------------- service control
// Every installed service, not only the auto-start third-party ones the Deactivate
// page lists: the Services page edits them, so it needs the whole picture including
// running state, description and whether Windows owns the binary.
struct ServiceRow {
    std::wstring name;
    std::wstring display;
    std::wstring description;
    std::wstring imagePath;
    DWORD startType = SERVICE_DEMAND_START;
    bool delayed = false;   // the delayed flag on an automatic service
    DWORD state = SERVICE_STOPPED;
    bool running = false;
    bool windows = false;   // binary lives under %SystemRoot%: read-only here
    bool driver = false;
    std::wstring error;     // why its details could not be read, when they could not
};

std::vector<ServiceRow> LoadAllServices();

struct ServiceOp {
    bool ok = false;
    bool adminNeeded = false;
    std::wstring error;
};

bool SetServiceStart(const std::wstring& name, DWORD startType, bool delayed, ServiceOp& op);
bool StartNamedService(const std::wstring& name, ServiceOp& op);
bool StopNamedService(const std::wstring& name, ServiceOp& op);
bool CreateNewService(const std::wstring& name, const std::wstring& display,
                      const std::wstring& imagePath, const std::wstring& description,
                      ServiceOp& op);
bool UpdateExistingService(const std::wstring& name, const std::wstring& display,
                           const std::wstring& imagePath, const std::wstring& description,
                           ServiceOp& op);

// Refuses Windows components and anything marked ours=false unless allowAny is set;
// the caller confirms with the user first either way.
bool DeleteNamedService(const ServiceRow& row, ServiceOp& op);

std::wstring ServiceStartTypeLabel(DWORD startType);
std::wstring ServiceStateLabel(DWORD state);

// ------------------------------------------------------------ task control
struct ProcRow {
    std::wstring name;
    std::wstring path;      // empty when the image cannot be opened
    std::wstring user;
    DWORD pid = 0;
    // When the process started, in FILETIME ticks. A pid on its own is not an identity: it
    // is reused, and a ticked row must never act on the next process to inherit it.
    uint64_t created = 0;
    DWORD sessionId = 0;
    uint64_t workingSet = 0;
    DWORD priority = NORMAL_PRIORITY_CLASS;
    ULONG_PTR affinity = 0;
    bool accessible = false;  // we could open it with the rights the page needs
};

// Every process on the PC, named first.
std::vector<ProcRow> LoadProcessRows();

bool EndRunningTask(DWORD pid, bool force, std::wstring& error);
bool SetTaskAffinity(DWORD pid, ULONG_PTR mask, std::wstring& error);
bool SetTaskPriority(DWORD pid, DWORD priority, std::wstring& error);
bool LaunchNewTask(const std::wstring& commandLine, std::wstring& error);
std::wstring PriorityLabel(DWORD priority);
std::wstring AffinityText(ULONG_PTR mask);

// A saved affinity/priority choice for one executable, applied to every process of
// that name by the suite and by the companion agent that starts at sign-in.
struct TaskRule {
    std::wstring exe;  // file name, lower case, e.g. "notepad.exe"
    ULONG_PTR affinity = 0;
    DWORD priority = 0;  // 0 means "leave the priority alone"
};

// Stored in the suite's settings file under [TaskRules]; one line per executable.
std::vector<TaskRule> LoadTaskRules(const std::wstring& settingsPath);
bool SaveTaskRules(const std::wstring& settingsPath, const std::vector<TaskRule>& rules);
// Applies the rules to every matching running process and returns how many it changed.
uint32_t ApplyTaskRules(const std::vector<TaskRule>& rules, uint32_t* considered = nullptr);

// The suite's settings file and agent path, so the agent and the suite agree.
std::wstring SuiteSettingsPath();
std::wstring SuiteAgentPath();

// Puts WinSuiteAgent.exe in the per-user Run key so the saved rules are applied again at
// every sign-in, or removes that entry when the last rule is gone.
bool RegisterTaskAgent(bool present, std::wstring& error);
bool TaskAgentRegistered();

// Starts WinSuiteAgent.exe when it is not already running, so a saved rule is re-applied
// to programs started later in this session instead of only at the next sign-in.
bool EnsureTaskAgentRunning();

// ------------------------------------------------------------- large files
struct LargeFile {
    std::wstring path;
    uint64_t bytes = 0;
    uint64_t modifiedTicks = 0;  // FILETIME packed into 64 bits
    bool selected = false;
};

struct LargeFileScan {
    uint64_t minBytes = 500ull * 1024 * 1024;
    size_t maxResults = 300;
    size_t maxDepth = 32;
};

// Profile plus any fixed drive other than the system drive.
std::vector<std::wstring> LargeFileRoots();
void ScanLargeFiles(const std::vector<std::wstring>& roots, const LargeFileScan& options,
                    std::vector<LargeFile>& out);

// The shared safety gate: reparse points, protected Windows/program locations and
// anything outside the scanned roots are rejected before a file is touched.
bool CanRemoveLargeFile(const std::wstring& path, const std::vector<std::wstring>& roots,
                        std::wstring& error);

struct LargeDeleteResult {
    uint32_t removed = 0;
    uint32_t failed = 0;
    uint64_t bytes = 0;
    std::wstring firstError;
};

// Sends guarded files to the Recycle Bin so the action stays reversible.
LargeDeleteResult RemoveLargeFiles(const std::vector<std::wstring>& paths,
                                   const std::vector<std::wstring>& roots);

std::wstring FormatFileTime(uint64_t filetimeTicks);
