// Zanphar's WinSuite - engines brought over from the four programs.
//
// Browsers page  <- Browser Cleaner's per-browser trace catalog
// DNS page       <- DNS Protect's provider list, apply/restore and watchdog
// Game Booster   <- System Turbo's profile + the reversible gaming tweaks
// Memory page    <- Smart RAM's automatic-clean settings
// Network page   <- latency-first Windows settings for gaming, journalled like Boost
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// ------------------------------------------------------------------ browsers
struct BrowserTrace {
    std::wstring label;                 // "History", "Cookies", ...
    std::vector<std::wstring> paths;    // files or folders, each may hold several
    uint64_t bytes = 0;
    uint32_t files = 0;
    bool exists = false;
};

struct BrowserEntry {
    std::wstring key;      // stable id, e.g. "chrome"
    std::wstring name;     // "Google Chrome"
    std::wstring detail;   // where the profile lives, shown under the name
    std::vector<std::wstring> processes;  // exe names that count as "running"
    std::vector<BrowserTrace> traces;
    bool installed = false;
    bool running = false;
    bool selected = true;
    uint64_t bytes = 0;
    uint32_t files = 0;
    bool scanned = false;
};

// Every browser the catalog knows about, installed or not.
std::vector<BrowserEntry> BuildBrowserCatalog();

// Measures one browser's traces (safe to call off the UI thread).
void ScanBrowserEntry(BrowserEntry& entry);

struct BrowserCleanResult {
    uint64_t bytesFreed = 0;
    uint32_t deleted = 0;
    uint32_t skipped = 0;
};

// Deletes the traces of the selected entries. Skips running browsers.
BrowserCleanResult CleanBrowserEntries(std::vector<BrowserEntry>& entries);

// Processes of those browsers that are running right now, one entry per browser.
std::vector<std::wstring> RunningBrowsers(const std::vector<BrowserEntry>& entries);

// Asks every running browser to close its windows and waits for them to go.
// Returns how many were still running afterwards; report explains the outcome.
uint32_t CloseRunningBrowsers(const std::vector<BrowserEntry>& entries, std::wstring& report);

// ----------------------------------------------------------------------- dns
struct DnsAdapter {
    std::wstring guid;
    std::wstring name;
    std::wstring current;    // what the adapter resolves with right now
    std::wstring override;   // NameServer value, empty when DHCP supplies DNS
};

struct DnsProvider {
    const wchar_t* key;
    const wchar_t* name;
    const wchar_t* primary;    // empty for "Automatic (system)"
    const wchar_t* secondary;
};

const std::vector<DnsProvider>& DnsProviders();

// Adapters that currently hold an IPv4 configuration.
std::vector<DnsAdapter> LoadDnsAdapters();

// Writes (or with an empty provider, clears) the DNS servers on every adapter.
bool ApplyDns(const std::vector<DnsAdapter>& adapters, const DnsProvider& provider,
              std::wstring& error);

// Puts every adapter back on DHCP-provided DNS.
bool RestoreSystemDns(const std::vector<DnsAdapter>& adapters, std::wstring& error);

// One watchdog pass: rewrites any adapter whose DNS drifted away from the two
// servers. Returns how many were snapped back.
uint32_t DnsWatchdogTick(const std::wstring& primary, const std::wstring& secondary);

// --------------------------------------------------------------------- boost
struct BoostState {
    bool active = false;
    std::wstring planName;                 // plan in use when the boost was taken
    std::wstring currentPlan;              // plan active right now
    std::vector<std::wstring> parked;      // services that were parked
    std::wstring lastMessage;
    bool lastMessageIsError = false;
};

BoostState LoadBoostState();

struct BoostResult {
    bool ok = true;
    uint32_t changed = 0;
    uint32_t failed = 0;      // settings Windows refused to change
    bool adminNeeded = false;  // at least one refusal was an access-denied
    std::wstring message;
};

// High performance plan, parked background services, Game DVR off.
BoostResult ApplyBoost();
// Puts back exactly what ApplyBoost changed, from the journal.
BoostResult RestoreBoost();

// The Network page's latency check: four ICMP echoes to one host, so a before/after pair
// can show what a tweak actually did. `ok` is false when the host did not answer at all.
struct LatencyResult {
    bool ok = false;
    std::wstring host;     // what was asked for
    std::wstring address;  // the IPv4 address it resolved to
    int sent = 0;
    int received = 0;
    int minMs = 0;
    int avgMs = 0;
    int maxMs = 0;
    std::wstring error;
};

// Pings `host` (a name or an IPv4 literal) `pings` times, one second timeout each. Only
// Winsock and the ICMP handle are touched, so this is safe to run off the UI thread.
LatencyResult MeasureLatency(const std::wstring& host, int pings);

// The page's line for a before/after pair: the two averages as "11 ms to 14 ms", then
// the difference in words, then a note when packet loss moved too. Empty unless both
// readings succeeded.
std::wstring LatencyComparison(const LatencyResult& before, const LatencyResult& after);

// ------------------------------------------------------ internet & networking
// One tweak as the page shows it. Everything it changes is saved first, so `journaled`
// means Revert can put the machine back; `applied` means the values already match.
struct NetTweakView {
    std::wstring id;
    std::wstring title;
    std::wstring summary;
    std::wstring writes;
    std::wstring state;      // adapters/values involved, or why there is nothing to do
    int targetCount = 0;
    bool applied = false;
    bool journaled = false;
    bool available = true;
};

std::vector<NetTweakView> LoadNetTweaks();

struct NetOpResult {
    bool ok = true;
    bool adminNeeded = false;
    uint32_t changed = 0;  // values written, or values put back
    uint32_t skipped = 0;
    std::wstring message;
};

// Applies, turns off or reverts one tweak by id (nagle, throttle, games, qos). Turning
// off saves the value that is there and then removes it, so a setting another tool made
// can be switched off from this page and still put back exactly; a revert reads the saved
// originals rather than guessing at a Windows default.
NetOpResult ApplyNetTweak(const std::wstring& id);
NetOpResult TurnOffNetTweak(const std::wstring& id);
NetOpResult RevertNetTweak(const std::wstring& id);

// Every tweak at once. Applied tweaks are left alone; only values this suite saved are
// put back, so settings made by another tool are never overwritten by a revert.
NetOpResult ApplyNetGamingProfile();
NetOpResult RevertNetGamingProfile();

// Switches all four tweaks off in one press, back to Windows' own defaults. Each value is
// saved before it is cleared, so Revert all still restores what a tweak had beforehand.
NetOpResult RestoreNetDefaults();

// Self-test hook: writes a value under HKCU, journals it, changes it and restores it
// through the same code the page uses. No real network setting is touched.
bool NetJournalRoundTrip(std::wstring& detail);

// ------------------------------------------------------------- Game Booster
// One row on the Game Booster page. The safe rows are part of the one-click profile;
// the advanced rows are only ever switched on their own.
struct GameTweakView {
    std::wstring id;
    std::wstring title;
    std::wstring summary;
    std::wstring how;         // when the change takes hold
    int valueCount = 0;
    bool inProfile = false;
    bool needsAdmin = false;
    bool applied = false;
    bool journaled = false;
    bool available = true;
};

std::vector<GameTweakView> LoadGameTweaks();
NetOpResult ApplyGameTweak(const std::wstring& id);
NetOpResult RevertGameTweak(const std::wstring& id);
// Switches a row off, saving what was there first so Revert can still put it back. A row
// whose Windows default is "on" (Game Mode) gets its explicit off value written instead of
// having the value removed, so switching it off really switches it off. This is what makes
// a row that was set by another tool controllable from the page.
NetOpResult TurnOffGameTweak(const std::wstring& id);

// The profile button applies the four safe rows; RevertGameTweaks undoes every row this
// suite saved, advanced ones included, and puts pointer and animation settings back into
// the running session as well as the registry.
NetOpResult ApplyGameTweaks(bool includeAdvanced);
NetOpResult RevertGameTweaks();

// Self-test hook: round-trips a REG_SZ value through the Game Booster journal under HKCU
// and checks that the original type comes back.
bool GameJournalRoundTrip(std::wstring& detail);

// One row of Game Booster's background-services list: a single Windows service, or the
// small family of services that makes up one feature (Xbox Live is four). Turning the row
// off stops those services and sets them to Manual - not Disabled, so Windows can still
// start one when something genuinely asks for it - after saving the original start type,
// running state and delayed-autostart flag, so Revert puts each one back exactly. These
// rows are never part of the one-click profile.
struct GameServiceView {
    std::wstring id;
    std::wstring title;
    std::wstring summary;
    std::wstring state;      // the service names and what they are doing right now
    int installed = 0;       // how many of the row's services exist on this PC
    int active = 0;          // how many are running or still start on their own
    bool applied = false;    // every one of them is stopped and set to Manual
    bool journaled = false;  // WinSuite saved the originals, so Revert is offered
    bool critical = false;   // Windows leans on it; the row says so on the page
};

// Every row, whether or not its services are installed, with the state read live.
std::vector<GameServiceView> LoadGameServices();

// Parks one row's services. `ok` is false when Windows refused one, and adminNeeded says
// whether more rights would have been enough.
NetOpResult TurnOffGameService(const std::wstring& id);
NetOpResult RevertGameService(const std::wstring& id);

// Puts back every service this page parked. RevertGameTweaks calls it, so Revert all
// undoes the services along with the registry tweaks.
NetOpResult RestoreGameServices();

// Self-test hook: round-trips a saved service state (start type, running, delayed) and
// checks the journal entry naming that keeps rows apart.
bool GameServiceJournalRoundTrip(std::wstring& detail);

// Elevated-test hook: parks one named service through the same code the rows use, checks
// that it really is Manual and stopped with its original state saved, then restores it and
// checks that the start type came back and the record was cleared. The service test calls
// it on a scratch service it creates and deletes itself.
bool GameServiceParkRoundTrip(const std::wstring& serviceName, std::wstring& detail);

// -------------------------------------------------------------------- memory
struct MemoryPrefs {
    bool autoClean = false;
    int threshold = 85;      // percent of physical memory
    bool idleClean = false;
    bool clearClipboard = false;
    // The Memory page asks before a clean unless these are switched off in Settings.
    // On by default, so an upgrade never starts cleaning without a prompt.
    bool confirmRegular = true;
    bool confirmDeep = true;
};

MemoryPrefs LoadMemoryPrefs();
void SaveMemoryPrefs(const MemoryPrefs& prefs);

void ClearClipboardNow();

// True when there has been no keyboard or mouse input for this many seconds.
bool SystemIdleFor(uint32_t seconds);

extern const wchar_t* kHighPerformancePlan;

// Friendly name of the power plan Windows is using right now.
std::wstring CurrentPowerPlanName();

// Self-test hook: writes, reads back and removes a REG_DWORD in the boost journal,
// which is how the Game DVR values are stored.
bool BoostDwordRoundTrip(std::wstring& detail);

// Self-test hook: round-trips a parked-service record so a restore keeps the start type,
// whether the service was running, and its delayed-autostart flag.
bool BoostServiceJournalRoundTrip(std::wstring& detail);
