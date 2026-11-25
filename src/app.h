// Zanphar's WinSuite - application state and page rendering
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core.h"
#include "extras.h"

enum class Page {
    Dashboard,
    Cleanup,
    Browsers,
    Memory,
    Dns,
    Network,
    Boost,
    LargeFiles,
    Startup,
    Deactivate,
    System,
    Storage,
    Services,
    Tasks,
    Settings
};

enum HitId {
    HitNone = 0,
    NavDashboard = 1,
    NavCleanup = 2,
    NavStartup = 3,
    NavSystem = 4,
    HitScan = 10,
    HitClean = 11,
    HitRefresh = 12,
    HitCopyReport = 13,
    HitStartupRefresh = 14,
    HitQuickScan = 15,
    HitQuickStartup = 16,
    NavLargeFiles = 17,
    HitLargeScan = 18,
    HitLargeRecycle = 19,
    HitRestartAdmin = 21,
    NavMemory = 22,
    HitMemClean = 23,
    HitMemDeep = 24,
    HitMemRefresh = 25,
    NavDeactivate = 26,
    HitDeactivateRefresh = 27,
    HitDeactivateRestoreAll = 28,
    NavBrowsers = 29,
    NavDns = 30,
    NavBoost = 31,
    HitBrowserScan = 32,
    HitBrowserClean = 33,
    HitBrowserClose = 34,
    HitDnsApply = 35,
    HitDnsRestore = 36,
    HitDnsRefresh = 37,
    HitDnsWatchdog = 38,
    HitBoostToggle = 39,
    HitBoostRestore = 40,
    HitBoostRefresh = 41,
    HitMemIdle = 42,
    HitMemClipboard = 43,
    HitCleanupReset = 44,
    HitCleanupSelectAll = 45,
    HitCleanupClearAll = 46,
    HitScanCancel = 47,
    HitCleanupSortSize = 48,
    HitCleanupSortDefault = 49,
    NavSettings = 50,
    HitSettingsCloseTray = 51,
    HitSettingsMinTray = 52,
    HitSettingsLicense = 53,
    HitLicenseClose = 54,
    NavServices = 55,
    NavTasks = 56,
    HitSvcRefresh = 60,
    HitSvcStart = 61,
    HitSvcStop = 62,
    HitSvcNew = 63,
    HitSvcEdit = 64,
    HitSvcDelete = 65,
    HitTaskRefresh = 66,
    HitTaskEnd = 67,
    HitTaskForce = 68,
    HitTaskNew = 69,
    HitTaskApplyAffinity = 70,
    HitTaskApplyPriority = 71,
    HitTaskRemember = 72,
    HitTaskForget = 73,
    HitTaskRulesHint = 74,
    NavNetwork = 57,
    HitNetApply = 75,
    HitNetRevert = 76,
    HitNetRefresh = 77,
    HitBoostProfile = 78,  // the system-profile row inside Game Booster
    HitSettingsMemRegular = 79,
    HitSettingsMemDeep = 80,
    HitSvcFilter = 81,
    HitSvcFilterClear = 82,
    HitTaskFilter = 83,
    HitTaskFilterClear = 84,
    HitNetHost = 85,       // the latency check's host field
    HitNetPing = 86,
    HitNetReset = 87,
    HitNetHostClear = 88,
    HitSvcSelectAll = 89,
    HitSvcClearChecked = 90,
    HitTaskSelectAll = 91,
    HitTaskClearChecked = 92,
    HitNetDefaults = 93,  // the Networking page's one-press Windows-defaults restore
    HitSettingsTrayStartup = 94,  // open hidden in the notification area
    HitSettingsToasts = 95,       // notification-area balloons on or off
    NavStorage = 96,
    HitStorageRefresh = 97,
    HitStorageCancel = 98,
    // Ranges that sit above every older one: matched before the DNS/row ranges below.
    HitSvcTypeBase = 8100,   // 4 start-type chips
    HitTaskPriorityBase = 8200,  // 6 priority chips
    HitTaskCpuBase = 8300,   // one chip per logical CPU, 0..63
    HitSvcRowBase = 9000,    // every installed service
    HitTaskRowBase = 10000,  // every running process
    HitNetRowBase = 11000,   // one network tweak, ids above every other range
    HitGameTweakBase = 12000,  // one Game Booster tweak, above every other range
    HitGameServiceBase = 13000,  // one Game Booster background-service row, above all of them
    HitStorageRowBase = 14000,   // one local volume's Optimize button
    // Kept well clear of the fixed ids above: the chip ranges are matched with a
    // "base .. base + n" test, so any id inside one would be swallowed.
    HitLargeChipBase = 100,
    HitDnsProviderBase = 400,   // 5 providers
    HitMemAutoBase = 500,       // Off plus four thresholds
    HitCleanupRowBase = 2000,
    HitStartupRowBase = 3000,
    HitLargeRowBase = 4000,
    HitDeactivateRowBase = 5000,
    HitBrowserRowBase = 6000,
    HitDnsRowBase = 7000
};

struct Hit {
    RECT rect;
    int id = HitNone;
    bool enabled = true;
};

struct CpuMeter {
    ULONGLONG idle = 0, kernel = 0, user = 0;
    double percent = 0.0;
    bool primed = false;
    void Update();
};

// One ticked process. The creation time tells two processes that shared a pid apart, so
// a tick is dropped as soon as the process it belonged to ends instead of passing to
// whatever Windows starts next under the same number. A process the suite may not open
// reports 0, and then the tick follows the pid alone, which is what it always did.
struct TaskRef {
    DWORD pid = 0;
    uint64_t created = 0;
};

struct AppState {
    Page page = Page::Dashboard;
    bool elevated = false;
    bool promptSuppressed = false;  // the user chose standard mode; stop asking on launch
    SysInfo sys;
    std::vector<StorageDevice> storage;
    CpuMeter cpu;

    std::vector<CleanGroup> groups;
    bool scanning = false;
    bool scanStopping = false;   // cancel asked for, worker has not acknowledged yet
    bool scanCancelled = false;  // last pass was stopped; do not restart it unasked
    bool cleaning = false;
    bool hasScanned = false;
    std::wstring scanStatus;
    std::wstring cleanMessage;
    bool cleanMessageIsError = false;
    uint64_t lastFreed = 0;
    uint32_t lastDeleted = 0;
    uint32_t lastSkipped = 0;
    bool showCleanResult = false;

    MemStats mem;
    std::vector<ProcMem> memProcs;
    std::wstring memMessage;
    bool memMessageIsError = false;
    bool memBusy = false;
    uint64_t memReclaimed = 0;
    uint32_t memTrimmed = 0;
    uint32_t memSkipped = 0;

    std::vector<StartupItem> startup;
    std::wstring startupMessage;
    bool startupMessageIsError = false;
    std::wstring systemMessage;

    std::vector<ServiceEntry> services;
    std::wstring deactivateMessage;
    bool deactivateMessageIsError = false;
    bool deactivateBusy = false;

    // Browsers page (Browser Cleaner)
    std::vector<BrowserEntry> browsers;
    bool browserScanning = false;
    bool browserCleaning = false;
    bool browserScanned = false;
    std::wstring browserMessage;
    bool browserMessageIsError = false;
    int scrollBrowsers = 0;

    // DNS page (DNS Protect)
    std::vector<DnsAdapter> dnsAdapters;
    int dnsProvider = 0;
    bool dnsBusy = false;
    bool dnsWatchdog = false;
    bool dnsApplied = false;
    uint32_t dnsIncidents = 0;
    std::wstring dnsPrimary;
    std::wstring dnsSecondary;
    std::wstring dnsMessage;
    bool dnsMessageIsError = false;
    int scrollDns = 0;

    // Game Booster page: System Turbo's profile plus the individual gaming tweaks.
    BoostState boost;
    bool boostBusy = false;
    std::vector<GameTweakView> gameTweaks;
    std::vector<GameServiceView> gameServices;
    std::wstring boosterMessage;
    bool boosterMessageIsError = false;
    int scrollBooster = 0;

    // Internet & networking page (gaming tweaks)
    std::vector<NetTweakView> netTweaks;
    std::wstring netMessage;
    bool netMessageIsError = false;

    // Latency check: one measurement before the tweaks and one after, against a host the
    // user picks. Both live only in memory, so a restart starts a fresh comparison.
    std::wstring netHost = L"1.1.1.1";
    LatencyResult netBefore;
    LatencyResult netAfter;
    bool netPinging = false;
    std::wstring netPingMessage;
    bool netPingMessageIsError = false;
    // Apply profile was pressed with no baseline in hand, so the profile waits for the
    // measurement that is now running and goes on by itself when it lands.
    bool netApplyPending = false;
    int scrollNet = 0;  // the page scrolls once the latency card is added to it

    // Memory page options (Smart RAM)
    MemoryPrefs memPrefs;

    std::vector<LargeFile> largeFiles;
    std::vector<std::wstring> largeRoots;
    bool largeScanning = false;
    bool largeDeleting = false;
    bool largeScanned = false;
    uint64_t largeThreshold = 500ull * 1024 * 1024;
    std::wstring largeMessage;
    bool largeMessageIsError = false;

    // Services page
    std::vector<ServiceRow> serviceRows;
    int serviceSelected = -1;
    bool servicesLoaded = false;
    std::wstring servicesMessage;
    bool servicesMessageIsError = false;
    int scrollServices = 0;
    std::wstring serviceFilter;  // narrows the list by display or service name
    // The rows ticked for a bulk Start/Stop, remembered by service name and by pid so a
    // refresh that reorders the list leaves the ticks on the same rows.
    std::vector<std::wstring> serviceChecked;

    // Task Manager page
    std::vector<ProcRow> taskRows;
    int taskSelected = -1;
    bool tasksLoaded = false;
    std::wstring tasksMessage;
    bool tasksMessageIsError = false;
    int scrollTasks = 0;
    std::wstring taskFilter;  // narrows the process list by name
    std::vector<TaskRef> taskChecked;
    int inputFocus = HitNone;  // the text field that takes typing, if any
    std::vector<TaskRule> taskRules;
    ULONG_PTR taskCpuMask = 0;      // affinity being edited for the selected task
    DWORD taskPriority = NORMAL_PRIORITY_CLASS;
    bool taskRemember = false;      // save the affinity/priority as a rule

    // Settings page. All three tray options are off until the user asks for them, so
    // closing, minimising and starting keep behaving the way they did before the page
    // existed.
    bool closeToTray = false;
    bool minimizeToTray = false;
    bool startInTray = false;  // start hidden: the tray icon is the way in
    bool toastNotifications = true;  // report a tray clean with a balloon; on by default
    int scrollSettings = 0;  // Settings scrolls once the window is short
    bool showLicense = false;  // the read-only licence view covers the window
    int licenseScroll = 0;

    int hoverId = HitNone;
    // The Cleanup rows in the order the page shows them: indices into groups, biggest
    // measured folder first (or the built-in order when the user picks that). groups
    // itself keeps its built-in order, because the scan, the bulk controls and the stored
    // ticks all work by position.
    std::vector<int> cleanOrder;
    bool cleanSortBySize = true;  // the Order switch above the list; not persisted
    int scrollNav = 0;            // the sidebar scrolls once its entries no longer fit
    int scrollCleanup = 0;
    int scrollDashboard = 0;  // the Storage card's list on the Dashboard
    int scrollSystem = 0;
    // Storage page: volumes, which mode defrag.exe will use, and the last result.
    bool storageBusy = false;
    std::wstring storageAction;      // "Defragmenting D:\" while the worker runs
    std::wstring storageMessage;
    bool storageMessageIsError = false;
    ULONGLONG storageStarted = 0;    // GetTickCount64() at the start of the operation
    int storagePercent = -1;         // last percentage defrag.exe reported, or -1
    int scrollStorage = 0;
    // Scrollbar geometry from the last Storage paint, for grabbing and dragging its thumb.
    int storageScrollMax = 0;
    int storagePageStep = 0;
    RECT storageTrack = {};
    RECT storageThumb = {};
    bool storageDragging = false;
    int storageDragOffset = 0;
    int scrollStartup = 0;
    int scrollLarge = 0;
    int scrollDeactivate = 0;
    int width = 1280;
    int height = 860;
};

extern float g_scale;

// The BSD 3-Clause text the Settings page shows read-only; defined in pages.cpp so the
// page, the viewer and the self-test all read the same words.
extern const wchar_t* const kLicenseText;

// The rows each list page shows, as indices into the full list: every row when the
// filter is empty, otherwise the ones whose name contains it (case-insensitive).
std::vector<int> FilterServiceRows(const std::vector<ServiceRow>& rows,
                                   const std::wstring& filter);
std::vector<int> FilterTaskRows(const std::vector<ProcRow>& rows, const std::wstring& filter);

// A row is ticked for a bulk action when its service name, or its process instance, is in
// the matching vector. Both survive a refresh because neither depends on the row's
// position, and the task tick carries the creation time as well: Windows reuses pids, and
// a tick that outlived its process must not put a stranger in the bulk action's sights.
bool IsServiceChecked(const AppState& st, const std::wstring& name);
bool IsTaskChecked(const AppState& st, DWORD pid, uint64_t created);

void LoadSystemPage(AppState& st);
void LoadBrowsersPage(AppState& st);
void LoadDnsPage(AppState& st);
void ToggleBrowserEntry(AppState& st, int index);

void LoadMemoryPage(AppState& st);
void LoadStartupPage(AppState& st);
void ToggleStartupItem(AppState& st, int index);
void LoadDeactivatePage(AppState& st);
void ToggleService(AppState& st, int index);
void ReactivateAllServices(AppState& st);
void PaintApp(HDC dc, int width, int height, AppState& st, std::vector<Hit>& hits);
std::wstring BuildReport(const AppState& st);
