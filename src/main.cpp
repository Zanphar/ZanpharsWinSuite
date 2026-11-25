// Zanphar's WinSuite - application shell
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app.h"
#include "core.h"
#include "ui.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

static const wchar_t* kAppName = L"Zanphar's WinSuite";
static const wchar_t* kVersion = L"2.0.0";

#define WM_APP_SCAN_DONE (WM_APP + 1)
#define WM_APP_CLEAN_DONE (WM_APP + 2)
#define WM_APP_LARGE_SCAN_DONE (WM_APP + 3)
#define WM_APP_LARGE_DELETE_DONE (WM_APP + 4)
#define WM_APP_QUIET_SCAN_DONE (WM_APP + 5)
#define WM_APP_MEM_DONE (WM_APP + 6)
#define WM_APP_BROWSER_SCAN_DONE (WM_APP + 7)
#define WM_APP_BROWSER_CLEAN_DONE (WM_APP + 8)
#define WM_APP_BOOST_DONE (WM_APP + 9)
#define WM_APP_CLEAN_REFRESH_DONE (WM_APP + 10)
#define WM_APP_SCAN_PROGRESS (WM_APP + 11)
#define WM_APP_TRAY (WM_APP + 12)
#define WM_APP_LATENCY_DONE (WM_APP + 13)
#define WM_APP_STORAGE_DONE (WM_APP + 14)
#define kQuietScanMinSeconds 5   // fastest cadence for the Cleanup page's live totals
#define kQuietScanMaxSeconds 60  // slowest, for folders that are expensive to measure

static HWND g_hwnd = nullptr;
static AppState* g_app = nullptr;
static std::vector<Hit> g_hits;

// Posted to a copy that is already running so it can close itself cleanly - tray icon and
// all - instead of being terminated mid-paint.
static UINT g_quitMessage = 0;

// The window is drawn at a scale (see pages.cpp); the shell needs the same arithmetic for
// the few places where it has to know where a control sits.
static int Scaled(int v) { return (int)std::lround((double)v * g_scale); }

// Whether a page is actually in front of the user. IsIconic alone is not enough: a window
// hidden in the notification area is not minimised, so a suite left in the tray would keep
// measuring folders, enumerating processes and reading services all day. Everything that
// is only there to keep a visible page current asks this instead.
static bool OnScreen(HWND hwnd) {
    return hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd);
}

static void ReloadServices(const std::wstring& keepName = L"");
static void LoadTasksPage(AppState& st);
static void LoadNetPage();
static void LoadBoosterPage();

static std::mutex g_workerMutex;
static std::vector<CleanGroup> g_scanResult;
static std::vector<CleanGroup> g_scanLive;  // the scan in progress, row by row
static CleanResult g_cleanResult;
static std::vector<LargeFile> g_largeScanResult;
static OptimizeOutcome g_storageOutcome;
static LargeDeleteResult g_largeDeleteResult;
static std::vector<CleanGroup> g_quietScanResult;
static std::vector<CleanGroup> g_cleanRefreshResult;
static std::atomic<bool> g_workerRunning{false};
static std::atomic<bool> g_scanCancel{false};  // set by the Cancel button, read per entry
static std::atomic<bool> g_quietScanActive{false};
static std::atomic<bool> g_cleanRefreshActive{false};
static std::atomic<unsigned> g_quietInterval{kQuietScanMinSeconds};  // grows with cost
static unsigned g_quietTick = 0;
static unsigned g_memTick = 0;
static unsigned g_storageTick = 0;
static std::atomic<int> g_storagePercent{-1};
static unsigned g_dnsTick = 0;
static unsigned g_svcTick = 0;
static unsigned g_taskTick = 0;
static MemCleanResult g_memResult;
static std::vector<BrowserEntry> g_browserScanResult;
static BrowserCleanResult g_browserCleanResult;
static BoostResult g_boostResult;
// The tweak half of an Apply/Revert-all press, carried across the Boost worker so the
// page can report both halves in one line when the profile finishes.
static std::wstring g_boostTweakNote;
static bool g_boostTweakNoteIsError = false;
static std::atomic<bool> g_dnsWatchdogRunning{false};
static std::atomic<uint32_t> g_dnsIncidentCount{0};
static bool g_idleCleanDone = false;
static ULONGLONG g_lastAutoClean = 0;

// ---------------------------------------------------------------- clipboard
static bool CopyText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        CloseClipboard();
        return false;
    }
    void* dst = GlobalLock(mem);
    memcpy(dst, text.c_str(), bytes);
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

// ---------------------------------------------------------------- elevation
static bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION info = {};
    DWORD size = sizeof(info);
    bool elevated = false;
    if (GetTokenInformation(token, TokenElevation, &info, sizeof(info), &size))
        elevated = info.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

static std::wstring QuoteArgument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring quoted = L"\"";
    for (wchar_t ch : arg) {
        if (ch == L'\"') quoted += L'\\';
        quoted += ch;
    }
    quoted += L'\"';
    return quoted;
}

// ------------------------------------------------------------ preferences
// A one-line ini under the user's local app data: Elevation\Prompt=0 means the
// user declined UAC once and should not be asked again on launch.
static std::wstring SettingsPath() {
    wchar_t base[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return L"";
    std::wstring folder = std::wstring(base) + L"\\ZanpharsWinSuite";
    CreateDirectoryW(folder.c_str(), nullptr);  // ok if it already exists
    return folder + L"\\settings.ini";
}

static bool PrefersStandardMode() {
    std::wstring path = SettingsPath();
    if (path.empty()) return false;
    return GetPrivateProfileIntW(L"Elevation", L"Prompt", 1, path.c_str()) == 0;
}

static void SetStandardModePreference(bool preferStandard) {
    std::wstring path = SettingsPath();
    if (path.empty()) return;
    WritePrivateProfileStringW(L"Elevation", L"Prompt", preferStandard ? L"0" : L"1",
                               path.c_str());
}

// The Cleanup page's ticks are stored per target id, so opting a cache out once keeps it
// out on the next launch and re-ticking it undoes that. The user's own folder is the
// exception both ways: it is never written and never restored ticked, so Downloads can
// never arrive armed, whatever a previous session did. Only deliberate clicks are saved
// (the page clearing its ticks after a clean is not a choice), which is why the path is
// passed in - the self-test round-trips a scratch file instead of the real settings.
static void SaveCleanSelection(const std::wstring& path,
                               const std::vector<CleanGroup>& groups) {
    if (path.empty()) return;
    for (const auto& g : groups) {
        if (g.personal || g.id.empty()) continue;
        WritePrivateProfileStringW(L"Cleanup", g.id.c_str(), g.selected ? L"1" : L"0",
                                   path.c_str());
    }
}

static void LoadCleanSelection(const std::wstring& path, std::vector<CleanGroup>& groups) {
    if (path.empty()) return;
    for (auto& g : groups) {
        if (g.personal || g.id.empty()) continue;
        wchar_t buf[8] = {};
        if (GetPrivateProfileStringW(L"Cleanup", g.id.c_str(), L"", buf, 8, path.c_str()) == 0)
            continue;  // never chosen: keep the built-in default for this target
        g.selected = buf[0] == L'1';
    }
}

// Drops every stored tick, so the page and the next launch go back to what
// BuildCleanGroups asks for: caches ticked, the user's own folder unticked.
static void ResetCleanSelection(const std::wstring& path) {
    if (path.empty()) return;
    WritePrivateProfileStringW(L"Cleanup", nullptr, nullptr, path.c_str());  // whole section
}

// The Settings page's tray options. All three are off when the key is missing, so a
// profile written before this page existed keeps closing, minimising and starting as it
// always did.
static bool LoadWindowPref(const std::wstring& path, const wchar_t* key) {
    if (path.empty()) return false;
    return GetPrivateProfileIntW(L"Window", key, 0, path.c_str()) != 0;
}

// Toasts are the exception: they were on before the option existed, so a missing key has
// to mean on. Otherwise upgrading would silently stop the reports a tray clean gives.
static bool LoadToastPref(const std::wstring& path) {
    if (path.empty()) return true;
    return GetPrivateProfileIntW(L"Window", L"ToastNotifications", 1, path.c_str()) != 0;
}

static void SaveWindowPrefs(const std::wstring& path, bool closeToTray, bool minimizeToTray,
                            bool startInTray, bool toastNotifications) {
    if (path.empty()) return;
    WritePrivateProfileStringW(L"Window", L"CloseToTray", closeToTray ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(L"Window", L"MinimizeToTray", minimizeToTray ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(L"Window", L"StartInTray", startInTray ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(L"Window", L"ToastNotifications",
                               toastNotifications ? L"1" : L"0", path.c_str());
}

// The Cleanup page's Order switch is a view choice, so it lives in its own section:
// Reset to defaults drops the stored ticks and must not reorder the list as well. A
// missing key means the built-in default, biggest first.
static bool LoadCleanupSortBySize(const std::wstring& path) {
    if (path.empty()) return true;
    wchar_t buf[16] = {};
    if (GetPrivateProfileStringW(L"CleanupView", L"Order", L"", buf, 16, path.c_str()) == 0)
        return true;
    return std::wstring(buf) != L"builtin";
}

static void SaveCleanupSortBySize(const std::wstring& path, bool bySize) {
    if (path.empty()) return;
    WritePrivateProfileStringW(L"CleanupView", L"Order", bySize ? L"size" : L"builtin",
                               path.c_str());
}

// The page's bulk controls. Select all ticks every target that is actually there, but
// never the user's own files: those stay a deliberate, one-row choice, so a bulk press
// can never arm Downloads. Clear all clears everything, present or not.
static void ApplyBulkCleanSelection(std::vector<CleanGroup>& groups, bool select) {
    for (auto& g : groups) {
        if (!select)
            g.selected = false;
        else if (g.exists && !g.personal)
            g.selected = true;
    }
}

// A row carries a figure once a pass has measured it, or - after a stopped pass - once it
// still shows the figure from an earlier one. Those are the rows the size order compares.
static bool CleanupRowHasFigure(const CleanGroup& g) {
    return g.exists && (g.scanned || g.bytes > 0 || g.files > 0);
}

// The Cleanup page shows its rows biggest first. groups keeps its built-in order - the
// scan fills results in by position, the bulk controls and the saved ticks index into it -
// so the size order lives beside it as a list of row indices. Rows with nothing to compare
// (not on disk, or never measured and no earlier figure either) stay below the figures in
// their built-in order, so a press can never move a row the user is reading on their behalf.
static void SortCleanupRows(AppState& st) {
    std::vector<int> order;
    order.reserve(st.groups.size());
    for (int i = 0; i < (int)st.groups.size(); ++i) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&st](int a, int b) {
        const CleanGroup& ga = st.groups[(size_t)a];
        const CleanGroup& gb = st.groups[(size_t)b];
        bool fa = CleanupRowHasFigure(ga);
        bool fb = CleanupRowHasFigure(gb);
        if (fa != fb) return fa;
        if (!fa) return false;  // both without a figure: keep the built-in order
        return ga.bytes > gb.bytes;
    });
    st.cleanOrder = std::move(order);
}

// Applies the page's Order switch: biggest measured folder first, or the order the list
// was built in. Called wherever a new set of figures lands and whenever the switch is
// pressed, so the rows always follow the choice without waiting for a pass.
static void RefreshCleanupOrder(AppState& st) {
    if (st.cleanSortBySize) {
        SortCleanupRows(st);
        return;
    }
    std::vector<int> builtIn;
    builtIn.reserve(st.groups.size());
    for (int i = 0; i < (int)st.groups.size(); ++i) builtIn.push_back(i);
    st.cleanOrder = std::move(builtIn);
}

// Relaunches this executable with the runas verb. Returns true only when the
// elevated copy actually started (meaning this process should exit). On failure
// launchError receives the ShellExecuteEx error, so callers can tell a declined
// prompt (ERROR_CANCELLED) from elevation being unavailable.
static bool RelaunchElevated(const std::vector<std::wstring>& args,
                             DWORD* launchError = nullptr) {
    if (launchError) *launchError = 0;
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;

    std::wstring parameters;
    for (const auto& arg : args) {
        if (!parameters.empty()) parameters += L' ';
        parameters += QuoteArgument(arg);
    }

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = exe;
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        if (launchError) *launchError = GetLastError();
        return false;  // includes the user declining UAC
    }
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

// ------------------------------------------------------------------ workers
static void StartScan();
static void UpdateDynamicValues();

// The only cleans that can touch the user's own files ask first, naming the target, the
// size and where the files go. Two kinds need the warning: a personal folder, whose files
// land in the Recycle Bin and can be put back, and the Recycle Bin itself, which can only
// be emptied. Caches never prompt, so the usual clean stays one click. The text is built
// apart from the dialog so the self-test can read it without a window.
static std::wstring PersonalCleanPrompt(const std::vector<CleanGroup>& targets) {
    std::vector<const CleanGroup*> moved;
    std::vector<const CleanGroup*> emptied;
    for (const auto& g : targets) {
        if (!g.personal || (g.bytes == 0 && g.files == 0)) continue;  // nothing to remove
        (g.recycleBin ? emptied : moved).push_back(&g);
    }
    if (moved.empty() && emptied.empty()) return std::wstring();

    auto subject = [](const std::vector<const CleanGroup*>& list) {
        std::wstring text = list[0]->name;
        for (size_t i = 1; i < list.size(); ++i)
            text += (i + 1 == list.size() ? L" and " : L", ") + list[i]->name;
        return text + (list.size() == 1 ? L" is ticked: " : L" are ticked: ");
    };
    auto totals = [](const std::vector<const CleanGroup*>& list, uint64_t& bytes,
                     uint32_t& files) {
        for (const CleanGroup* g : list) {
            bytes += g->bytes;
            files += g->files;
        }
    };

    std::wstring prompt;
    if (!moved.empty()) {
        uint64_t bytes = 0;
        uint32_t files = 0;
        totals(moved, bytes, files);
        prompt = subject(moved) + FormatBytes(bytes) + L" in " + FormatCount(files) +
                 L" files will be moved to the Recycle Bin, where you can put them back.";
    }
    if (!emptied.empty()) {
        uint64_t bytes = 0;
        uint32_t files = 0;
        totals(emptied, bytes, files);
        if (!prompt.empty()) prompt += L"\n";
        prompt += subject(emptied) + FormatBytes(bytes) + L" in " + FormatCount(files) +
                  L" items will be deleted for good - this cannot be undone.";
    }
    return prompt + L"\n\nClean them now?";
}

// No is the default button: the reversible choice is the one a stray Enter takes.
static bool ConfirmPersonalClean(const std::vector<CleanGroup>& targets) {
    std::wstring prompt = PersonalCleanPrompt(targets);
    if (prompt.empty()) return true;
    return MessageBoxW(g_hwnd, prompt.c_str(), L"Clean your own files?",
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

static void StartClean() {
    if (g_workerRunning.load() || !g_app->hasScanned) return;
    std::vector<CleanGroup> targets;
    for (const auto& g : g_app->groups)
        // Only what this page has measured: a row left unmeasured by a stopped pass must
        // not be cleaned behind a figure the user never saw.
        if (g.selected && g.exists && g.scanned) targets.push_back(g);
    if (targets.empty()) return;
    // Downloads and the Recycle Bin are the destructive non-cache rows; ask about them.
    if (!ConfirmPersonalClean(targets)) return;

    g_workerRunning.store(true);
    g_app->cleaning = true;
    g_app->showCleanResult = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);

    std::thread([targets]() {
        CleanResult total;
        for (const auto& group : targets) {
            // Downloads goes to the Recycle Bin, the Recycle Bin empties, caches delete.
            CleanResult r = CleanGroupContents(group);
            total.bytesFreed += r.bytesFreed;
            total.bytesRecycled += r.bytesRecycled;
            total.deleted += r.deleted;
            total.skipped += r.skipped;
        }
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_cleanResult = total;
        }
        g_workerRunning.store(false);
        PostMessageW(g_hwnd, WM_APP_CLEAN_DONE, 0, 0);
    }).detach();
}

static void StartScan() {
    if (g_workerRunning.load()) return;
    g_workerRunning.store(true);
    g_scanCancel.store(false);
    g_app->scanning = true;
    g_app->scanStopping = false;
    g_app->scanCancelled = false;  // pressing Scan now is the user asking for a pass
    // Every row starts this pass unmeasured, which the page paints as "Measuring..."
    // until that row's own figures arrive; the figures from a previous pass are replaced
    // rather than left sitting under a new pass they no longer describe.
    for (auto& g : g_app->groups) g.scanned = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);

    std::vector<CleanGroup> snapshot = g_app->groups;
    {
        std::lock_guard<std::mutex> lock(g_workerMutex);
        g_scanLive = snapshot;
    }
    std::thread([snapshot]() mutable {
        // Back to front: the list is built with the heavy user caches first, so walking it
        // the other way lets the quick rows land while the big folders are still being
        // measured, and the "n of m" line moves instead of sitting at zero and jumping.
        // Results keep their place in the list either way.
        bool cancelled = false;
        for (int i = (int)snapshot.size() - 1; i >= 0; --i) {
            if (g_scanCancel.load()) {
                cancelled = true;
                break;
            }
            ScanCleanGroup(snapshot[(size_t)i], &g_scanCancel);
            if (!snapshot[(size_t)i].scanned) {  // stopped inside this row
                cancelled = true;
                break;
            }
            // One row is done: hand the whole list so far to the page, which shows the
            // finished rows filling in behind the ones still being walked.
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                g_scanLive = snapshot;
            }
            PostMessageW(g_hwnd, WM_APP_SCAN_PROGRESS, 0, 0);
        }
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_scanResult = std::move(snapshot);
        }
        g_workerRunning.store(false);
        PostMessageW(g_hwnd, WM_APP_SCAN_DONE, cancelled ? 1 : 0, 0);
    }).detach();
}

// The Cleanup page measures itself the first time it is shown, so it never sits on
// "Nothing scanned yet" waiting for a button press. It is called when the page opens and
// once a second while it is open: the retry matters when another job (a large-file scan,
// a memory clean) still holds the worker at the moment the page appears, because the
// measurement must not be dropped just because it could not start that instant. Once
// anything has been measured the page keeps its figures - the quiet scan keeps them
// fresh - so reopening it never triggers another full measurement.
static void EnsureCleanupMeasured() {
    if (!g_app || g_app->page != Page::Cleanup) return;
    if (g_app->hasScanned || g_app->scanning || g_app->cleaning) return;
    if (g_workerRunning.load()) return;
    // A pass the user stopped stays stopped until they press Scan now: restarting it a
    // second after the cancel would make the button pointless.
    if (g_app->scanCancelled) return;
    StartScan();
}

// Keeps the Cleanup page's figures current while the page is open. It never touches
// the busy flags, the buttons or the result banner, and stands down whenever a real
// scan or a clean is running, so nothing jumps while the user is acting on it.
static void StartQuietScan() {
    if (!g_app || g_app->page != Page::Cleanup) return;
    if (!g_app->hasScanned || g_app->scanning || g_app->cleaning) return;
    // A stopped pass is not quietly resumed behind the user's back; the figures on the
    // page stay as the stop left them until Scan now asks for a fresh pass.
    if (g_app->scanCancelled) return;
    if (g_workerRunning.load() || g_quietScanActive.load() || g_cleanRefreshActive.load())
        return;

    g_quietScanActive.store(true);
    std::vector<CleanGroup> snapshot = g_app->groups;
    std::thread([snapshot]() mutable {
        ULONGLONG started = GetTickCount64();
        for (auto& group : snapshot) ScanCleanGroup(group);
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_quietScanResult = std::move(snapshot);
        }
        // Cheap folders refresh every 5 s; a tree that takes seconds to measure backs
        // off, so a huge temp folder is never walked continuously.
        ULONGLONG ms = GetTickCount64() - started;
        unsigned secs = (unsigned)(ms / 200);
        if (secs < kQuietScanMinSeconds) secs = kQuietScanMinSeconds;
        if (secs > kQuietScanMaxSeconds) secs = kQuietScanMaxSeconds;
        g_quietInterval.store(secs);
        g_quietScanActive.store(false);
        PostMessageW(g_hwnd, WM_APP_QUIET_SCAN_DONE, 0, 0);
    }).detach();
}

// Takes a freshly measured list and puts it on the page, carrying the ticks the user
// has made by id so a refresh never arms something they unticked - including the rows
// that were just cleaned and cleared, which stay clear.
static void ApplyCleanRefresh(std::vector<CleanGroup> fresh) {
    if (!g_app || fresh.empty()) return;
    for (auto& group : fresh) {
        for (const auto& current : g_app->groups) {
            if (current.id != group.id) continue;
            group.selected = current.selected;
            break;
        }
    }
    g_app->groups = std::move(fresh);
    g_app->hasScanned = true;
    // A new set of figures is the moment the size order is allowed to move: the pass is
    // over, so nothing reorders in the middle of one.
    RefreshCleanupOrder(*g_app);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// A finished clean leaves the rows describing files that are gone. This rebuilds the
// list from what is on disk now and measures every target again, so the page shows
// real leftovers (including anything a locked file kept) instead of the figures from
// before the clean. It never takes the page busy: Scan stays usable throughout, and
// the refreshed rows land when the pass finishes.
static void StartCleanRefresh() {
    if (!g_app) return;
    if (g_workerRunning.load() || g_cleanRefreshActive.load()) return;  // a scan covers it

    g_cleanRefreshActive.store(true);
    // The list is built on the UI thread, beside the other shell lookups; the
    // worker only walks the folders.
    std::vector<CleanGroup> fresh = BuildCleanGroups();
    std::thread([fresh]() mutable {
        for (auto& group : fresh) ScanCleanGroup(group);
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_cleanRefreshResult = std::move(fresh);
        }
        g_cleanRefreshActive.store(false);
        PostMessageW(g_hwnd, WM_APP_CLEAN_REFRESH_DONE, 0, 0);
    }).detach();
}

// A finished clean: report what it freed, leave nothing armed for a second click,
// and put the page back in its idle state before the items refresh arrives.
static void FinishClean() {
    if (!g_app) return;
    CleanResult result;
    {
        std::lock_guard<std::mutex> lock(g_workerMutex);
        result = g_cleanResult;
    }
    g_app->cleaning = false;
    g_app->scanning = false;
    g_app->hasScanned = true;
    g_app->showCleanResult = true;
    g_app->cleanMessageIsError = false;
    std::wstring message;
    if (result.bytesFreed > 0) message = L"Freed " + FormatBytes(result.bytesFreed);
    if (result.bytesRecycled > 0) {
        std::wstring recycled = FormatBytes(result.bytesRecycled) + L" to the Recycle Bin";
        message += message.empty() ? L"Moved " + recycled : L", moved " + recycled;
    }
    if (message.empty()) message = L"Nothing needed cleaning";
    message += L" from " + FormatCount(result.deleted) + L" files";
    if (result.skipped > 0)
        message += L"; " + FormatCount(result.skipped) + L" in use and left alone";
    message += L".";
    g_app->cleanMessage = message;
    g_app->lastFreed = result.bytesFreed;
    g_app->lastDeleted = result.deleted;
    g_app->lastSkipped = result.skipped;
    // The cleaned folders are now empty (or partly locked), so drop their old figures
    // and leave them unchecked: the list is about to be measured again, and nothing
    // should stay armed for a second destructive click.
    for (auto& group : g_app->groups) {
        if (group.selected && group.exists) {
            group.bytes = 0;
            group.files = 0;
            group.selected = false;
        }
    }
    UpdateDynamicValues();  // freed space shows on the disk cards at once
    InvalidateRect(g_hwnd, nullptr, FALSE);
    // The page stays idle and scannable - no busy flag is set - while the items are
    // rebuilt and re-measured, so Scan is usable the moment cleaning ends.
    StartCleanRefresh();
}

static void FinishCleanRefresh() {
    if (!g_app) return;
    std::vector<CleanGroup> fresh;
    {
        std::lock_guard<std::mutex> lock(g_workerMutex);
        fresh = std::move(g_cleanRefreshResult);
    }
    ApplyCleanRefresh(std::move(fresh));
}

static void StartMemoryClean(bool deep) {
    if (!g_app || g_app->memBusy || g_workerRunning.load()) return;
    g_workerRunning.store(true);
    g_app->memBusy = true;
    g_app->memMessage.clear();
    g_app->memMessageIsError = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);

    std::thread([deep]() {
        MemCleanResult result = deep ? DeepCleanMemory() : TrimWorkingSets(false);
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_memResult = result;
        }
        g_workerRunning.store(false);
        PostMessageW(g_hwnd, WM_APP_MEM_DONE, 0, 0);
    }).detach();
}

// ------------------------------------------------- browsers / dns / boost
static void SaveDnsSettings() {
    if (!g_app) return;
    std::wstring path = SettingsPath();
    if (path.empty()) return;
    auto writeInt = [&](const wchar_t* name, int value) {
        WritePrivateProfileStringW(L"Dns", name, std::to_wstring(value).c_str(), path.c_str());
    };
    writeInt(L"Applied", g_app->dnsApplied ? 1 : 0);
    writeInt(L"Watchdog", g_app->dnsWatchdog ? 1 : 0);
    writeInt(L"Provider", g_app->dnsProvider);
    WritePrivateProfileStringW(L"Dns", L"Server1", g_app->dnsPrimary.c_str(), path.c_str());
    WritePrivateProfileStringW(L"Dns", L"Server2", g_app->dnsSecondary.c_str(), path.c_str());
}

static void LoadDnsSettings(AppState& app) {
    std::wstring path = SettingsPath();
    if (path.empty()) return;
    auto readText = [&](const wchar_t* name) {
        wchar_t buf[128] = {};
        GetPrivateProfileStringW(L"Dns", name, L"", buf, 128, path.c_str());
        return std::wstring(buf);
    };
    app.dnsApplied = GetPrivateProfileIntW(L"Dns", L"Applied", 0, path.c_str()) != 0;
    app.dnsWatchdog = GetPrivateProfileIntW(L"Dns", L"Watchdog", 0, path.c_str()) != 0;
    app.dnsProvider = (int)GetPrivateProfileIntW(L"Dns", L"Provider", 1, path.c_str());
    app.dnsPrimary = readText(L"Server1");
    app.dnsSecondary = readText(L"Server2");
}

static void StartDnsWatchdog() {
    if (!g_app || g_dnsWatchdogRunning.load() || g_app->dnsPrimary.empty()) return;
    std::wstring primary = g_app->dnsPrimary;
    std::wstring secondary = g_app->dnsSecondary;
    g_dnsWatchdogRunning.store(true);
    std::thread([primary, secondary]() {
        while (g_dnsWatchdogRunning.load()) {
            uint32_t changed = DnsWatchdogTick(primary, secondary);
            if (changed) g_dnsIncidentCount.fetch_add(changed);
            Sleep(1500);
        }
    }).detach();
}

static void StartBrowserScan() {
    if (!g_app || g_app->browserScanning || g_app->browserCleaning) return;
    if (g_workerRunning.load() || g_app->browsers.empty()) return;

    g_workerRunning.store(true);
    g_app->browserScanning = true;
    g_app->browserMessage.clear();
    g_app->browserMessageIsError = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);

    std::vector<BrowserEntry> snapshot = g_app->browsers;
    std::thread([snapshot]() mutable {
        for (auto& entry : snapshot)
            if (entry.installed) ScanBrowserEntry(entry);
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_browserScanResult = std::move(snapshot);
        }
        g_workerRunning.store(false);
        PostMessageW(g_hwnd, WM_APP_BROWSER_SCAN_DONE, 0, 0);
    }).detach();
}

static void StartBrowserClean() {
    if (!g_app || g_app->browserCleaning || g_app->browserScanning) return;
    if (g_workerRunning.load()) return;

    bool any = false;
    for (const auto& browser : g_app->browsers)
        if (browser.selected && browser.installed && !browser.running && browser.bytes > 0)
            any = true;
    if (!any) return;

    g_workerRunning.store(true);
    g_app->browserCleaning = true;
    g_app->browserMessage.clear();
    InvalidateRect(g_hwnd, nullptr, FALSE);

    std::vector<BrowserEntry> snapshot = g_app->browsers;
    std::thread([snapshot]() mutable {
        BrowserCleanResult result = CleanBrowserEntries(snapshot);
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_browserCleanResult = result;
        }
        g_workerRunning.store(false);
        PostMessageW(g_hwnd, WM_APP_BROWSER_CLEAN_DONE, 0, 0);
    }).detach();
}

static void StartBoost(bool restore) {
    if (!g_app || g_app->boostBusy || g_workerRunning.load()) return;
    g_workerRunning.store(true);
    g_app->boostBusy = true;
    InvalidateRect(g_hwnd, nullptr, FALSE);

    std::thread([restore]() {
        BoostResult result = restore ? RestoreBoost() : ApplyBoost();
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_boostResult = result;
        }
        g_workerRunning.store(false);
        PostMessageW(g_hwnd, WM_APP_BOOST_DONE, 0, 0);
    }).detach();
}

static void StartLargeScan() {
    if (g_workerRunning.load()) return;
    g_workerRunning.store(true);
    g_app->largeScanning = true;
    g_app->largeMessage.clear();
    g_app->largeMessageIsError = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);

    std::vector<std::wstring> roots = LargeFileRoots();
    g_app->largeRoots = roots;
    LargeFileScan options;
    options.minBytes = g_app->largeThreshold;
    std::thread([roots, options]() {
        std::vector<LargeFile> found;
        ScanLargeFiles(roots, options, found);
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_largeScanResult = std::move(found);
        }
        g_workerRunning.store(false);
        PostMessageW(g_hwnd, WM_APP_LARGE_SCAN_DONE, 0, 0);
    }).detach();
}

static void StartLargeDelete() {
    if (g_workerRunning.load()) return;
    std::vector<std::wstring> selected;
    for (const auto& file : g_app->largeFiles)
        if (file.selected) selected.push_back(file.path);
    if (selected.empty()) return;

    g_workerRunning.store(true);
    g_app->largeDeleting = true;
    g_app->largeMessage.clear();
    InvalidateRect(g_hwnd, nullptr, FALSE);

    std::vector<std::wstring> roots = g_app->largeRoots;
    std::thread([selected, roots]() {
        LargeDeleteResult result = RemoveLargeFiles(selected, roots);
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_largeDeleteResult = result;
        }
        g_workerRunning.store(false);
        PostMessageW(g_hwnd, WM_APP_LARGE_DELETE_DONE, 0, 0);
    }).detach();
}

// ---------------------------------------------------------------- interaction
static int HitTest(const std::vector<Hit>& hits, POINT pt, bool* enabledOut = nullptr) {
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        if (PtInRect(&it->rect, pt)) {
            if (enabledOut) *enabledOut = it->enabled;
            return it->id;
        }
    }
    if (enabledOut) *enabledOut = false;
    return HitNone;
}

static void RefreshPage() {
    if (!g_app) return;
    switch (g_app->page) {
        case Page::Dashboard:
        case Page::System:
            LoadSystemPage(*g_app);
            break;
        case Page::Startup:
            LoadStartupPage(*g_app);
            break;
        case Page::Cleanup:
            if (!g_app->scanning) StartScan();
            break;
        case Page::Memory:
            LoadMemoryPage(*g_app);
            break;
        case Page::Deactivate:
            LoadDeactivatePage(*g_app);
            break;
        case Page::Browsers:
            LoadBrowsersPage(*g_app);
            StartBrowserScan();
            break;
        case Page::Dns:
            LoadDnsPage(*g_app);
            break;
        case Page::Network:
            LoadNetPage();
            break;
        case Page::Boost:
            LoadBoosterPage();
            break;
        case Page::Services:
            ReloadServices();
            break;
        case Page::Tasks:
            LoadTasksPage(*g_app);
            break;
        case Page::LargeFiles:
            // Refresh means the same thing here as Scan now: measure the folders again
            // with the current threshold. A scan already in flight is left alone.
            if (!g_app->largeScanning) StartLargeScan();
            break;
        case Page::Settings:
            break;
    }
}

static void NavigateTo(Page page) {
    if (!g_app) return;
    g_app->page = page;
    // Leaving the page ends a stop: coming back to Cleanup is a new visit, and it measures
    // itself like any other first visit.
    if (page != Page::Cleanup) g_app->scanCancelled = false;
    if (page == Page::Cleanup) EnsureCleanupMeasured();
    if (page == Page::Startup) LoadStartupPage(*g_app);
    if (page == Page::Dashboard || page == Page::System || page == Page::Storage)
        LoadSystemPage(*g_app);
    if (page == Page::Memory) LoadMemoryPage(*g_app);
    if (page == Page::Deactivate) LoadDeactivatePage(*g_app);
    if (page == Page::Browsers) {
        LoadBrowsersPage(*g_app);
        StartBrowserScan();
    }
    if (page == Page::Dns) LoadDnsPage(*g_app);
    if (page == Page::Network) {
        g_app->netMessage.clear();
        g_app->netMessageIsError = false;
        LoadNetPage();
    }
    if (page == Page::Boost) {
        // Arriving is a fresh visit, so the previous outcome does not follow the user in.
        g_app->boosterMessage.clear();
        g_app->boosterMessageIsError = false;
        LoadBoosterPage();
    }
    if (page == Page::Services) {
        // "Refresh" is the only thing that clears a stale banner, but arriving on the page
        // is a fresh visit, so the last action's message does not follow the user in.
        g_app->servicesMessage.clear();
        g_app->servicesMessageIsError = false;
        ReloadServices();
    }
    if (page == Page::Tasks) {
        g_app->tasksMessage.clear();
        g_app->tasksMessageIsError = false;
        LoadTasksPage(*g_app);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Changing a service writes to the service database, which standard users cannot do.
// Explains that and offers the same one-click restart as the Memory page.
static bool ConfirmAdminRights(const std::wstring& what, const wchar_t* title) {
    if (!g_app || g_app->elevated) return true;
    std::wstring message = what +
                           L" needs administrator rights.\n\nRestart Zanphar's WinSuite as "
                           L"administrator now?";
    if (MessageBoxW(g_hwnd, message.c_str(), title, MB_YESNO | MB_ICONINFORMATION) != IDYES)
        return false;

    DWORD launchError = 0;
    if (RelaunchElevated({}, &launchError)) {
        SetStandardModePreference(false);
        DestroyWindow(g_hwnd);  // the elevated copy takes over
        return false;
    }
    MessageBoxW(g_hwnd, L"Administrator rights were not granted, so nothing was changed.", title,
                MB_OK | MB_ICONINFORMATION);
    return false;
}

static bool ConfirmServiceRights(const std::wstring& what) {
    return ConfirmAdminRights(what, L"Deactivate");
}

static std::wstring StorageMediaText(const StorageDevice& device) {
    std::wstring media = !device.mediaKnown ? L"media type unknown"
                          : device.ssd      ? L"solid-state drive"
                                           : L"mechanical hard disk";
    if (!device.typeName.empty()) media += L" / " + device.typeName;
    if (!device.bus.empty()) media += L" / " + device.bus;
    if (!device.model.empty()) media += L" / " + device.model;
    return media;
}

static std::wstring StorageVolumeText(const StorageDevice& device) {
    std::wstring name = device.root;
    if (!device.label.empty()) name += L" (" + device.label + L")";
    return name;
}

static std::wstring StorageModeText(const StorageDevice& device) {
    return device.ssd ? L"Retrim (TRIM, /L)" : L"Defragment (/D /H)";
}

static std::wstring StorageElapsedText(ULONGLONG started) {
    const ULONGLONG seconds = started ? (GetTickCount64() - started) / 1000 : 0;
    wchar_t buf[32];
    swprintf(buf, 32, L"%llu:%02llu", (unsigned long long)(seconds / 60),
             (unsigned long long)(seconds % 60));
    return buf;
}

static void UpdateStorageProgress() {
    if (!g_app || !g_app->storageBusy) return;
    std::wstring text = g_app->storageAction + L" - " + StorageElapsedText(g_app->storageStarted) +
                        L" elapsed";
    const int percent = g_storagePercent.load();
    g_app->storagePercent = percent;
    if (percent >= 0) text += L" - " + std::to_wstring(percent) + L"%";
    g_app->storageMessage = text;
    g_app->storageMessageIsError = false;
}

static void CancelStorageOptimize() {
    if (!g_app || !g_app->storageBusy) return;
    CancelVolumeOptimize();
    g_app->storageMessage = L"Cancelling " + g_app->storageAction + L"...";
    g_app->storageMessageIsError = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// defrag.exe needs an elevated token for the volume it opens, so the Storage page
// asks once and restarts the suite the same way the service actions do.
static void StartStorageOptimize(int slot) {
    if (!g_app || g_app->storageBusy) return;
    if (slot < 0 || slot >= 26) return;
    const wchar_t letter = (wchar_t)(L'A' + slot);
    auto found = std::find_if(g_app->storage.begin(), g_app->storage.end(),
                              [letter](const StorageDevice& d) {
                                  return !d.root.empty() &&
                                         std::towupper(d.root[0]) == letter;
                              });
    if (found == g_app->storage.end()) return;
    const StorageDevice device = *found;
    if (g_workerRunning.load()) {
        g_app->storageMessage = L"Another job is still running - try again in a moment.";
        g_app->storageMessageIsError = true;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    const std::wstring refusal = StorageOptimizeRefusal(device);
    if (!refusal.empty()) {
        g_app->storageMessage = refusal;
        g_app->storageMessageIsError = true;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    const std::wstring volume = StorageVolumeText(device);
    const std::wstring mode = StorageModeText(device);
    std::wstring prompt =
        L"Optimize " + volume + L"?\n\n" + L"Media: " + StorageMediaText(device) + L"\n" +
        L"Mode: " + mode + L"\n\n" +
        (device.ssd ? L"TRIM tells the drive which blocks are no longer in use; it does not "
                      L"delete files.\n\n"
                    : L"Defragmentation can take minutes on a large or busy disk.\n\n") +
        L"All Storage actions will be disabled until it finishes. You can cancel from "
        L"the Storage page.\n\nStart now?";
    if (MessageBoxW(g_hwnd, prompt.c_str(), L"Confirm storage optimization",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        g_app->storageMessage = L"Optimization cancelled - nothing was changed.";
        g_app->storageMessageIsError = false;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (!ConfirmAdminRights(L"Optimizing " + volume + L" (" + mode + L")", L"Storage")) {
        g_app->storageMessage =
            L"Nothing was changed: optimizing a volume needs administrator rights.";
        g_app->storageMessageIsError = true;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (g_workerRunning.load() || g_app->storageBusy) return;

    ResetVolumeOptimize();
    g_storagePercent.store(-1);
    g_app->storagePercent = -1;
    g_workerRunning.store(true);
    g_app->storageBusy = true;
    g_app->storageStarted = GetTickCount64();
    g_app->storageAction =
        (device.ssd ? L"Trimming " : L"Defragmenting ") + device.root;
    g_app->storageMessage = g_app->storageAction + L"...";
    g_app->storageMessageIsError = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);

    std::thread([device]() {
        OptimizeOutcome outcome = OptimizeVolume(device, [](int percent) {
            g_storagePercent.store(percent);
        });
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_storageOutcome = outcome;
        }
        g_workerRunning.store(false);
        PostMessageW(g_hwnd, WM_APP_STORAGE_DONE, 0, 0);
    }).detach();
}

// ------------------------------------------------------------ modal form
// A small owned window with one edit control per field, so creating a service or starting
// a task can ask for a name or a command line. It runs its own message loop, which makes
// it modal without a dialog resource, and it uses the suite's colours so it does not open
// a white rectangle over a dark window.
struct FormSpec {
    std::wstring title;
    std::wstring hint;
    std::vector<std::wstring> labels;
    std::vector<std::wstring> values;
    std::vector<bool> readOnly;
};

static FormSpec* g_form = nullptr;
static std::vector<HWND> g_formEdits;
static bool g_formAccepted = false;
static bool g_formClassReady = false;
static HBRUSH g_formBack = nullptr;
static HBRUSH g_formField = nullptr;

static void FormCollect() {
    if (!g_form) return;
    for (size_t i = 0; i < g_formEdits.size() && i < g_form->values.size(); ++i) {
        int len = GetWindowTextLengthW(g_formEdits[i]);
        std::wstring text((size_t)len + 1, L'\0');
        GetWindowTextW(g_formEdits[i], text.data(), len + 1);
        text.resize((size_t)len);
        g_form->values[i] = text;
    }
}

static LRESULT CALLBACK FormProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                FormCollect();
                g_formAccepted = true;
                DestroyWindow(hwnd);
            } else if (LOWORD(wp) == IDCANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (wp == VK_RETURN) {
                FormCollect();
                g_formAccepted = true;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, g_formBack);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
            SetTextColor((HDC)wp, theme::muted);
            SetBkColor((HDC)wp, theme::bg);
            return (LRESULT)g_formBack;
        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)wp, theme::text);
            SetBkColor((HDC)wp, RGB(20, 26, 33));
            return (LRESULT)g_formField;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool RunForm(FormSpec& spec) {
    if (!g_formClassReady) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = FormProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"WinSuiteForm";
        RegisterClassExW(&wc);
        g_formClassReady = true;
    }

    g_form = &spec;
    g_formAccepted = false;
    g_formEdits.clear();
    g_formBack = CreateSolidBrush(theme::bg);
    g_formField = CreateSolidBrush(RGB(20, 26, 33));

    const int pad = Scaled(22);
    const int rowH = Scaled(54);
    const int labelH = Scaled(18);
    const int fieldH = Scaled(26);
    const int width = Scaled(566);
    const int hintH = spec.hint.empty() ? 0 : Scaled(38);
    const int height = pad + hintH + (int)spec.values.size() * rowH + Scaled(46) + pad;

    RECT owner = {0, 0, 0, 0};
    if (g_hwnd) GetWindowRect(g_hwnd, &owner);
    int left = owner.left + ((owner.right - owner.left) - width) / 2;
    int top = owner.top + ((owner.bottom - owner.top) - height) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"WinSuiteForm", spec.title.c_str(),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, left, top, width, height,
                                g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        DeleteObject(g_formBack);
        DeleteObject(g_formField);
        g_form = nullptr;
        return false;
    }

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int y = pad;
    if (!spec.hint.empty()) {
        HWND hint = CreateWindowExW(0, L"STATIC", spec.hint.c_str(),
                                    WS_CHILD | WS_VISIBLE | SS_LEFT, pad, y, width - 2 * pad,
                                    Scaled(32), hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hint, WM_SETFONT, (WPARAM)font, TRUE);
        y += hintH;
    }
    for (size_t i = 0; i < spec.values.size(); ++i) {
        HWND label = CreateWindowExW(0, L"STATIC", spec.labels[i].c_str(),
                                     WS_CHILD | WS_VISIBLE | SS_LEFT, pad, y, width - 2 * pad,
                                     labelH, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
        if (!spec.readOnly.empty() && spec.readOnly[i]) style |= ES_READONLY;
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", spec.values[i].c_str(), style,
                                    pad, y + labelH, width - 2 * pad, fieldH, hwnd,
                                    (HMENU)(INT_PTR)(100 + (int)i), nullptr, nullptr);
        SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
        g_formEdits.push_back(edit);
        y += rowH;
    }

    const int btnW = Scaled(96), btnH = Scaled(30);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"Save",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              width - pad - btnW * 2 - Scaled(8), y + Scaled(4), btnW, btnH,
                              hwnd,
                              (HMENU)IDOK, nullptr, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  width - pad - btnW, y + Scaled(4), btnW, btnH, hwnd,
                                  (HMENU)IDCANCEL, nullptr, nullptr);
    SendMessageW(ok, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);

    if (g_hwnd) EnableWindow(g_hwnd, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    HWND focus = g_formEdits.empty() ? ok : g_formEdits.front();
    for (size_t i = 0; i < g_formEdits.size(); ++i) {
        if (spec.readOnly.empty() || !spec.readOnly[i]) {
            focus = g_formEdits[i];
            break;
        }
    }
    SetFocus(focus);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && IsChild(hwnd, msg.hwnd)) {
            if (msg.wParam == VK_RETURN) {
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                continue;
            }
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_hwnd) {
        EnableWindow(g_hwnd, TRUE);
        SetForegroundWindow(g_hwnd);
    }
    DeleteObject(g_formBack);
    DeleteObject(g_formField);
    g_formBack = nullptr;
    g_formField = nullptr;
    g_formEdits.clear();
    g_form = nullptr;
    return g_formAccepted;
}

// ------------------------------------------------------- services and tasks
static std::wstring LowerText(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), (DWORD)text.size());
    return text;
}

static const TaskRule* RuleFor(const std::vector<TaskRule>& rules, const std::wstring& exe) {
    std::wstring lower = LowerText(exe);
    for (const auto& rule : rules)
        if (rule.exe == lower) return &rule;
    return nullptr;
}

static bool SaveTaskRule(const std::wstring& exe, ULONG_PTR affinity, DWORD priority) {
    std::wstring lower = LowerText(exe);
    if (lower.empty()) return false;
    std::vector<TaskRule> rules = LoadTaskRules(SettingsPath());
    bool replaced = false;
    for (auto& rule : rules) {
        if (rule.exe != lower) continue;
        rule.affinity = affinity;
        rule.priority = priority;
        replaced = true;
        break;
    }
    if (!replaced) {
        TaskRule rule;
        rule.exe = lower;
        rule.affinity = affinity;
        rule.priority = priority;
        rules.push_back(rule);
    }
    SaveTaskRules(SettingsPath(), rules);
    g_app->taskRules = rules;
    // The suite is running, so the rule is put on the matching processes straight away;
    // the agent is what carries it to the next sign-in.
    for (const auto& rule : rules)
        if (rule.exe == lower) ApplyTaskRules({rule});
    std::wstring error;
    if (!RegisterTaskAgent(true, error)) {
        g_app->tasksMessage = L"Rule saved, but it will not be re-applied at sign-in: " + error;
        g_app->tasksMessageIsError = true;
    } else if (!EnsureTaskAgentRunning()) {
        // The rule is on disk and in the Run key; only this session's later launches are
        // uncovered until the helper can be started.
        g_app->tasksMessage = L"Rule saved, but the helper that re-applies it to programs "
                              L"started from now on could not be started.";
        g_app->tasksMessageIsError = true;
    }
    return true;
}

static bool ForgetTaskRule(const std::wstring& exe) {
    std::wstring lower = LowerText(exe);
    std::vector<TaskRule> rules = LoadTaskRules(SettingsPath());
    size_t before = rules.size();
    rules.erase(std::remove_if(rules.begin(), rules.end(),
                               [&](const TaskRule& rule) { return rule.exe == lower; }),
                rules.end());
    if (rules.size() == before) return false;
    SaveTaskRules(SettingsPath(), rules);
    g_app->taskRules = rules;
    if (rules.empty()) {
        std::wstring error;
        RegisterTaskAgent(false, error);  // nothing left to apply at sign-in
    }
    return true;
}

static void ReloadServices(const std::wstring& keepName) {
    std::wstring name = keepName;
    if (name.empty() && g_app->serviceSelected >= 0 &&
        g_app->serviceSelected < (int)g_app->serviceRows.size())
        name = g_app->serviceRows[(size_t)g_app->serviceSelected].name;
    g_app->serviceRows = LoadAllServices();
    g_app->servicesLoaded = true;
    g_app->serviceSelected = -1;
    for (size_t i = 0; i < g_app->serviceRows.size(); ++i) {
        if (_wcsicmp(g_app->serviceRows[i].name.c_str(), name.c_str()) == 0) {
            g_app->serviceSelected = (int)i;
            break;
        }
    }
    if (g_app->serviceSelected < 0 && !g_app->serviceRows.empty()) g_app->serviceSelected = 0;

    // Ticks are kept by name, so a refresh leaves them where they were; an entry that has
    // gone (deleted or hidden) takes its tick with it.
    std::vector<std::wstring> kept;
    for (const auto& service : g_app->serviceRows)
        if (IsServiceChecked(*g_app, service.name)) kept.push_back(service.name);
    g_app->serviceChecked.swap(kept);
}

static std::vector<ServiceRow> CheckedServiceRows() {
    std::vector<ServiceRow> out;
    for (const auto& service : g_app->serviceRows)
        if (IsServiceChecked(*g_app, service.name)) out.push_back(service);
    return out;
}

static const ServiceRow* SelectedService() {
    if (g_app->serviceSelected < 0 || g_app->serviceSelected >= (int)g_app->serviceRows.size())
        return nullptr;
    return &g_app->serviceRows[(size_t)g_app->serviceSelected];
}

static void SelectTaskRow(int index) {
    if (index < 0 || index >= (int)g_app->taskRows.size()) return;
    g_app->taskSelected = index;
    const ProcRow& row = g_app->taskRows[(size_t)index];
    g_app->taskCpuMask = row.affinity;
    g_app->taskPriority = row.priority;
    g_app->taskRemember = RuleFor(g_app->taskRules, row.name) != nullptr;
}

static void LoadTasksPage(AppState& st) {
    // Only a real selection is followed across a refresh: with nothing selected there is
    // no pid to look for, and pid 0 would match the idle process and quietly select it.
    const bool hadSelection =
        st.taskSelected >= 0 && st.taskSelected < (int)st.taskRows.size();
    TaskRef keep;
    if (hadSelection) {
        keep.pid = st.taskRows[(size_t)st.taskSelected].pid;
        keep.created = st.taskRows[(size_t)st.taskSelected].created;
    }
    st.taskRows = LoadProcessRows();
    st.tasksLoaded = true;
    int found = -1;
    for (size_t i = 0; hadSelection && i < st.taskRows.size(); ++i) {
        // The creation time is part of the identity: a reused pid must not inherit the
        // highlight from the process that has just ended.
        if (st.taskRows[i].pid == keep.pid && st.taskRows[i].created == keep.created) {
            found = (int)i;
            break;
        }
    }
    st.taskSelected = -1;
    if (found >= 0) SelectTaskRow(found);

    // A tick belongs to one process instance; anything that has ended drops it on the next
    // read, so a bulk action can never reach a process that only shares the number.
    std::vector<TaskRef> kept;
    for (const auto& process : st.taskRows)
        if (process.pid > 4 && IsTaskChecked(st, process.pid, process.created))
            kept.push_back({process.pid, process.created});
    st.taskChecked.swap(kept);
}

static void LoadNetPage() {
    if (!g_app) return;
    g_app->netTweaks = LoadNetTweaks();
}

static void LoadBoosterPage() {
    if (!g_app) return;
    g_app->boost = LoadBoostState();
    g_app->gameTweaks = LoadGameTweaks();
    g_app->gameServices = LoadGameServices();
}

// One Game Booster row. Safe rows are HKCU and need no elevation; the advanced rows and
// the system profile do, and get the usual one-click restart offer.
static void ToggleGameTweak(int index) {
    if (!g_app || index < 0 || index >= (int)g_app->gameTweaks.size()) return;
    const GameTweakView tweak = g_app->gameTweaks[(size_t)index];
    if (!tweak.available) return;
    if (tweak.needsAdmin &&
        !ConfirmAdminRights(L"Changing \"" + tweak.title + L"\"", L"Game Booster"))
        return;

    // Ticked rows stay controllable here: a value that was already on when WinSuite found
    // it is switched off by clearing it to the Windows default, and the value it had is
    // journalled first so Revert can still put it back.
    NetOpResult result = tweak.journaled
                             ? RevertGameTweak(tweak.id)
                             : (tweak.applied ? TurnOffGameTweak(tweak.id)
                                              : ApplyGameTweak(tweak.id));
    g_app->boosterMessage = result.message;
    g_app->boosterMessageIsError = !result.ok;
    LoadBoosterPage();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// One background-service row. The services are only ever stopped and set to Manual, and
// what each one was doing before is saved first, so Revert puts it back exactly.
static void ToggleGameService(int index) {
    if (!g_app || index < 0 || index >= (int)g_app->gameServices.size()) return;
    const GameServiceView row = g_app->gameServices[(size_t)index];
    if (row.installed == 0) return;
    const std::wstring what = row.journaled ? L"Putting \"" + row.title + L"\" back"
                                            : L"Stopping \"" + row.title + L"\"";
    if (!ConfirmAdminRights(what, L"Game Booster")) return;

    NetOpResult result = row.journaled ? RevertGameService(row.id) : TurnOffGameService(row.id);
    g_app->boosterMessage = result.message;
    g_app->boosterMessageIsError = !result.ok;
    LoadBoosterPage();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// One row's switch: revert when this suite saved the old values, turn off a value that was
// already in gaming shape when WinSuite found it, and apply when it is not set at all.
// Turning off saves the value that is there before removing it, so this page can undo a
// setting made elsewhere without losing what made it.
static void ToggleNetTweak(int index) {
    if (!g_app || index < 0 || index >= (int)g_app->netTweaks.size()) return;
    const NetTweakView tweak = g_app->netTweaks[(size_t)index];
    if (!tweak.available) return;
    if (!ConfirmAdminRights(L"Changing network settings", L"Internet & networking")) return;

    NetOpResult result;
    if (tweak.journaled) {
        result = RevertNetTweak(tweak.id);
    } else if (tweak.applied) {
        result = TurnOffNetTweak(tweak.id);
    } else {
        result = ApplyNetTweak(tweak.id);
    }
    g_app->netMessage = result.message;
    g_app->netMessageIsError = !result.ok;
    LoadNetPage();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ResetLatencyCheck() {
    if (!g_app || g_app->netPinging) return;
    g_app->netBefore = LatencyResult();
    g_app->netAfter = LatencyResult();
    g_app->netPingMessage.clear();
    g_app->netPingMessageIsError = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// The pings run on a worker and report back through WM_APP_LATENCY_DONE, so the window
// never sits frozen for the length of four one-second timeouts.
static void StartLatencyCheck() {
    if (!g_app || g_app->netPinging) return;
    const std::wstring host = g_app->netHost;
    if (host.empty()) {
        g_app->netPingMessage = L"Type a host or IP address to ping.";
        g_app->netPingMessageIsError = true;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    g_app->netPinging = true;
    g_app->netPingMessage = L"Pinging " + host + L" ...";
    g_app->netPingMessageIsError = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);

    std::thread([host] {
        LatencyResult* result = new LatencyResult(MeasureLatency(host, 4));
        PostMessageW(g_hwnd, WM_APP_LATENCY_DONE, 0, (LPARAM)result);
    }).detach();
}

// The single place the gaming profile is applied and reported, so a press that starts it
// straight away and one that waits for its baseline end in the same state.
static void ApplyGamingProfileNow() {
    NetOpResult result = ApplyNetGamingProfile();
    g_app->netMessage = result.message;
    g_app->netMessageIsError = !result.ok;
    LoadNetPage();
    // With a baseline in hand the after-reading is taken here, so applying the profile ends
    // with the comparison on the card instead of waiting for a second press. Only a press
    // that actually changed a value measures: an apply that found the profile already in
    // place has nothing to compare. A ping already in flight is left alone - it is being
    // taken after the baseline and lands as the after-reading itself.
    if (result.ok && result.changed > 0 && g_app->netBefore.ok && !g_app->netHost.empty() &&
        !g_app->netPinging) {
        StartLatencyCheck();
        if (g_app->netPinging) {
            g_app->netPingMessage = L"Profile applied - measuring the after-reading to " +
                                    g_app->netHost + L" ...";
            g_app->netPingMessageIsError = false;
        }
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ReportServiceOp(bool ok, const ServiceOp& op, const std::wstring& done) {
    g_app->servicesMessage = ok ? done : op.error;
    g_app->servicesMessageIsError = !ok;
}

static void ApplyServiceStartType(int typeIndex) {
    DWORD start = SERVICE_DEMAND_START;
    bool delayed = false;
    if (typeIndex == 0)
        start = SERVICE_AUTO_START;
    else if (typeIndex == 1) {
        start = SERVICE_AUTO_START;
        delayed = true;
    } else if (typeIndex == 3)
        start = SERVICE_DISABLED;

    // The four chips belong to the list, not to one row: with rows ticked they set the
    // whole selection, the way Start and Stop do, and with nothing ticked they fall back
    // to the highlighted row. A row already in the requested state is left out, so the
    // report counts only the services the press actually changed.
    std::vector<ServiceRow> targets;
    for (const auto& service : CheckedServiceRows())
        if (service.startType != start || service.delayed != delayed) targets.push_back(service);
    if (targets.empty()) {
        const ServiceRow* row = SelectedService();
        if (!row || (row->startType == start && row->delayed == delayed)) return;
        targets.push_back(*row);
    }

    std::wstring label = start == SERVICE_AUTO_START
                             ? (delayed ? L"Automatic (delayed)" : L"Automatic")
                             : (start == SERVICE_DISABLED ? L"Disabled" : L"Manual");
    std::wstring what =
        targets.size() == 1
            ? L"Changing \"" + targets[0].display + L"\""
            : L"Setting " + std::to_wstring(targets.size()) + L" services to " + label;
    if (!ConfirmAdminRights(what, L"Services")) return;

    int done = 0, failed = 0;
    std::wstring firstError;
    for (const auto& target : targets) {
        ServiceOp op;
        if (SetServiceStart(target.name, start, delayed, op)) {
            ++done;
        } else {
            ++failed;
            if (firstError.empty()) firstError = target.display + L": " + op.error;
        }
    }

    if (targets.size() == 1) {
        g_app->servicesMessage = done > 0
                                     ? L"\"" + targets[0].display + L"\" is now " + label + L"."
                                     : L"Could not change \"" + targets[0].display +
                                           L"\": " + firstError;
    } else if (failed == 0) {
        g_app->servicesMessage =
            L"Set " + std::to_wstring(done) + L" services to " + label + L".";
    } else if (done > 0) {
        g_app->servicesMessage = L"Set " + std::to_wstring(done) + L" of " +
                                 std::to_wstring(targets.size()) + L" services to " + label +
                                 L"; " + std::to_wstring(failed) + L" refused - " + firstError;
    } else {
        g_app->servicesMessage = L"None of the " + std::to_wstring(targets.size()) +
                                 L" services could be changed: " + firstError;
    }
    g_app->servicesMessageIsError = failed > 0;
    ReloadServices(targets.size() == 1 ? targets[0].name : L"");
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void StartOrStopService(bool start) {
    // Every ticked row that still needs the change, in list order; with nothing ticked,
    // the highlighted row alone.
    std::vector<ServiceRow> targets;
    for (const auto& service : CheckedServiceRows())
        if (start ? !service.running : service.running) targets.push_back(service);
    if (targets.empty()) {
        const ServiceRow* row = SelectedService();
        if (!row || (start ? row->running : !row->running)) return;
        targets.push_back(*row);
    }

    std::wstring what =
        targets.size() == 1
            ? (start ? L"Starting \"" : L"Stopping \"") + targets[0].display + L"\""
            : (start ? L"Starting " : L"Stopping ") + std::to_wstring(targets.size()) +
                  L" services";
    if (!ConfirmAdminRights(what, L"Services")) return;

    int done = 0, failed = 0;
    std::wstring firstError;
    for (const auto& target : targets) {
        ServiceOp op;
        bool ok = start ? StartNamedService(target.name, op) : StopNamedService(target.name, op);
        if (ok) {
            ++done;
        } else {
            ++failed;
            if (firstError.empty()) firstError = target.display + L": " + op.error;
        }
    }

    if (targets.size() == 1) {
        g_app->servicesMessage = done > 0
                                     ? (start ? L"Started \"" : L"Stopped \"") +
                                           targets[0].display + L"\"."
                                     : (start ? L"Could not start \"" : L"Could not stop \"") +
                                           targets[0].display + L"\": " + firstError;
    } else if (failed == 0) {
        g_app->servicesMessage = (start ? L"Started " : L"Stopped ") + std::to_wstring(done) +
                                 L" services.";
    } else if (done > 0) {
        g_app->servicesMessage = (start ? L"Started " : L"Stopped ") + std::to_wstring(done) +
                                 L" of " + std::to_wstring(targets.size()) +
                                 L" services; " + std::to_wstring(failed) + L" refused - " +
                                 firstError;
    } else {
        g_app->servicesMessage = (start ? L"None of the " : L"None of the ") +
                                 std::to_wstring(targets.size()) + L" services could be " +
                                 (start ? L"started: " : L"stopped: ") + firstError;
    }
    g_app->servicesMessageIsError = failed > 0;
    g_app->serviceChecked.clear();
    ReloadServices(targets[0].name);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ShowServiceForm(bool editing) {
    const ServiceRow* row = SelectedService();
    if (editing && !row) return;
    ServiceRow target = editing ? *row : ServiceRow();

    if (!ConfirmAdminRights(editing ? L"Editing \"" + target.display + L"\""
                                    : L"Creating a service",
                            L"Services"))
        return;

    FormSpec form;
    form.title = editing ? L"Edit service" : L"New service";
    form.hint = editing
                    ? L"The name is fixed once Windows knows the entry; the display name, "
                      L"binary and description can change."
                    : L"Windows creates the entry as Manual and stopped. Point the binary at "
                      L"the service program, not a normal app.";
    if (editing) {
        form.labels = {L"Service name", L"Display name", L"Binary path", L"Description"};
        form.values = {target.name, target.display, target.imagePath, target.description};
        form.readOnly = {true, false, false, false};
    } else {
        form.labels = {L"Service name", L"Display name", L"Binary path", L"Description"};
        form.values = {L"", L"", L"", L""};
        form.readOnly = {false, false, false, false};
    }

    if (!RunForm(form)) return;

    ServiceOp op;
    bool ok = editing
                  ? UpdateExistingService(target.name, form.values[1], form.values[2],
                                          form.values[3], op)
                  : CreateNewService(form.values[0], form.values[1], form.values[2],
                                     form.values[3], op);
    std::wstring name = editing ? target.name : form.values[0];
    ReportServiceOp(ok, op, editing ? L"Saved \"" + name + L"\"."
                                    : L"Created \"" + name + L"\". Set its start type below.");
    ReloadServices(name);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void DeleteSelectedService() {
    const ServiceRow* row = SelectedService();
    if (!row) return;
    const ServiceRow target = *row;
    if (target.windows) return;
    std::wstring message = L"Delete the service \"" + target.display + L"\"?\n\nWindows "
                           L"removes the entry. The program on disk is left alone, and a "
                           L"service that is still in use may refuse to go until it stops.";
    if (MessageBoxW(g_hwnd, message.c_str(), L"Services",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    if (!ConfirmAdminRights(L"Deleting \"" + target.display + L"\"", L"Services")) return;
    ServiceOp op;
    bool ok = DeleteNamedService(target, op);
    ReportServiceOp(ok, op, L"Deleted \"" + target.display + L"\".");
    ReloadServices();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static const ProcRow* SelectedTask() {
    if (g_app->taskSelected < 0 || g_app->taskSelected >= (int)g_app->taskRows.size()) return nullptr;
    return &g_app->taskRows[(size_t)g_app->taskSelected];
}

static void EndSelectedTask(bool force) {
    std::vector<ProcRow> targets;
    for (const auto& process : g_app->taskRows) {
        if (process.pid <= 4) continue;
        if (IsTaskChecked(*g_app, process.pid, process.created)) targets.push_back(process);
    }
    if (targets.empty()) {
        const ProcRow* row = SelectedTask();
        if (!row || row->pid <= 4) return;
        targets.push_back(*row);
    }

    if (force) {
        std::wstring message =
            targets.size() == 1
                ? L"Force \"" + targets[0].name + L"\" to stop?\n\nThe process is terminated "
                  L"at once and anything it had not saved is lost."
                : L"Force " + std::to_wstring(targets.size()) + L" processes to stop?\n\nThey "
                  L"are terminated at once and anything they had not saved is lost.";
        if (MessageBoxW(g_hwnd, message.c_str(), L"Task Manager",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
    }

    int done = 0, failed = 0;
    std::wstring firstError, firstName;
    for (const auto& target : targets) {
        std::wstring error;
        if (EndRunningTask(target.pid, force, error)) {
            ++done;
        } else {
            ++failed;
            if (firstError.empty()) {
                firstName = target.name;
                firstError = error;
            }
        }
    }

    if (targets.size() == 1) {
        g_app->tasksMessage = done == 1
                                  ? (force ? L"Force-ended \"" : L"Ended \"") +
                                        targets[0].name + L"\" (PID " +
                                        std::to_wstring(targets[0].pid) + L")."
                                  : L"Could not end \"" + targets[0].name + L"\": " + firstError;
    } else if (failed == 0) {
        g_app->tasksMessage = (force ? L"Force-ended " : L"Ended ") + std::to_wstring(done) +
                              L" processes.";
    } else if (done > 0) {
        g_app->tasksMessage = (force ? L"Force-ended " : L"Ended ") + std::to_wstring(done) +
                              L" of " + std::to_wstring(targets.size()) + L" processes; " +
                              std::to_wstring(failed) + L" refused - " + firstName + L": " +
                              firstError;
    } else {
        g_app->tasksMessage = L"None of the " + std::to_wstring(targets.size()) +
                              L" processes could be ended: " + firstName + L": " + firstError;
    }
    g_app->tasksMessageIsError = failed > 0;
    // The targets are gone, so the ticks and the highlight go with them: keeping either
    // would leave the card acting on pids that no longer exist until the next load.
    g_app->taskChecked.clear();
    g_app->taskSelected = -1;
    LoadTasksPage(*g_app);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ShowNewTaskForm() {
    FormSpec form;
    form.title = L"New task";
    form.hint = L"Type a program or a command line, the way the Run box takes it. It starts "
                L"in the same session as the suite.";
    form.labels = {L"Program or command"};
    form.values = {L""};
    form.readOnly = {false};
    if (!RunForm(form)) return;
    if (form.values[0].empty()) return;

    std::wstring error;
    if (LaunchNewTask(form.values[0], error)) {
        g_app->tasksMessage = L"Started: " + form.values[0];
        g_app->tasksMessageIsError = false;
    } else {
        g_app->tasksMessage = L"Could not start it: " + error;
        g_app->tasksMessageIsError = true;
    }
    LoadTasksPage(*g_app);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ApplySelectedAffinity() {
    const ProcRow* row = SelectedTask();
    if (!row) return;
    if (!g_app->taskCpuMask) {
        g_app->tasksMessage = L"Tick at least one core before applying.";
        g_app->tasksMessageIsError = true;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    const ProcRow target = *row;
    const ULONG_PTR requested = g_app->taskCpuMask;
    std::wstring error;
    bool ok = SetTaskAffinity(target.pid, requested, error);
    LoadTasksPage(*g_app);

    // Report the mask Windows actually holds, not the one that was asked for: a process
    // in another processor group, or one that has just gone, is not the same as success.
    ULONG_PTR applied = requested;
    bool stillListed = false;
    for (const auto& live : g_app->taskRows) {
        if (live.pid != target.pid) continue;
        applied = live.affinity;
        stillListed = true;
        break;
    }
    if (ok) {
        if (stillListed && applied != requested) {
            g_app->tasksMessage = L"\"" + target.name + L"\" kept " + AffinityText(applied) +
                                  L" - Windows did not move it to " + AffinityText(requested) +
                                  L".";
            g_app->tasksMessageIsError = true;
        } else {
            g_app->tasksMessage = L"\"" + target.name + L"\" now runs on " +
                                  AffinityText(applied) + L".";
            g_app->tasksMessageIsError = false;
            if (g_app->taskRemember && stillListed)
                SaveTaskRule(target.name, applied, g_app->taskPriority);
        }
    } else {
        g_app->tasksMessage = L"Could not change the cores: " + error;
        g_app->tasksMessageIsError = true;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ApplySelectedPriority() {
    const ProcRow* row = SelectedTask();
    if (!row) return;
    const ProcRow target = *row;
    if (g_app->taskPriority == REALTIME_PRIORITY_CLASS) {
        // Above every normal class: a badly written program at this level can starve the
        // mouse and the shell, so it is asked for by name.
        std::wstring question = L"Set \"" + target.name + L"\" to Real time?\n\n"
                                L"Real time runs above every normal priority, including "
                                L"the keyboard and mouse. Only use it for a program that "
                                L"needs it and is known to behave.";
        if (MessageBoxW(g_hwnd, question.c_str(), L"Task Manager",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
    }
    std::wstring error;
    bool ok = SetTaskPriority(target.pid, g_app->taskPriority, error);
    LoadTasksPage(*g_app);

    // Report what Windows actually applied rather than what was asked for: it can turn a
    // request into a different class, and the row is the truth either way.
    DWORD applied = g_app->taskPriority;
    bool stillListed = false;
    for (const auto& live : g_app->taskRows) {
        if (live.pid != target.pid) continue;
        applied = live.priority;
        stillListed = true;
        break;
    }
    if (ok) {
        g_app->tasksMessage = L"\"" + target.name + L"\" is now " + PriorityLabel(applied) +
                              L" priority.";
        g_app->tasksMessageIsError = false;
        if (g_app->taskRemember && stillListed)
            SaveTaskRule(target.name,
                         target.affinity ? target.affinity : g_app->taskCpuMask, applied);
    } else {
        g_app->tasksMessage = L"Could not change the priority: " + error;
        g_app->tasksMessageIsError = true;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ApplyFilterChange(AppState& st);

// The text field that has the caret, if any: the two list filters or the latency host.
static std::wstring* FocusedText(AppState& st) {
    if (st.inputFocus == HitSvcFilter) return &st.serviceFilter;
    if (st.inputFocus == HitTaskFilter) return &st.taskFilter;
    if (st.inputFocus == HitNetHost) return &st.netHost;
    return nullptr;
}

// A reading belongs to the host it was taken against, so anything that changes the host
// drops both readings - typing, backspace and the field's own clear button alike.
static void LatencyHostChanged(AppState& st) {
    st.netBefore = LatencyResult();
    st.netAfter = LatencyResult();
    st.netPingMessage.clear();
    st.netPingMessageIsError = false;
}

// The list filters re-check the selection instead.
static void TextEdited(AppState& st) {
    if (st.inputFocus == HitNetHost) {
        LatencyHostChanged(st);
        return;
    }
    ApplyFilterChange(st);
}

// Narrowing a list can hide the row the buttons act on, so a selection the filter has
// hidden is dropped, and the list starts at its top again.
static void ApplyFilterChange(AppState& st) {
    if (st.page == Page::Services) {
        std::vector<int> visible = FilterServiceRows(st.serviceRows, st.serviceFilter);
        if (st.serviceSelected >= 0 &&
            std::find(visible.begin(), visible.end(), st.serviceSelected) == visible.end())
            st.serviceSelected = -1;
        // A tick on a row the filter hides is dropped too: a bulk Start or Stop must never
        // change a service the user cannot see.
        std::vector<std::wstring> kept;
        for (int i : visible)
            if (IsServiceChecked(st, st.serviceRows[(size_t)i].name))
                kept.push_back(st.serviceRows[(size_t)i].name);
        st.serviceChecked.swap(kept);
        st.scrollServices = 0;
    } else if (st.page == Page::Tasks) {
        std::vector<int> visible = FilterTaskRows(st.taskRows, st.taskFilter);
        if (st.taskSelected >= 0 &&
            std::find(visible.begin(), visible.end(), st.taskSelected) == visible.end())
            st.taskSelected = -1;
        std::vector<TaskRef> kept;
        for (int i : visible) {
            const ProcRow& row = st.taskRows[(size_t)i];
            if (row.pid > 4 && IsTaskChecked(st, row.pid, row.created))
                kept.push_back({row.pid, row.created});
        }
        st.taskChecked.swap(kept);
        st.scrollTasks = 0;
    }
}

// The two Memory page cleans, written once so the page's buttons and the tray menu run the
// same thing: Settings decides whether a prompt appears, and a deep clean still asks for
// the administrator restart when the suite is running without rights. A tray press has no
// visible window to own its dialog, so that prompt is raised unowned and still comes
// forward, which is what makes the tray items usable without opening the window.
static HWND PromptOwner() {
    return (g_hwnd && IsWindowVisible(g_hwnd)) ? g_hwnd : nullptr;
}

static void RunRegularMemoryClean() {
    if (!g_app || g_app->memBusy) return;
    if (!g_app->memPrefs.confirmRegular) {
        StartMemoryClean(false);
        return;
    }
    const wchar_t* message =
        L"Trim the working set of the programs in your session?\n\n"
        L"Nothing is closed and no data is lost, but open apps may pause for a moment while "
        L"Windows reads their memory back in from disk.";
    if (MessageBoxW(PromptOwner(), message, L"Regular memory clean",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
        StartMemoryClean(false);
}

static void RunDeepMemoryClean() {
    if (!g_app || g_app->memBusy) return;
    if (!g_app->elevated) {
        const wchar_t* message =
            L"Deep clean trims every reachable process and purges the standby cache, which "
            L"Windows only allows with administrator rights.\n\n"
            L"Restart Zanphar's WinSuite as administrator now?";
        if (MessageBoxW(PromptOwner(), message, L"Deep memory clean",
                        MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            DWORD launchError = 0;
            if (RelaunchElevated({}, &launchError)) {
                SetStandardModePreference(false);
                DestroyWindow(g_hwnd);  // the elevated copy takes over
            } else {
                MessageBoxW(PromptOwner(),
                            L"Administrator rights were not granted, so no deep clean was "
                            L"run.",
                            L"Deep memory clean", MB_OK | MB_ICONINFORMATION);
            }
        }
        return;
    }
    if (!g_app->memPrefs.confirmDeep) {
        StartMemoryClean(true);
        return;
    }
    const wchar_t* warning =
        L"Deep clean does two things:\n\n"
        L"  1. trims the working set of every process it can reach,\n"
        L"  2. purges the Windows standby cache.\n\n"
        L"Standby pages are clean file data Windows keeps for speed. Purging them frees "
        L"memory, but the next launches have to read from disk again. No program is "
        L"closed.\n\nRun the deep clean?";
    if (MessageBoxW(PromptOwner(), warning, L"Deep memory clean",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
        StartMemoryClean(true);
}

static void HandleHit(int id, bool enabled) {
    if (!g_app || !enabled) return;

    // Typing belongs to a text field only while it is the last thing clicked; any other
    // press takes the caret away again.
    const bool searchHit = id == HitSvcFilter || id == HitSvcFilterClear ||
                           id == HitTaskFilter || id == HitTaskFilterClear ||
                           id == HitNetHost || id == HitNetHostClear;
    if (!searchHit && g_app->inputFocus != HitNone) {
        g_app->inputFocus = HitNone;
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
    if (id == HitSvcFilter || id == HitTaskFilter || id == HitNetHost) {
        g_app->inputFocus = id;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id == HitNetHostClear) {
        // The clear button works without the caret being in the field, so the readings
        // are dropped here rather than through TextEdited's focus test.
        g_app->netHost.clear();
        LatencyHostChanged(*g_app);
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id == HitNetPing) {
        StartLatencyCheck();
        return;
    }
    if (id == HitNetReset) {
        ResetLatencyCheck();
        return;
    }
    if (id == HitSvcFilterClear) {
        g_app->serviceFilter.clear();
        ApplyFilterChange(*g_app);
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id == HitTaskFilterClear) {
        g_app->taskFilter.clear();
        ApplyFilterChange(*g_app);
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    // The Storage rows are the highest range, then the game-service rows, then the game
    // tweaks: every test here is a lower bound, so the highest range has to be matched first.
    if (id >= HitStorageRowBase && id < HitStorageRowBase + 26) {
        StartStorageOptimize(id - HitStorageRowBase);
        return;
    }
    if (id >= HitGameServiceBase) {
        ToggleGameService(id - HitGameServiceBase);
        return;
    }
    if (id >= HitGameTweakBase) {
        ToggleGameTweak(id - HitGameTweakBase);
        return;
    }
    if (id >= HitNetRowBase) {
        ToggleNetTweak(id - HitNetRowBase);
        return;
    }
    if (id >= HitTaskRowBase) {
        int index = id - HitTaskRowBase;
        SelectTaskRow(index);
        if (index >= 0 && index < (int)g_app->taskRows.size()) {
            const ProcRow& row = g_app->taskRows[(size_t)index];
            if (row.pid > 4) {
                auto& checked = g_app->taskChecked;
                auto at = std::find_if(checked.begin(), checked.end(), [&](const TaskRef& ref) {
                    return ref.pid == row.pid && ref.created == row.created;
                });
                if (at == checked.end())
                    checked.push_back({row.pid, row.created});
                else
                    checked.erase(at);
            }
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id >= HitSvcRowBase) {
        int index = id - HitSvcRowBase;
        if (index >= 0 && index < (int)g_app->serviceRows.size()) {
            g_app->serviceSelected = index;
            // A press ticks the row so a second one joins the bulk set; pressing a ticked
            // row again takes its tick off.
            const std::wstring& name = g_app->serviceRows[(size_t)index].name;
            auto& checked = g_app->serviceChecked;
            auto at = std::find(checked.begin(), checked.end(), name);
            if (at == checked.end())
                checked.push_back(name);
            else
                checked.erase(at);
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id >= HitTaskCpuBase && id < HitTaskCpuBase + 64) {
        int cpu = id - HitTaskCpuBase;
        g_app->taskCpuMask ^= ((ULONG_PTR)1 << cpu);
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id >= HitTaskPriorityBase && id < HitTaskPriorityBase + 6) {
        static const DWORD values[] = {IDLE_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS,
                                       NORMAL_PRIORITY_CLASS, ABOVE_NORMAL_PRIORITY_CLASS,
                                       HIGH_PRIORITY_CLASS, REALTIME_PRIORITY_CLASS};
        g_app->taskPriority = values[id - HitTaskPriorityBase];
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id >= HitSvcTypeBase && id < HitSvcTypeBase + 4) {
        ApplyServiceStartType(id - HitSvcTypeBase);
        return;
    }

    // The older row ranges sit above the rest, so they come next.
    if (id >= HitDnsRowBase) return;  // adapter rows are read-only
    if (id >= HitBrowserRowBase) {
        ToggleBrowserEntry(*g_app, id - HitBrowserRowBase);
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id >= HitDnsProviderBase && id < HitDnsProviderBase + 5) {
        g_app->dnsProvider = id - HitDnsProviderBase;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id >= HitMemAutoBase && id < HitMemAutoBase + 5) {
        static const int kValues[] = {0, 65, 75, 85, 95};
        int index = id - HitMemAutoBase;
        g_app->memPrefs.autoClean = index > 0;
        if (index > 0) g_app->memPrefs.threshold = kValues[index];
        SaveMemoryPrefs(g_app->memPrefs);
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    // Checked before the other row ranges: its ids sit above all of them.
    if (id >= HitDeactivateRowBase) {
        int index = id - HitDeactivateRowBase;
        if (index < 0 || index >= (int)g_app->services.size()) return;
        const ServiceEntry service = g_app->services[(size_t)index];
        std::wstring what = service.deactivated
                                ? L"Turning \"" + service.display + L"\" back on"
                                : L"Turning \"" + service.display + L"\" off";
        if (!ConfirmServiceRights(what)) return;
        ToggleService(*g_app, index);
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    if (id >= HitCleanupRowBase && id < HitStartupRowBase) {
        int index = id - HitCleanupRowBase;
        if (index >= 0 && index < (int)g_app->groups.size()) {
            g_app->groups[index].selected = !g_app->groups[index].selected;
            SaveCleanSelection(SettingsPath(), g_app->groups);
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id >= HitLargeRowBase) {
        int index = id - HitLargeRowBase;
        if (index >= 0 && index < (int)g_app->largeFiles.size())
            g_app->largeFiles[index].selected = !g_app->largeFiles[index].selected;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id >= HitStartupRowBase) {
        ToggleStartupItem(*g_app, id - HitStartupRowBase);
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id >= HitLargeChipBase && id < HitLargeChipBase + 4) {
        static const uint64_t kThresholds[] = {100ull << 20, 500ull << 20, 1ull << 30,
                                               5ull << 30};
        g_app->largeThreshold = kThresholds[id - HitLargeChipBase];
        StartLargeScan();
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    switch (id) {
        case NavDashboard:
            NavigateTo(Page::Dashboard);
            break;
        case NavCleanup:
            NavigateTo(Page::Cleanup);
            break;
        case NavStartup:
            NavigateTo(Page::Startup);
            break;
        case NavDeactivate:
            NavigateTo(Page::Deactivate);
            break;
        case HitDeactivateRefresh:
            LoadDeactivatePage(*g_app);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitDeactivateRestoreAll:
            if (ConfirmServiceRights(L"Reactivating these services")) {
                ReactivateAllServices(*g_app);
                InvalidateRect(g_hwnd, nullptr, FALSE);
            }
            break;
        case NavMemory:
            NavigateTo(Page::Memory);
            break;
        case NavBrowsers:
            NavigateTo(Page::Browsers);
            break;
        case NavDns:
            NavigateTo(Page::Dns);
            break;
        case NavNetwork:
            NavigateTo(Page::Network);
            break;
        case HitNetApply: {
            if (g_app->netApplyPending) break;  // already waiting for its baseline
            if (!ConfirmAdminRights(L"Applying the gaming profile", L"Internet & networking"))
                break;
            // A reading only counts as a baseline when it is taken before the values
            // change, so an apply with none yet measures a baseline first and then goes on
            // by itself - still one press, and the after-reading is taken the moment the
            // values land. A measurement already in flight is the one that lands as the
            // baseline. A baseline already in hand is never replaced: it may have been
            // taken before the tweaks on purpose.
            if (!g_app->netBefore.ok && !g_app->netHost.empty()) {
                g_app->netApplyPending = true;
                g_app->netMessage =
                    L"Measuring the baseline to " + g_app->netHost +
                    L" first - the profile goes on as soon as that reading lands.";
                g_app->netMessageIsError = false;
                if (!g_app->netPinging) StartLatencyCheck();
                InvalidateRect(g_hwnd, nullptr, FALSE);
                break;
            }
            ApplyGamingProfileNow();
            break;
        }
        case HitNetRevert: {
            if (g_app->netApplyPending) break;
            if (!ConfirmAdminRights(L"Reverting the network changes", L"Internet & networking"))
                break;
            NetOpResult result = RevertNetGamingProfile();
            g_app->netMessage = result.message;
            g_app->netMessageIsError = !result.ok;
            LoadNetPage();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitNetDefaults: {
            if (g_app->netApplyPending) break;
            if (!ConfirmAdminRights(L"Restoring the Windows networking defaults",
                                    L"Internet & networking"))
                break;
            // All four tweaks in one press, each value journalled before it is cleared, so
            // Revert all can still put the machine back where it was.
            NetOpResult result = RestoreNetDefaults();
            g_app->netMessage = result.message;
            g_app->netMessageIsError = !result.ok;
            LoadNetPage();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitNetRefresh:
            LoadNetPage();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case NavBoost:
            NavigateTo(Page::Boost);
            break;
        case HitBrowserScan:
            StartBrowserScan();
            break;
        case HitBrowserClean: {
            if (g_app->browserScanning || g_app->browserCleaning) break;
            int count = 0;
            uint64_t bytes = 0;
            for (const auto& browser : g_app->browsers)
                if (browser.selected && browser.installed && !browser.running) {
                    ++count;
                    bytes += browser.bytes;
                }
            if (count == 0) break;
            std::wstring message = L"Delete history, cookies, autofill, caches and session "
                                   L"data for " +
                                   std::to_wstring(count) + L" browser(s)?\n\n" +
                                   FormatBytes(bytes) +
                                   L" will be removed. Sites will sign you out and autofill "
                                   L"entries are lost.";
            if (MessageBoxW(g_hwnd, message.c_str(), L"Browsers",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
                StartBrowserClean();
            break;
        }
        case HitBrowserClose: {
            std::wstring report;
            CloseRunningBrowsers(g_app->browsers, report);
            LoadBrowsersPage(*g_app);
            g_app->browserMessage = report;
            g_app->browserMessageIsError = false;
            StartBrowserScan();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitDnsApply: {
            if (!ConfirmAdminRights(L"Changing DNS", L"DNS")) break;
            const auto& providers = DnsProviders();
            int index = std::clamp(g_app->dnsProvider, 0, (int)providers.size() - 1);
            const DnsProvider& provider = providers[(size_t)index];
            bool system = !provider.primary || !*provider.primary;
            std::wstring error;
            if (ApplyDns(g_app->dnsAdapters, provider, error)) {
                g_app->dnsApplied = !system;
                g_app->dnsPrimary = provider.primary ? provider.primary : L"";
                g_app->dnsSecondary = provider.secondary ? provider.secondary : L"";
                g_app->dnsWatchdog = !system;
                g_dnsIncidentCount.store(0);
                g_app->dnsIncidents = 0;
                if (system) {
                    g_dnsWatchdogRunning.store(false);
                    g_app->dnsMessage =
                        L"Every adapter is back on the DNS its network hands out.";
                } else {
                    StartDnsWatchdog();
                    g_app->dnsMessage = std::wstring(L"Applied ") + provider.name + L" to " +
                                        std::to_wstring(g_app->dnsAdapters.size()) +
                                        L" adapter(s). Watchdog on.";
                }
                g_app->dnsMessageIsError = false;
                SaveDnsSettings();
                LoadDnsPage(*g_app);
            } else {
                g_app->dnsMessage = error.empty() ? L"Nothing was changed." : error;
                g_app->dnsMessageIsError = true;
            }
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitDnsRestore: {
            if (!ConfirmAdminRights(L"Restoring system DNS", L"DNS")) break;
            std::wstring error;
            if (RestoreSystemDns(g_app->dnsAdapters, error)) {
                g_app->dnsApplied = false;
                g_app->dnsWatchdog = false;
                g_app->dnsProvider = 0;
                g_app->dnsPrimary.clear();
                g_app->dnsSecondary.clear();
                g_dnsWatchdogRunning.store(false);
                g_app->dnsIncidents = 0;
                g_app->dnsMessage = L"System DNS restored on every adapter.";
                g_app->dnsMessageIsError = false;
                SaveDnsSettings();
                LoadDnsPage(*g_app);
            } else {
                g_app->dnsMessage = error.empty() ? L"Nothing was changed." : error;
                g_app->dnsMessageIsError = true;
            }
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitDnsWatchdog: {
            if (!g_app->dnsApplied) {
                g_app->dnsMessage =
                    L"Apply a provider first - the watchdog needs servers to defend.";
                g_app->dnsMessageIsError = true;
            } else {
                g_app->dnsWatchdog = !g_app->dnsWatchdog;
                if (g_app->dnsWatchdog)
                    StartDnsWatchdog();
                else
                    g_dnsWatchdogRunning.store(false);
                g_app->dnsMessage = g_app->dnsWatchdog
                                        ? L"Watchdog on: drift is reverted within two seconds."
                                        : L"Watchdog off.";
                g_app->dnsMessageIsError = false;
                SaveDnsSettings();
            }
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitDnsRefresh:
            LoadDnsPage(*g_app);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitBoostToggle: {
            if (g_app->boostBusy || g_workerRunning.load()) {
                g_app->boosterMessage = L"Another job is still running - try again in a moment.";
                g_app->boosterMessageIsError = true;
                InvalidateRect(g_hwnd, nullptr, FALSE);
                break;
            }
            if (!ConfirmAdminRights(L"Applying the Game Booster", L"Game Booster")) break;
            // The safe tweaks are immediate; the profile runs on its worker and the two
            // halves meet again in WM_APP_BOOST_DONE.
            NetOpResult tweaks = ApplyGameTweaks(false);
            if (g_app->boost.active) {
                // The profile is already on and only its tweaks were switched off, so
                // there is nothing for the worker to do. Starting it would only answer
                // "already on", which reads as if the press had failed.
                g_app->boosterMessage = tweaks.message;
                g_app->boosterMessageIsError = !tweaks.ok;
                LoadBoosterPage();
                InvalidateRect(g_hwnd, nullptr, FALSE);
                break;
            }
            g_boostTweakNote = tweaks.message;
            g_boostTweakNoteIsError = !tweaks.ok;
            StartBoost(false);
            break;
        }
        case HitBoostRestore: {
            if (g_app->boostBusy || g_workerRunning.load()) {
                g_app->boosterMessage = L"Another job is still running - try again in a moment.";
                g_app->boosterMessageIsError = true;
                InvalidateRect(g_hwnd, nullptr, FALSE);
                break;
            }
            if (!ConfirmAdminRights(L"Reverting the Game Booster", L"Game Booster")) break;
            NetOpResult tweaks = RevertGameTweaks();
            if (!g_app->boost.active) {
                // No system profile to put back, so the answer is just the tweaks'.
                g_app->boosterMessage = tweaks.message;
                g_app->boosterMessageIsError = !tweaks.ok;
                LoadBoosterPage();
                InvalidateRect(g_hwnd, nullptr, FALSE);
                break;
            }
            g_boostTweakNote = tweaks.message;
            g_boostTweakNoteIsError = !tweaks.ok;
            StartBoost(true);
            break;
        }
        case HitBoostProfile:
            if (g_app->boostBusy || g_workerRunning.load()) {
                g_app->boosterMessage = L"Another job is still running - try again in a moment.";
                g_app->boosterMessageIsError = true;
                InvalidateRect(g_hwnd, nullptr, FALSE);
                break;
            }
            if (ConfirmAdminRights(g_app->boost.active ? L"Restoring the system profile"
                                                       : L"Applying the system profile",
                                   L"Game Booster")) {
                g_boostTweakNote.clear();
                g_boostTweakNoteIsError = false;
                StartBoost(g_app->boost.active);
            }
            break;
        case HitBoostRefresh:
            LoadBoosterPage();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitMemIdle:
            g_app->memPrefs.idleClean = !g_app->memPrefs.idleClean;
            SaveMemoryPrefs(g_app->memPrefs);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitMemClipboard:
            g_app->memPrefs.clearClipboard = !g_app->memPrefs.clearClipboard;
            SaveMemoryPrefs(g_app->memPrefs);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case NavLargeFiles:
            NavigateTo(Page::LargeFiles);
            break;
        case NavSystem:
            NavigateTo(Page::System);
            break;
        case NavStorage:
            NavigateTo(Page::Storage);
            break;
        case HitStorageRefresh:
            if (!g_app->storageBusy) {
                LoadSystemPage(*g_app);
                g_app->storageMessage.clear();
                g_app->storageMessageIsError = false;
                InvalidateRect(g_hwnd, nullptr, FALSE);
            }
            break;
        case HitStorageCancel:
            CancelStorageOptimize();
            break;
        case NavSettings:
            NavigateTo(Page::Settings);
            break;
        case NavServices:
            NavigateTo(Page::Services);
            break;
        case NavTasks:
            NavigateTo(Page::Tasks);
            break;
        case HitSvcRefresh:
            ReloadServices();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitSvcStart:
            StartOrStopService(true);
            break;
        case HitSvcStop:
            StartOrStopService(false);
            break;
        case HitSvcNew:
            ShowServiceForm(false);
            break;
        case HitSvcEdit:
            ShowServiceForm(true);
            break;
        case HitSvcDelete:
            DeleteSelectedService();
            break;
        case HitSvcSelectAll: {
            // Only the rows the filter is showing: Select all never reaches a row that is
            // out of sight.
            const std::vector<int> visible = FilterServiceRows(g_app->serviceRows, g_app->serviceFilter);
            g_app->serviceChecked.clear();
            for (int i : visible) g_app->serviceChecked.push_back(g_app->serviceRows[(size_t)i].name);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitSvcClearChecked:
            g_app->serviceChecked.clear();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitTaskSelectAll: {
            const std::vector<int> visible = FilterTaskRows(g_app->taskRows, g_app->taskFilter);
            g_app->taskChecked.clear();
            for (int i : visible) {
                const ProcRow& row = g_app->taskRows[(size_t)i];
                if (row.pid > 4) g_app->taskChecked.push_back({row.pid, row.created});
            }
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitTaskClearChecked:
            g_app->taskChecked.clear();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitTaskRefresh:
            LoadTasksPage(*g_app);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitTaskEnd:
            EndSelectedTask(false);
            break;
        case HitTaskForce:
            EndSelectedTask(true);
            break;
        case HitTaskNew:
            ShowNewTaskForm();
            break;
        case HitTaskApplyAffinity:
            ApplySelectedAffinity();
            break;
        case HitTaskApplyPriority:
            ApplySelectedPriority();
            break;
        case HitTaskRemember: {
            g_app->taskRemember = !g_app->taskRemember;
            const ProcRow* row = SelectedTask();
            if (g_app->taskRemember && row) {
                // Ticking it is the save: the values shown for this program become the
                // rule, so a tick followed by a page change is never silently dropped.
                std::wstring name = row->name;
                ULONG_PTR mask = g_app->taskCpuMask ? g_app->taskCpuMask : row->affinity;
                DWORD priority = g_app->taskPriority ? g_app->taskPriority : row->priority;
                if (SaveTaskRule(name, mask, priority)) {
                    g_app->tasksMessage = L"Saved a rule for " + name + L".";
                    g_app->tasksMessageIsError = false;
                }
            } else if (!g_app->taskRemember && row) {
                ForgetTaskRule(row->name);
                g_app->tasksMessage = L"Removed the saved rule for " + row->name + L".";
                g_app->tasksMessageIsError = false;
            }
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitTaskForget: {
            const ProcRow* row = SelectedTask();
            if (!row) break;
            std::wstring name = row->name;
            if (ForgetTaskRule(name)) {
                g_app->taskRemember = false;
                g_app->tasksMessage = L"Removed the saved rule for " + name + L".";
                g_app->tasksMessageIsError = false;
            }
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitSettingsCloseTray:
        case HitSettingsMinTray:
        case HitSettingsTrayStartup:
        case HitSettingsToasts: {
            bool& option = id == HitSettingsCloseTray     ? g_app->closeToTray
                           : id == HitSettingsMinTray     ? g_app->minimizeToTray
                           : id == HitSettingsTrayStartup ? g_app->startInTray
                                                          : g_app->toastNotifications;
            option = !option;
            SaveWindowPrefs(SettingsPath(), g_app->closeToTray, g_app->minimizeToTray,
                            g_app->startInTray, g_app->toastNotifications);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitSettingsMemRegular:
            g_app->memPrefs.confirmRegular = !g_app->memPrefs.confirmRegular;
            SaveMemoryPrefs(g_app->memPrefs);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitSettingsMemDeep:
            g_app->memPrefs.confirmDeep = !g_app->memPrefs.confirmDeep;
            SaveMemoryPrefs(g_app->memPrefs);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitSettingsLicense:
            g_app->showLicense = true;
            g_app->licenseScroll = 0;
            g_app->inputFocus = HitNone;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitLicenseClose:
            g_app->showLicense = false;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitLargeScan:
            StartLargeScan();
            break;
        case HitLargeRecycle:
            StartLargeDelete();
            break;
        case HitRestartAdmin:
            if (!g_app->elevated) {
                DWORD launchError = 0;
                if (RelaunchElevated({}, &launchError)) {
                    // They asked for admin, so stop suppressing the launch prompt.
                    SetStandardModePreference(false);
                    DestroyWindow(g_hwnd);  // the elevated copy takes over
                } else {
                    std::wstring message =
                        L"Administrator rights were not granted, so WinSuite keeps "
                        L"running with the rights it already has.";
                    if (launchError == ERROR_CANCELLED) {
                        SetStandardModePreference(true);
                        g_app->promptSuppressed = true;
                        InvalidateRect(g_hwnd, nullptr, FALSE);
                        message += L"\n\nWinSuite will stop asking for administrator "
                                   L"rights when it starts. Use this button whenever you "
                                   L"need them.";
                    }
                    MessageBoxW(g_hwnd, message.c_str(), kAppName,
                                MB_OK | MB_ICONINFORMATION);
                }
            }
            break;
        case HitQuickScan:
            NavigateTo(Page::Cleanup);
            g_app->showCleanResult = false;
            StartScan();
            break;
        case HitQuickStartup:
            NavigateTo(Page::Startup);
            break;
        case HitScan:
            g_app->showCleanResult = false;  // an old "Freed ..." banner over a new measurement
            StartScan();
            break;
        case HitScanCancel:
            // The worker stops at the next file boundary; until it acknowledges, the
            // button says "Stopping..." so a second press does nothing.
            if (g_app->scanning && !g_app->scanStopping) {
                g_app->scanStopping = true;
                g_scanCancel.store(true);
                InvalidateRect(g_hwnd, nullptr, FALSE);
            }
            break;
        case HitClean:
            StartClean();
            break;
        case HitCleanupSelectAll:
        case HitCleanupClearAll: {
            ApplyBulkCleanSelection(g_app->groups, id == HitCleanupSelectAll);
            // A bulk press is a deliberate choice, so it is remembered like a row click.
            SaveCleanSelection(SettingsPath(), g_app->groups);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitCleanupSortSize:
        case HitCleanupSortDefault: {
            // Reordering is a view choice: it touches no figures and no ticks, and it
            // takes effect at once rather than at the next pass.
            bool bySize = id == HitCleanupSortSize;
            if (g_app->cleanSortBySize == bySize) break;
            g_app->cleanSortBySize = bySize;
            RefreshCleanupOrder(*g_app);
            // Remembered like a tick, so the page reopens in the order last used.
            SaveCleanupSortBySize(SettingsPath(), bySize);
            // The rows move under the offset, so the new order is read from its top.
            g_app->scrollCleanup = 0;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitCleanupReset: {
            // Back to the built-in selection. The rows keep their figures - only the ticks
            // change - so nothing has to be measured again for this.
            ResetCleanSelection(SettingsPath());
            std::vector<CleanGroup> defaults = BuildCleanGroups();
            for (auto& g : g_app->groups) {
                for (const auto& d : defaults) {
                    if (d.id != g.id) continue;
                    g.selected = d.selected;
                    break;
                }
            }
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
        case HitMemRefresh:
            if (!g_app->memBusy) LoadMemoryPage(*g_app);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HitMemClean:
            RunRegularMemoryClean();
            break;
        case HitMemDeep:
            RunDeepMemoryClean();
            break;
        case HitRefresh:
        case HitStartupRefresh:
            RefreshPage();
            break;
        case HitCopyReport:
            g_app->systemMessage = CopyText(g_hwnd, BuildReport(*g_app))
                                       ? L"Report copied to the clipboard."
                                       : L"Could not open the clipboard.";
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------------- window
static void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP back = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, back);
    PaintApp(mem, rc.right, rc.bottom, *g_app, g_hits);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(back);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// Re-reads free space for the drives already discovered, so the disk cards follow
// a cleanup instead of showing what the volume looked like when the app started.
static void RefreshDriveSpace(SysInfo& sys) {
    for (auto& d : sys.drives) {
        ULARGE_INTEGER freeAvail = {}, total = {}, totalFree = {};
        if (!GetDiskFreeSpaceExW(d.root.c_str(), &freeAvail, &total, &totalFree)) continue;
        d.freeBytes = freeAvail.QuadPart;
        if (total.QuadPart > 0) d.total = total.QuadPart;
    }
}

// Free space for every mounted volume on the System page, without re-querying the
// device descriptors (those only change when hardware is plugged in).
static void RefreshStorageSpace(AppState& st) {
    for (auto& d : st.storage) {
        if (!d.total) continue;
        ULARGE_INTEGER freeAvail = {}, total = {};
        if (!GetDiskFreeSpaceExW(d.root.c_str(), &freeAvail, &total, nullptr)) continue;
        d.freeBytes = freeAvail.QuadPart;
        if (total.QuadPart > 0) d.total = total.QuadPart;
    }
}

static void UpdateDynamicValues() {
    if (!g_app) return;
    g_app->cpu.Update();
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        g_app->sys.ramTotal = mem.ullTotalPhys;
        g_app->sys.ramAvail = mem.ullAvailPhys;
    }
    RefreshDriveSpace(g_app->sys);
    RefreshStorageSpace(*g_app);
    g_app->sys.uptimeSeconds = GetTickCount64() / 1000ULL;
}

// ------------------------------------------------------- notification area
// Defined with the window class further down; the tray icon needs it first.
static HICON LoadAppIcon(HINSTANCE inst, int cx, int cy);

static HICON g_trayIcon = nullptr;
static bool g_trayVisible = false;
static bool g_quitting = false;  // set only when the user really wants to exit
static UINT g_taskbarCreatedMessage = 0;

static NOTIFYICONDATAW TrayData() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.hIcon = g_trayIcon;
    wcsncpy_s(nid.szTip, L"Zanphar's WinSuite", _TRUNCATE);
    return nid;
}

static bool AddTrayIcon() {
    if (g_trayVisible) return true;
    if (!g_trayIcon)
        g_trayIcon = LoadAppIcon(GetModuleHandleW(nullptr), GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON));
    NOTIFYICONDATAW nid = TrayData();
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;
    g_trayVisible = true;
    return true;
}

static void RemoveTrayIcon() {
    if (!g_trayVisible) return;
    NOTIFYICONDATAW nid = TrayData();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayVisible = false;
}

// Hides the window in the notification area. If the icon cannot be added - no shell, or
// the area refuses it - the window stays where it is, because a hidden window with
// nothing to bring it back would look like the app had crashed.
static bool HideToTray() {
    if (!g_hwnd || !AddTrayIcon()) return false;
    ShowWindow(g_hwnd, SW_HIDE);
    return true;
}

static void RestoreFromTray() {
    if (!g_hwnd) return;
    RemoveTrayIcon();
    ShowWindow(g_hwnd, SW_SHOW);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, L"Open Zanphar's WinSuite");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // The two cleans the Memory page offers, so a long clean never needs the window opened.
    // They ask the same questions the page does unless Settings turned the prompt off.
    AppendMenuW(menu, MF_STRING, 3, L"Regular Memory Clean");
    AppendMenuW(menu, MF_STRING, 4, L"Deep Memory Clean");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"Exit");
    POINT pt = {};
    GetCursorPos(&pt);
    // The window has to be in front, or the menu stays up when the user clicks away.
    SetForegroundWindow(g_hwnd);
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x,
                                  pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    if (cmd == 1) {
        RestoreFromTray();
    } else if (cmd == 3) {
        RunRegularMemoryClean();
    } else if (cmd == 4) {
        RunDeepMemoryClean();
    } else if (cmd == 2) {
        // A real exit: the tray icon goes first so nothing is left behind, then the
        // close handler is told not to hide the window again.
        RemoveTrayIcon();
        g_quitting = true;
        DestroyWindow(g_hwnd);
    }
}

// A tray press runs while the window is hidden, so the outcome comes back as a balloon:
// without one the clean would finish silently and look as though nothing had happened.
// Settings can switch this off for a silent clean, in which case nothing is shown at all.
static void TrayBalloon(const wchar_t* title, const std::wstring& text) {
    if (!g_trayVisible) return;
    if (g_app && !g_app->toastNotifications) return;
    NOTIFYICONDATAW nid = TrayData();
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout = 6000;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Explorer restarted and dropped every notification icon: put ours back while the
    // window is hidden, otherwise the app would be unreachable.
    if (g_taskbarCreatedMessage != 0 && msg == g_taskbarCreatedMessage) {
        if (g_app && !IsWindowVisible(hwnd)) {
            g_trayVisible = false;
            AddTrayIcon();
        }
        return 0;
    }

    // A newer launch is replacing this copy. Closing through the normal exit path removes
    // the tray icon and stops the workers, which terminating the process would not.
    if (g_quitMessage != 0 && msg == g_quitMessage) {
        g_quitting = true;
        DestroyWindow(hwnd);
        return 0;
    }

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            PaintWindow(hwnd);
            return 0;

        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_CLOSE:
            // Settings decides whether the X hides the window or exits. If the icon could
            // not be added, HideToTray fails and the close goes ahead as before.
            if (!g_quitting && g_app && g_app->closeToTray && HideToTray()) return 0;
            break;

        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_MINIMIZE && g_app && g_app->minimizeToTray &&
                HideToTray())
                return 0;
            break;

        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                    RestoreFromTray();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowTrayMenu();
                    break;
                default:
                    break;
            }
            return 0;

        case WM_KEYDOWN:
            // The read-only agreement is a view of its own; Escape leaves it.
            if (wp == VK_ESCAPE && g_app && g_app->showLicense) {
                g_app->showLicense = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // Escape leaves a text field: the first press clears the text, the next one
            // gives the keyboard back to the window.
            if (wp == VK_ESCAPE && g_app && g_app->inputFocus != HitNone) {
                if (std::wstring* text = FocusedText(*g_app)) {
                    if (!text->empty()) {
                        text->clear();
                        TextEdited(*g_app);
                    } else {
                        g_app->inputFocus = HitNone;
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            break;

        case WM_CHAR: {
            if (!g_app || g_app->inputFocus == HitNone) break;
            std::wstring* text = FocusedText(*g_app);
            if (!text) break;
            wchar_t ch = (wchar_t)wp;
            // Enter in the host field starts the measurement, so the check is one click
            // or one keystroke away from the tweaks it is meant to measure.
            if (ch == L'\r' && g_app->inputFocus == HitNetHost) {
                StartLatencyCheck();
                return 0;
            }
            bool changed = false;
            if (ch == L'\b') {
                if (!text->empty()) {
                    text->pop_back();
                    changed = true;
                }
            } else if (ch >= 32 && ch != 127) {
                text->push_back(ch);
                changed = true;
            }
            if (changed) {
                TextEdited(*g_app);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (g_app->storageDragging) {
                g_app->storageDragging = false;
                ReleaseCapture();
                // Rebuild the row hit rects at the final offset before the next press.
                UpdateWindow(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (g_app->storageDragging) {
                g_app->storageDragging = false;
                UpdateWindow(hwnd);
            }
            return 0;

        case WM_MOUSEMOVE: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (g_app->storageDragging) {
                RECT track = g_app->storageTrack;
                const int thumbH = (int)(g_app->storageThumb.bottom - g_app->storageThumb.top);
                const int travel = std::max(1, (int)(track.bottom - track.top) - thumbH);
                const int offset = std::clamp((int)pt.y - g_app->storageDragOffset -
                                                  (int)track.top, 0, travel);
                g_app->scrollStorage = MulDiv(offset, g_app->storageScrollMax, travel);
                g_app->hoverId = HitNone;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            bool enabled = false;
            int id = HitTest(g_hits, pt, &enabled);
            if (id != g_app->hoverId) {
                g_app->hoverId = id;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (id != HitNone && enabled) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
            } else {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            }
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                bool enabled = false;
                int id = HitTest(g_hits, pt, &enabled);
                SetCursor(LoadCursorW(nullptr, (id != HitNone && enabled) ? IDC_HAND : IDC_ARROW));
                return 1;
            }
            break;

        case WM_LBUTTONDOWN: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (g_app->page == Page::Storage && g_app->storageScrollMax > 0) {
                if (PtInRect(&g_app->storageThumb, pt)) {
                    g_app->storageDragging = true;
                    g_app->storageDragOffset = (int)pt.y - (int)g_app->storageThumb.top;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                // Clicking the track pages the list; the thumb keeps its own drag.
                if (PtInRect(&g_app->storageTrack, pt)) {
                    const int direction = pt.y < g_app->storageThumb.top ? -1 : 1;
                    g_app->scrollStorage = std::clamp(
                        g_app->scrollStorage + direction * g_app->storagePageStep, 0,
                        g_app->storageScrollMax);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            bool enabled = false;
            int id = HitTest(g_hits, pt, &enabled);
            HandleHit(id, enabled);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            const int notches = delta / WHEEL_DELTA;
            int step = notches * 64;
            // The wheel goes to whatever the pointer is over: the sidebar scrolls its own
            // entries, a page scrolls its list. That is what keeps every entry reachable
            // when the navigation no longer fits on a short window.
            POINT cursor = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &cursor);
            const LONG sidebarRight = (LONG)Scaled(214);
            if (g_app->showLicense) {
                // Only the agreement scrolls while it covers the window.
                g_app->licenseScroll = std::max(0, g_app->licenseScroll - step);
            } else if (cursor.x < sidebarRight) {
                g_app->scrollNav = std::max(0, g_app->scrollNav - step);
            } else if (g_app->page == Page::Services) {
                g_app->scrollServices = std::max(0, g_app->scrollServices - step);
            } else if (g_app->page == Page::Tasks) {
                g_app->scrollTasks = std::max(0, g_app->scrollTasks - step);
            } else if (g_app->page == Page::Cleanup) {
                g_app->scrollCleanup = std::max(0, g_app->scrollCleanup - step);
            } else if (g_app->page == Page::Startup) {
                g_app->scrollStartup = std::max(0, g_app->scrollStartup - step);
            } else if (g_app->page == Page::LargeFiles) {
                g_app->scrollLarge = std::max(0, g_app->scrollLarge - step);
            } else if (g_app->page == Page::Deactivate) {
                g_app->scrollDeactivate = std::max(0, g_app->scrollDeactivate - step);
            } else if (g_app->page == Page::Network) {
                g_app->scrollNet = std::max(0, g_app->scrollNet - step);
            } else if (g_app->page == Page::Dns) {
                g_app->scrollDns = std::max(0, g_app->scrollDns - step);
            } else if (g_app->page == Page::Boost) {
                g_app->scrollBooster = std::max(0, g_app->scrollBooster - step);
            } else if (g_app->page == Page::Dashboard) {
                g_app->scrollDashboard = std::max(0, g_app->scrollDashboard - step);
            } else if (g_app->page == Page::System) {
                g_app->scrollSystem = std::max(0, g_app->scrollSystem - step);
            } else if (g_app->page == Page::Storage) {
                // Storage snaps to whole 78 px rows at 100% DPI, so a fixed 64 px notch
                // would round away at higher scale. Move by the scaled row height instead.
                g_app->scrollStorage =
                    std::max(0, g_app->scrollStorage - notches * (int)Scaled(78));
            } else if (g_app->page == Page::Settings) {
                g_app->scrollSettings = std::max(0, g_app->scrollSettings - step);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_TIMER:
            if (wp == 1) {
                UpdateDynamicValues();
                if (g_app->storageBusy) {
                    UpdateStorageProgress();
                    if (g_app->page == Page::Storage)
                        InvalidateRect(hwnd, nullptr, FALSE);
                }
                if (g_app->page == Page::Dashboard || g_app->page == Page::System)
                    InvalidateRect(hwnd, nullptr, FALSE);
                // The Cleanup page measures itself when it opens and re-measures in the
                // background after that, so totals follow temp files appearing again
                // without anyone pressing Scan now.
                if (g_app->page == Page::Cleanup && OnScreen(hwnd)) {
                    EnsureCleanupMeasured();
                    if (++g_quietTick >= g_quietInterval.load()) {
                        g_quietTick = 0;
                        StartQuietScan();
                    }
                } else {
                    g_quietTick = 0;
                }
                // The Memory page keeps its own readout live while it is visible.
                if (g_app->page == Page::Memory && OnScreen(hwnd)) {
                    if (++g_memTick >= 2) {
                        g_memTick = 0;
                        LoadMemoryPage(*g_app);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                } else {
                    g_memTick = 0;
                }
                // The Task Manager follows processes as they appear and go; the Services
                // page follows start types changed elsewhere, such as by services.msc.
                // While a process is selected the list is left alone: rows appearing and
                // going would move it out from under the buttons about to be pressed. The
                // selection is re-found by pid whenever the list is read again.
                if (g_app->page == Page::Tasks && OnScreen(hwnd) && g_app->taskSelected < 0) {
                    if (++g_taskTick >= 3) {
                        g_taskTick = 0;
                        LoadTasksPage(*g_app);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                } else {
                    g_taskTick = 0;
                }
                if (g_app->page == Page::Services && OnScreen(hwnd)) {
                    if (++g_svcTick >= 5) {
                        g_svcTick = 0;
                        ReloadServices();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                } else {
                    g_svcTick = 0;
                }

                // The DNS page keeps its adapter list and incident count live.
                if (g_app->page == Page::Dns && OnScreen(hwnd)) {
                    g_app->dnsIncidents = g_dnsIncidentCount.load();
                    if (++g_dnsTick >= 5) {
                        g_dnsTick = 0;
                        LoadDnsPage(*g_app);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                } else {
                    g_dnsTick = 0;
                }

                // Smart RAM's automatic clean, folded into the Memory page.
                if (!g_app->memBusy && !g_workerRunning.load()) {
                    const MemoryPrefs& prefs = g_app->memPrefs;
                    if (prefs.autoClean || prefs.idleClean) g_app->mem = CollectMemStats();
                    if (g_app->mem.total > 0) {
                        uint64_t used = g_app->mem.total - g_app->mem.avail;
                        double usedPct = 100.0 * (double)used / (double)g_app->mem.total;
                        ULONGLONG now = GetTickCount64();
                        bool thresholdHit = prefs.autoClean && usedPct >= prefs.threshold &&
                                            (g_lastAutoClean == 0 ||
                                             now - g_lastAutoClean > 120000);
                        bool idleNow = prefs.idleClean && SystemIdleFor(300);
                        if (!idleNow) g_idleCleanDone = false;
                        bool idleHit = prefs.idleClean && idleNow && !g_idleCleanDone;
                        if (thresholdHit || idleHit) {
                            if (idleHit) g_idleCleanDone = true;
                            g_lastAutoClean = now;
                            StartMemoryClean(false);
                        }
                    }
                }
                // Re-enumerate volumes periodically so a drive plugged in while the
                // Dashboard or System page is open appears without pressing Refresh.
                if ((g_app->page == Page::Dashboard || g_app->page == Page::System) &&
                    OnScreen(hwnd)) {
                    if (++g_storageTick >= 5) {
                        g_storageTick = 0;
                        g_app->storage = CollectStorageDevices();
                    }
                } else {
                    g_storageTick = 0;
                }
            }
            return 0;

        case WM_APP_BROWSER_SCAN_DONE: {
            std::vector<BrowserEntry> fresh;
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                fresh = g_browserScanResult;
            }
            g_app->browserScanning = false;
            if (!fresh.empty()) {
                // Whatever was ticked before the scan stays ticked.
                for (size_t i = 0; i < fresh.size() && i < g_app->browsers.size(); ++i)
                    fresh[i].selected = g_app->browsers[i].selected && fresh[i].installed &&
                                        !fresh[i].running;
                g_app->browsers = std::move(fresh);
            }
            g_app->browserScanned = true;
            uint64_t total = 0;
            int count = 0;
            for (const auto& browser : g_app->browsers)
                if (browser.installed) {
                    ++count;
                    if (!browser.running) total += browser.bytes;
                }
            wchar_t buf[256];
            swprintf(buf, 256, L"%ls found across %d browser(s). A running browser is skipped.",
                     FormatBytes(total).c_str(), count);
            g_app->browserMessage = buf;
            g_app->browserMessageIsError = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_BROWSER_CLEAN_DONE: {
            BrowserCleanResult result;
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                result = g_browserCleanResult;
            }
            g_app->browserCleaning = false;
            wchar_t buf[256];
            swprintf(buf, 256, L"Removed %ls in %lu file(s), %lu skipped.",
                     FormatBytes(result.bytesFreed).c_str(), (unsigned long)result.deleted,
                     (unsigned long)result.skipped);
            LoadBrowsersPage(*g_app);
            g_app->browserMessage = buf;
            g_app->browserMessageIsError = result.deleted == 0 && result.skipped > 0;
            StartBrowserScan();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_LATENCY_DONE: {
            LatencyResult* result = (LatencyResult*)lp;
            if (!g_app) {
                delete result;
                return 0;
            }
            g_app->netPinging = false;
            const bool queuedApply = g_app->netApplyPending;
            g_app->netApplyPending = false;
            if (!result) {
                if (queuedApply) ApplyGamingProfileNow();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (!result->ok) {
                g_app->netPingMessage = result->error;
                g_app->netPingMessageIsError = true;
            } else if (result->host != g_app->netHost) {
                // The field changed while the worker was pinging; those numbers belong to
                // the host that is no longer asked for.
                g_app->netPingMessage = L"The host changed while pinging - measure again.";
                g_app->netPingMessageIsError = true;
            } else if (!g_app->netBefore.ok) {
                g_app->netBefore = *result;
                g_app->netPingMessage = queuedApply
                                            ? L"Baseline saved for " + result->address +
                                                  L" - Measure after compares against it."
                                            : L"Baseline saved for " + result->address;
                g_app->netPingMessageIsError = false;
            } else {
                // No second message: with both readings in, the comparison line under the
                // figures is the result, and it should not be hidden behind a status line.
                g_app->netAfter = *result;
                g_app->netPingMessage.clear();
                g_app->netPingMessageIsError = false;
            }
            delete result;
            // The profile that was waiting for its baseline goes on now - a failed
            // measurement does not cancel the press that asked for it.
            if (queuedApply) ApplyGamingProfileNow();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_BOOST_DONE: {
            BoostResult result;
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                result = g_boostResult;
            }
            g_app->boostBusy = false;
            LoadBoosterPage();
            g_app->boosterMessage = result.message;
            if (!g_boostTweakNote.empty()) g_app->boosterMessage += L"  " + g_boostTweakNote;
            g_app->boosterMessageIsError = !result.ok || g_boostTweakNoteIsError;
            g_boostTweakNote.clear();
            g_boostTweakNoteIsError = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_MEM_DONE: {
            MemCleanResult result;
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                result = g_memResult;
            }
            g_app->memBusy = false;
            if (g_app->memPrefs.clearClipboard) ClearClipboardNow();
            g_app->memTrimmed = result.trimmed;
            g_app->memSkipped = result.skipped;
            g_app->memReclaimed = result.reclaimed;
            LoadMemoryPage(*g_app);

            wchar_t buf[320];
            swprintf(buf, 320, L"Trimmed %lu processes, skipped %lu, reclaimed %ls.",
                     (unsigned long)result.trimmed, (unsigned long)result.skipped,
                     FormatBytes(result.reclaimed).c_str());
            g_app->memMessage = buf;
            if (result.purgedStandby) g_app->memMessage += L" Standby cache purged.";
            g_app->memMessageIsError = false;
            if (!result.error.empty()) {
                g_app->memMessage += L" " + result.error;
                g_app->memMessageIsError = true;
            }
            // Started from the tray menu: the window is hidden, so the result has to be
            // visible somewhere or the press looks like it did nothing.
            if (!IsWindowVisible(hwnd)) TrayBalloon(L"Memory clean", g_app->memMessage);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_QUIET_SCAN_DONE: {
            if (!g_app || g_app->page != Page::Cleanup || g_app->scanning || g_app->cleaning)
                return 0;
            std::vector<CleanGroup> fresh;
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                fresh = g_quietScanResult;
            }
            size_t n = std::min(fresh.size(), g_app->groups.size());
            for (size_t i = 0; i < n; ++i) {
                // Sizes and errors only: whatever is ticked stays ticked.
                g_app->groups[i].bytes = fresh[i].bytes;
                g_app->groups[i].files = fresh[i].files;
                g_app->groups[i].error = fresh[i].error;
                g_app->groups[i].exists = fresh[i].exists;
                g_app->groups[i].scanned = fresh[i].scanned;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_SCAN_PROGRESS: {
            if (!g_app || !g_app->scanning) return 0;
            std::vector<CleanGroup> live;
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                live = g_scanLive;
            }
            // Figures and state only: whatever is ticked on screen stays the user's.
            size_t n = std::min(live.size(), g_app->groups.size());
            for (size_t i = 0; i < n; ++i) {
                g_app->groups[i].bytes = live[i].bytes;
                g_app->groups[i].files = live[i].files;
                g_app->groups[i].error = live[i].error;
                g_app->groups[i].exists = live[i].exists;
                g_app->groups[i].scanned = live[i].scanned;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_SCAN_DONE: {
            std::vector<CleanGroup> fresh;
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                fresh = std::move(g_scanResult);
            }
            bool cancelled = wp != 0;
            g_app->scanning = false;
            g_app->scanStopping = false;
            if (cancelled) {
                // Keep what was measured before the stop and leave the rest as they were:
                // a half-walked folder is never shown as a finished figure, and the pass
                // is not restarted on its own - Scan now is the way back in.
                int measured = 0;
                for (const auto& g : g_app->groups)
                    if (g.scanned) ++measured;
                g_app->scanCancelled = true;
                g_app->hasScanned = measured > 0;
                if (measured > 0) {
                    wchar_t buf[160];
                    swprintf(buf, 160, L"Scan stopped - %d of %d folders measured.", measured,
                             (int)g_app->groups.size());
                    g_app->cleanMessage = buf;
                    g_app->cleanMessageIsError = false;
                    g_app->showCleanResult = true;
                } else {
                    g_app->showCleanResult = false;
                }
                // The rows this pass did reach keep their place in the size order; the
                // rest fall back to their earlier figures or below them.
                if (measured > 0) RefreshCleanupOrder(*g_app);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            g_app->hasScanned = true;
            // A scan takes seconds, and ticks (or a Select all / Clear all) made while it
            // ran are the user's: the measured rows take them over by id rather than
            // replacing them, the same rule the quiet scan and the post-clean refresh use.
            ApplyCleanRefresh(std::move(fresh));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_CLEAN_DONE:
            FinishClean();
            return 0;

        case WM_APP_CLEAN_REFRESH_DONE:
            FinishCleanRefresh();
            return 0;

        case WM_APP_STORAGE_DONE: {
            OptimizeOutcome outcome;
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                outcome = g_storageOutcome;
            }
            g_app->storageBusy = false;
            g_app->storageStarted = 0;
            g_app->storageAction.clear();
            g_app->storagePercent = -1;
            g_storagePercent.store(-1);
            g_app->storageMessage = outcome.message;
            g_app->storageMessageIsError = !outcome.ok && !outcome.cancelled;
            LoadSystemPage(*g_app);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_LARGE_SCAN_DONE: {
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                g_app->largeFiles = g_largeScanResult;
            }
            g_app->largeScanning = false;
            g_app->largeScanned = true;
            g_app->largeMessage.clear();
            g_app->largeMessageIsError = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_LARGE_DELETE_DONE: {
            LargeDeleteResult result;
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                result = g_largeDeleteResult;
            }
            g_app->largeDeleting = false;
            auto& files = g_app->largeFiles;
            files.erase(std::remove_if(files.begin(), files.end(),
                                       [](const LargeFile& file) {
                                           return GetFileAttributesW(file.path.c_str()) ==
                                                  INVALID_FILE_ATTRIBUTES;
                                       }),
                        files.end());

            wchar_t buf[320];
            if (result.failed == 0) {
                swprintf(buf, 320, L"Sent %lu files (%ls) to the Recycle Bin.",
                         (unsigned long)result.removed, FormatBytes(result.bytes).c_str());
            } else {
                swprintf(buf, 320,
                         L"Sent %lu files (%ls) to the Recycle Bin; %lu failed. %ls",
                         (unsigned long)result.removed, FormatBytes(result.bytes).c_str(),
                         (unsigned long)result.failed, result.firstError.c_str());
            }
            g_app->largeMessage = buf;
            g_app->largeMessageIsError = result.failed > 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            // Window tracking sizes are physical pixels, so the minimum travels with the
            // DPI scale. Otherwise a 125% screen would only get 784 x 512 of layout.
            mmi->ptMinTrackSize.x = (LONG)(980 * g_scale + 0.5f);
            mmi->ptMinTrackSize.y = (LONG)(640 * g_scale + 0.5f);
            return 0;
        }

        case WM_DPICHANGED: {
            float dpi = (float)HIWORD(wp);
            g_scale = dpi / 96.0f;
            g_fonts.Create(g_scale);
            // Windows hands over the rectangle that keeps the window the same size on
            // screen at the new scale. Following it is what stops the layout from being
            // squeezed - and overlapping itself - when the window moves to a monitor
            // with a different scale.
            const RECT* suggested = (const RECT*)lp;
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_DESTROY:
            // Never leave defrag.exe running with no window to report to. Closing to
            // the tray only hides the window, so this is the real exit path.
            if (g_app && g_app->storageBusy) CancelVolumeOptimize();
            RemoveTrayIcon();
            KillTimer(hwnd, 1);
            g_dnsWatchdogRunning.store(false);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Resource id of the application icon in src/app.rc.
#define IDI_APPICON 101

// The scale Windows is using for the primary display, before a window exists. The app is
// per-monitor DPI aware, so every size below is a physical pixel count for that scale.
static float SystemScale() {
    typedef UINT(WINAPI * DpiForSystemFn)();
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        // Through void*: the address is checked for null before use, and the cast only
        // describes the prototype the export actually has.
        auto dpiForSystem = (DpiForSystemFn)(void*)GetProcAddress(user32, "GetDpiForSystem");
        if (dpiForSystem) {
            UINT dpi = dpiForSystem();
            if (dpi) return dpi / 96.0f;
        }
    }
    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi / 96.0f : 1.0f;
}

// Loads our own icon at the size Windows asks for; falls back to the stock
// application icon if the resource is missing (e.g. an exe built without app.rc).
static HICON LoadAppIcon(HINSTANCE inst, int cx, int cy) {
    HICON icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, cx, cy,
                                  LR_DEFAULTCOLOR);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

static bool CreateMainWindow(HINSTANCE inst) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadAppIcon(inst, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm =
        LoadAppIcon(inst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    wc.lpszClassName = L"ZanpharsWinSuiteWindow";
    if (!RegisterClassExW(&wc)) return false;

    // The product opens at 1024 x 768 of layout, centred on the screen it starts on, and
    // the window can still be dragged smaller (down to the tracked minimum) or larger.
    // The size is scaled here because a per-monitor DPI aware window is measured in
    // physical pixels: on a 125% display that is 1280 x 960, which gives the layout the
    // same room it has at 100% instead of squeezing it until text and buttons collide.
    g_scale = SystemScale();
    g_fonts.Create(g_scale);

    RECT work = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
        SetRect(&work, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    int workW = (int)(work.right - work.left);
    int workH = (int)(work.bottom - work.top);
    int w = std::min((int)(1024 * g_scale + 0.5f), workW);
    int h = std::min((int)(768 * g_scale + 0.5f), workH);
    int x = work.left + std::max(0, (workW - w) / 2);
    int y = work.top + std::max(0, (workH - h) / 2);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Zanphar's WinSuite", WS_OVERLAPPEDWINDOW,
                             x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return false;

    // The class icons already cover the title bar, taskbar and Alt+Tab; setting them on
    // the window too keeps tools that ask the window (WM_GETICON) from getting NULL.
    SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    auto dpiForWindow =
        (GetDpiForWindowFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                 "GetDpiForWindow");
    g_scale = dpiForWindow ? (float)dpiForWindow(g_hwnd) / 96.0f : 1.0f;
    if (g_scale <= 0) g_scale = 1.0f;
    g_fonts.Create(g_scale);

    // Settings can ask for a silent start: the icon goes into the notification area and the
    // window is never shown, so the tray menu is the way in. A shell that refuses the icon
    // gets the window instead - a hidden app with nothing to click would look crashed.
    if (g_app && g_app->startInTray && AddTrayIcon()) {
        UpdateWindow(g_hwnd);
        return true;
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    return true;
}

// ------------------------------------------------------------------ console
static HANDLE g_stdout = INVALID_HANDLE_VALUE;
static FILE* g_logFile = nullptr;
static std::wstring g_logPath;  // --selftest-log, for an elevated run with no console

static void InitOutput() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        // Already attached or no console; keep whatever handles we inherited.
    }
    g_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!g_logPath.empty()) {
        if (!g_logFile) g_logFile = _wfopen(g_logPath.c_str(), L"w, ccs=UTF-8");
    } else if (g_stdout == INVALID_HANDLE_VALUE || g_stdout == nullptr) {
        g_logFile = _wfopen(L"zanphars-winsuite-selftest.log", L"w, ccs=UTF-8");
    }
}

static void Out(const std::wstring& line) {
    std::wstring withNewline = line + L"\r\n";
    if (g_stdout != INVALID_HANDLE_VALUE && g_stdout != nullptr) {
        DWORD mode = 0;
        if (GetConsoleMode(g_stdout, &mode)) {
            DWORD written = 0;
            WriteConsoleW(g_stdout, withNewline.c_str(), (DWORD)withNewline.size(), &written,
                          nullptr);
        } else {
            int len = WideCharToMultiByte(CP_UTF8, 0, withNewline.c_str(),
                                          (int)withNewline.size(), nullptr, 0, nullptr, nullptr);
            std::string utf8((size_t)len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, withNewline.c_str(), (int)withNewline.size(),
                                utf8.data(), len, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(g_stdout, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        }
    }
    if (g_logFile) {
        fputws(line.c_str(), g_logFile);
        fputws(L"\r\n", g_logFile);
        fflush(g_logFile);
    }
}

// Reads the VERSIONINFO block out of this executable - the same data Windows shows in
// Properties > Details. Returns "<product> <file version>", or empty when missing.
static std::wstring OwnVersionBlock() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return L"";
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0) return L"";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) return L"";

    struct Translation {
        WORD language;
        WORD codePage;
    };
    Translation* translations = nullptr;
    UINT translationBytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&translations,
                        &translationBytes) ||
        translationBytes < sizeof(Translation) || !translations)
        return L"";

    wchar_t table[64];
    swprintf(table, 64, L"\\StringFileInfo\\%04x%04x\\", translations[0].language,
             translations[0].codePage);
    auto query = [&](const wchar_t* key) -> std::wstring {
        std::wstring full = table;
        full += key;
        wchar_t* value = nullptr;
        UINT bytes = 0;
        if (!VerQueryValueW(data.data(), full.c_str(), (void**)&value, &bytes) || !value ||
            bytes == 0)
            return L"";
        return value;
    };

    std::wstring product = query(L"ProductName");
    std::wstring version = query(L"FileVersion");
    if (product.empty() || version.empty()) return L"";
    return product + L" " + version;
}

// ----------------------------------------------------------------- self-test
static int g_passes = 0;
static int g_failures = 0;
static int g_skipped = 0;

static void Check(bool ok, const std::wstring& label, const std::wstring& detail = L"") {
    if (ok)
        ++g_passes;
    else
        ++g_failures;
    Out((ok ? L"[PASS] " : L"[FAIL] ") + label + (detail.empty() ? L"" : L"  -> " + detail));
}

// A few checks describe the machine rather than the program: a clean CI runner
// has no configured adapter and can have no third-party auto-start service at
// all. Those states are not defects, so they are reported as [SKIP] and do not
// fail the run - the self-test then means the same thing on a build machine as
// on a desktop.
static void Skip(const std::wstring& label, const std::wstring& reason) {
    ++g_skipped;
    Out(L"[SKIP] " + label + (reason.empty() ? L"" : L"  -> " + reason));
}

static const wchar_t* StartTypeNameForLog(DWORD start) {
    switch (start) {
        case SERVICE_AUTO_START: return L"Automatic";
        case SERVICE_DEMAND_START: return L"Manual";
        case SERVICE_BOOT_START: return L"Boot";
        case SERVICE_SYSTEM_START: return L"System";
        case SERVICE_DISABLED: return L"Disabled";
        default: return L"Unknown";
    }
}

static bool RegistryRoundTrip(std::wstring& detail) {
    const std::wstring base = L"Software\\ZanpharsWinSuite\\SelfTest";
    const std::wstring runKey = base + L"\\Run";
    const std::wstring backupKey = base + L"\\Backup";
    std::wstring err;
    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());

    RegValue original;
    original.type = REG_EXPAND_SZ;
    original.data = L"%SystemRoot%\\notepad.exe";
    if (!WriteRegValue(HKEY_CURRENT_USER, runKey, L"DemoEntry", KEY_WOW64_64KEY, original, err)) {
        detail = L"write failed: " + err;
        return false;
    }
    RegValue readBack;
    if (!ReadRegValue(HKEY_CURRENT_USER, runKey, L"DemoEntry", KEY_WOW64_64KEY, readBack, err)) {
        detail = L"read failed: " + err;
        return false;
    }
    if (readBack.data != original.data || readBack.type != original.type) {
        detail = L"round-trip mismatch";
        return false;
    }

    // Disable: move the value into the backup key.
    RegValue stored;
    stored.type = REG_SZ;
    stored.data = std::to_wstring(readBack.type) + L"|" + readBack.data;
    if (!WriteRegValue(HKEY_CURRENT_USER, backupKey, L"HKCU_RUN|DemoEntry", 0, stored, err)) {
        detail = L"backup write failed: " + err;
        return false;
    }
    if (!DeleteRegValue(HKEY_CURRENT_USER, runKey, L"DemoEntry", KEY_WOW64_64KEY, err)) {
        detail = L"delete failed: " + err;
        return false;
    }

    // Enable: restore exactly.
    RegValue backup;
    if (!ReadRegValue(HKEY_CURRENT_USER, backupKey, L"HKCU_RUN|DemoEntry", 0, backup, err)) {
        detail = L"backup read failed: " + err;
        return false;
    }
    size_t bar = backup.data.find(L'|');
    RegValue restored;
    restored.type = (DWORD)_wtoi(backup.data.substr(0, bar).c_str());
    restored.data = backup.data.substr(bar + 1);
    if (!WriteRegValue(HKEY_CURRENT_USER, runKey, L"DemoEntry", KEY_WOW64_64KEY, restored, err)) {
        detail = L"restore failed: " + err;
        return false;
    }
    RegValue finalValue;
    if (!ReadRegValue(HKEY_CURRENT_USER, runKey, L"DemoEntry", KEY_WOW64_64KEY, finalValue, err)) {
        detail = L"final read failed: " + err;
        return false;
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    if (finalValue.data != original.data || finalValue.type != original.type) {
        detail = L"restored value differs from original";
        return false;
    }
    detail = L"REG_EXPAND_SZ value preserved through disable/enable";
    return true;
}

static bool StartupToggleTest(std::wstring& detail) {
    const std::wstring runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const std::wstring backupKey = L"Software\\ZanpharsWinSuite\\DisabledStartup";
    const std::wstring valueName = L"ZanpharSelfTestEntry";
    std::wstring err;

    RegValue original;
    original.type = REG_SZ;
    original.data = L"\"C:\\Windows\\System32\\notepad.exe\" /selftest";
    if (!WriteRegValue(HKEY_CURRENT_USER, runKey, valueName, KEY_WOW64_64KEY, original, err)) {
        detail = L"could not create test Run value: " + err;
        return false;
    }

    auto findIndex = [](const std::vector<StartupItem>& items) {
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].valueName == L"ZanpharSelfTestEntry" && items[i].sourceId == L"HKCU_RUN")
                return (int)i;
        return -1;
    };

    bool ok = true;
    auto items = LoadStartupItems();
    int index = findIndex(items);
    if (index < 0) {
        detail = L"test value was not enumerated";
        ok = false;
    } else if (!SetStartupEnabled(items[index], false, err)) {
        detail = L"disable failed: " + err;
        ok = false;
    } else {
        auto disabled = LoadStartupItems();
        int disabledIndex = findIndex(disabled);
        if (disabledIndex < 0 || disabled[disabledIndex].enabled) {
            detail = L"entry not reported as disabled";
            ok = false;
        } else if (!SetStartupEnabled(disabled[disabledIndex], true, err)) {
            detail = L"re-enable failed: " + err;
            ok = false;
        } else {
            RegValue restored;
            std::wstring readErr;
            if (!ReadRegValue(HKEY_CURRENT_USER, runKey, valueName, KEY_WOW64_64KEY, restored,
                              readErr)) {
                detail = L"entry missing after re-enable: " + readErr;
                ok = false;
            } else if (restored.data != original.data) {
                detail = L"restored command text differs";
                ok = false;
            } else {
                detail = L"created, disabled, restored and removed";
            }
        }
    }

    std::wstring ignored;
    DeleteRegValue(HKEY_CURRENT_USER, runKey, valueName, KEY_WOW64_64KEY, ignored);
    DeleteRegValue(HKEY_CURRENT_USER, backupKey, L"HKCU_RUN|ZanpharSelfTestEntry", 0, ignored);
    return ok;
}

static bool TempDeleteTest(std::wstring& detail) {
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring root = std::wstring(tempDir) + L"ZanpharsWinSuite-selftest";
    std::wstring nested = root + L"\\nested";
    std::wstring outside = std::wstring(tempDir) + L"ZanpharsWinSuite-selftest-outside.txt";

    auto writeFile = [](const std::wstring& path, const char* data, DWORD len) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool ok = WriteFile(h, data, len, &written, nullptr) && written == len;
        CloseHandle(h);
        return ok;
    };

    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(nested.c_str(), nullptr);
    bool created = writeFile(root + L"\\a.txt", "alpha", 5) &&
                   writeFile(root + L"\\b.bin", "bravo-bravo", 11) &&
                   writeFile(nested + L"\\c.txt", "charlie", 7) &&
                   writeFile(outside, "keep me", 7);
    if (!created) {
        detail = L"could not create test files";
        return false;
    }

    CleanResult result = DeleteUnderRoot(root);
    DWORD rootAttrs = GetFileAttributesW(root.c_str());
    bool rootKept = rootAttrs != INVALID_FILE_ATTRIBUTES && (rootAttrs & FILE_ATTRIBUTE_DIRECTORY);
    bool nestedGone = GetFileAttributesW(nested.c_str()) == INVALID_FILE_ATTRIBUTES;
    bool outsideKept = GetFileAttributesW(outside.c_str()) != INVALID_FILE_ATTRIBUTES;
    DeleteFileW(outside.c_str());
    RemoveDirectoryW(root.c_str());

    if (!rootKept) {
        detail = L"the cleanup root folder itself was deleted";
        return false;
    }
    if (!nestedGone) {
        detail = L"an emptied subfolder was left behind";
        return false;
    }
    if (!outsideKept) {
        detail = L"deleter removed a file outside the target folder";
        return false;
    }
    wchar_t buf[256];
    swprintf(buf, 256,
             L"deleted %lu files / freed %ls; root kept, empty subfolder removed, "
             L"neighbouring file untouched",
             (unsigned long)result.deleted, FormatBytes(result.bytesFreed).c_str());
    detail = buf;
    return result.deleted == 3;
}

// A target flagged personal (the user's Downloads folder) must hand its files to the
// Recycle Bin, keep its own folder in place, and never count itself as freed space. An
// ordinary cache row must still delete outright. Both go through CleanGroupContents,
// the function StartClean calls, so the routing itself is what the check exercises.
static bool CleanupRecycleTest(std::wstring& detail) {
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    auto writeFile = [](const std::wstring& path, const char* data, DWORD len) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool ok = WriteFile(h, data, len, &written, nullptr) && written == len;
        CloseHandle(h);
        return ok;
    };

    std::wstring personalRoot = std::wstring(tempDir) + L"ZanpharsWinSuite-recycle";
    std::wstring nested = personalRoot + L"\\folder";
    std::wstring cacheRoot = std::wstring(tempDir) + L"ZanpharsWinSuite-recycle-cache";
    CreateDirectoryW(personalRoot.c_str(), nullptr);
    CreateDirectoryW(nested.c_str(), nullptr);
    CreateDirectoryW(cacheRoot.c_str(), nullptr);
    bool created = writeFile(personalRoot + L"\\a.txt", "alpha", 5) &&
                   writeFile(nested + L"\\b.txt", "bravo", 5) &&
                   writeFile(cacheRoot + L"\\c.tmp", "abc", 3);
    if (!created) {
        DeleteFileW((nested + L"\\b.txt").c_str());
        DeleteFileW((personalRoot + L"\\a.txt").c_str());
        DeleteFileW((cacheRoot + L"\\c.tmp").c_str());
        RemoveDirectoryW(nested.c_str());
        RemoveDirectoryW(personalRoot.c_str());
        RemoveDirectoryW(cacheRoot.c_str());
        detail = L"could not create test files";
        return false;
    }

    CleanGroup personal;
    personal.id = L"downloads";
    personal.path = personalRoot;
    personal.personal = true;
    CleanResult recycled = CleanGroupContents(personal);

    CleanGroup cache;
    cache.id = L"user_temp";
    cache.path = cacheRoot;
    CleanResult deletedOutright = CleanGroupContents(cache);

    DWORD rootAttrs = GetFileAttributesW(personalRoot.c_str());
    bool rootKept = rootAttrs != INVALID_FILE_ATTRIBUTES && (rootAttrs & FILE_ATTRIBUTE_DIRECTORY);
    bool insideGone =
        GetFileAttributesW((personalRoot + L"\\a.txt").c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(nested.c_str()) == INVALID_FILE_ATTRIBUTES;

    DeleteFileW((nested + L"\\b.txt").c_str());
    DeleteFileW((personalRoot + L"\\a.txt").c_str());
    DeleteFileW((cacheRoot + L"\\c.tmp").c_str());
    RemoveDirectoryW(nested.c_str());
    RemoveDirectoryW(personalRoot.c_str());
    RemoveDirectoryW(cacheRoot.c_str());

    if (!rootKept) {
        detail = L"the personal folder itself went into the bin";
        return false;
    }
    if (!insideGone) {
        detail = L"a personal file was left on disk";
        return false;
    }
    if (recycled.bytesFreed != 0 || recycled.bytesRecycled != 10 || recycled.deleted != 2) {
        detail = L"personal target reported " + FormatBytes(recycled.bytesFreed) + L" freed, " +
                 FormatBytes(recycled.bytesRecycled) + L" recycled, " +
                 FormatCount(recycled.deleted) + L" files";
        return false;
    }
    if (deletedOutright.bytesRecycled != 0 || deletedOutright.bytesFreed != 3 ||
        deletedOutright.deleted != 1) {
        detail = L"cache target reported " + FormatBytes(deletedOutright.bytesFreed) +
                 L" freed, " + FormatBytes(deletedOutright.bytesRecycled) + L" recycled, " +
                 FormatCount(deletedOutright.deleted) + L" files";
        return false;
    }
    detail = L"Downloads row recycled 2 files / " + FormatBytes(recycled.bytesRecycled) +
             L", cache row deleted " + FormatBytes(deletedOutright.bytesFreed) +
             L", folder kept";
    return true;
}

static bool LargeFileTest(std::wstring& detail) {
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring root = std::wstring(tempDir) + L"ZanpharsWinSuite-largefiles";
    std::wstring bigPath = root + L"\\big.dat";
    std::wstring smallPath = root + L"\\small.txt";
    CreateDirectoryW(root.c_str(), nullptr);

    HANDLE big = CreateFileW(bigPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (big == INVALID_HANDLE_VALUE) {
        detail = L"could not create the test file";
        return false;
    }
    LARGE_INTEGER size = {};
    size.QuadPart = 2 * 1024 * 1024;  // sparse: no real disk usage
    SetFilePointerEx(big, size, nullptr, FILE_BEGIN);
    SetEndOfFile(big);
    CloseHandle(big);

    HANDLE small = CreateFileW(smallPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (small != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(small, "tiny", 4, &written, nullptr);
        CloseHandle(small);
    }

    LargeFileScan options;
    options.minBytes = 1024 * 1024;
    options.maxResults = 8;
    std::vector<LargeFile> found;
    ScanLargeFiles({root}, options, found);
    bool foundBig =
        found.size() == 1 && _wcsicmp(found[0].path.c_str(), bigPath.c_str()) == 0;

    std::wstring errSystem, errOutside, errInside;
    bool blocksSystem = !CanRemoveLargeFile(L"C:\\Windows\\notepad.exe", {root}, errSystem);
    bool blocksOutside =
        !CanRemoveLargeFile(bigPath, {std::wstring(tempDir) + L"somewhere-else"}, errOutside);
    bool allowsInside = CanRemoveLargeFile(bigPath, {root}, errInside);

    LargeDeleteResult recycled = RemoveLargeFiles({smallPath}, {root});
    bool recycleOk = recycled.removed == 1 &&
                     GetFileAttributesW(smallPath.c_str()) == INVALID_FILE_ATTRIBUTES;

    DeleteFileW(bigPath.c_str());
    DeleteFileW(smallPath.c_str());
    RemoveDirectoryW(root.c_str());

    if (!foundBig)
        detail = L"the size filter did not return the oversized test file";
    else if (!blocksSystem)
        detail = L"a protected Windows path was not blocked";
    else if (!blocksOutside)
        detail = L"a file outside the scanned root was not blocked";
    else if (!allowsInside)
        detail = L"a legitimate file was rejected: " + errInside;
    else if (!recycleOk)
        detail = L"recycle removal failed: " + recycled.firstError;
    else
        detail = L"size filter, guards and recycle all behaved";

    return foundBig && blocksSystem && blocksOutside && allowsInside && recycleOk;
}

static bool ElevationPreferenceTest(std::wstring& detail) {
    bool original = PrefersStandardMode();
    SetStandardModePreference(true);
    bool stored = PrefersStandardMode();
    SetStandardModePreference(false);
    bool cleared = !PrefersStandardMode();
    SetStandardModePreference(original);
    bool restored = PrefersStandardMode() == original;

    if (!stored)
        detail = L"the remembered standard-mode choice was not saved";
    else if (!cleared)
        detail = L"becoming administrator did not clear the stored choice";
    else if (!restored)
        detail = L"the previous preference was not restored";
    else
        detail = std::wstring(L"saved, cleared and restored (was ") +
                 (original ? L"standard mode" : L"prompting") + L")";
    return stored && cleared && restored;
}

// --------------------------------------------------- elevated service test
// Exercises the real deactivation path against a service of our own making: it
// points at this executable, is created Automatic, then disabled and restored with
// the same LoadServices / DeactivateService / ReactivateService calls the page uses.
// It needs administrator rights, so it is a separate mode and never part of
// --selftest, and it only ever touches the service it created.
static bool QueryServiceStartType(SC_HANDLE scm, const std::wstring& name, DWORD& start) {
    SC_HANDLE service = OpenServiceW(scm, name.c_str(), SERVICE_QUERY_CONFIG);
    if (!service) return false;
    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    if (needed == 0) {
        CloseServiceHandle(service);
        return false;
    }
    std::vector<BYTE> buffer(needed, 0);
    auto* config = (QUERY_SERVICE_CONFIGW*)buffer.data();
    bool ok = QueryServiceConfigW(service, config, needed, &needed) != FALSE;
    if (ok) start = config->dwStartType;
    CloseServiceHandle(service);
    return ok;
}

static bool JournalHasService(const std::wstring& name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\ZanpharsWinSuite\\DeactivatedServices", 0, KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS)
        return false;
    LSTATUS status = RegQueryValueExW(key, name.c_str(), nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

// `gameOk` and `gameDetail` carry the second half of the run: the Game Booster service
// rows park and restore the same scratch service through their own engine.
static bool ServiceEndToEndTest(std::wstring& detail, bool& gameOk, std::wstring& gameDetail) {
    gameOk = false;
    if (!IsProcessElevated()) {
        detail = L"needs administrator rights";
        gameDetail = detail;
        return false;
    }

    const std::wstring name = L"ZanpharSelfTestService";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        detail = L"could not open the service manager: " + WinErrorText(GetLastError());
        return false;
    }

    // Clear anything an interrupted earlier run left behind.
    if (SC_HANDLE stale = OpenServiceW(scm, name.c_str(), SERVICE_ALL_ACCESS)) {
        DeleteService(stale);
        CloseServiceHandle(stale);
        Sleep(500);
    }

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // The service is started again by ReactivateService, so give it an argument that
    // makes it exit immediately instead of opening a window as LocalSystem.
    std::wstring image = std::wstring(L"\"") + exe + L"\" --servicetest-noop";
    SC_HANDLE created = CreateServiceW(
        scm, name.c_str(), L"Zanphar deactivation self-test", SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, image.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!created) {
        detail = L"could not create the test service: " + WinErrorText(GetLastError());
        CloseServiceHandle(scm);
        return false;
    }
    CloseServiceHandle(created);

    auto findEntry = [&](const std::vector<ServiceEntry>& list) {
        for (size_t i = 0; i < list.size(); ++i)
            if (_wcsicmp(list[i].name.c_str(), name.c_str()) == 0) return (int)i;
        return -1;
    };

    bool listed = false, offered = false, disabled = false, journaled = false;
    bool restored = false, journalCleared = false;
    ServiceChange off, on;

    auto services = LoadServices();
    int index = findEntry(services);
    if (index >= 0) {
        listed = true;
        offered = services[(size_t)index].canDeactivate;
        if (offered) {
            off = DeactivateService(services[(size_t)index]);
            DWORD start = 0;
            disabled = off.ok && QueryServiceStartType(scm, name, start) &&
                       start == SERVICE_DISABLED;

            auto after = LoadServices();
            int afterIndex = findEntry(after);
            journaled = afterIndex >= 0 && after[(size_t)afterIndex].deactivated &&
                        after[(size_t)afterIndex].originalStart == SERVICE_AUTO_START &&
                        JournalHasService(name);

            if (journaled) {
                on = ReactivateService(after[(size_t)afterIndex]);
                DWORD start = 0;
                restored = on.ok && QueryServiceStartType(scm, name, start) &&
                           start == SERVICE_AUTO_START;
                journalCleared = !JournalHasService(name);
                auto back = LoadServices();
                int backIndex = findEntry(back);
                if (backIndex >= 0 && back[(size_t)backIndex].deactivated) journalCleared = false;
            }
        }
    }

    // The Game Booster rows stop and restore services through their own engine, so the same
    // scratch service proves that path too: parked at Manual, saved, then put back.
    gameOk = GameServiceParkRoundTrip(name, gameDetail);

    if (SC_HANDLE cleanup = OpenServiceW(scm, name.c_str(), SERVICE_ALL_ACCESS)) {
        DeleteService(cleanup);
        CloseServiceHandle(cleanup);
    }
    CloseServiceHandle(scm);

    bool ok = listed && offered && off.ok && disabled && journaled && on.ok && restored &&
              journalCleared;
    if (!listed)
        detail = L"the test service was not listed";
    else if (!offered)
        detail = L"the test service was listed but not offered for deactivation";
    else if (!off.ok)
        detail = L"deactivation failed: " + off.error;
    else if (!disabled)
        detail = L"the start type was not Disabled afterwards";
    else if (!journaled)
        detail = L"the original start type was not journaled as Automatic";
    else if (!on.ok)
        detail = L"reactivation failed: " + on.error;
    else if (!restored)
        detail = L"the start type was not restored to Automatic";
    else if (!journalCleared)
        detail = L"the journal entry survived reactivation";
    else
        detail = L"created an Automatic service, disabled it, restored Automatic and cleared "
                 L"the journal";
    return ok;
}

static int RunServiceTest() {
    std::wstring detail, gameDetail;
    bool gameOk = false;
    bool ok = ServiceEndToEndTest(detail, gameOk, gameDetail);

    // Elevation is requested through UAC, so this usually runs without a console:
    // leave the result next to the exe as well as writing it out.
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring logPath = exe;
    size_t slash = logPath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) logPath.resize(slash + 1);
    logPath += L"servicetest.log";

    std::wstring line = std::wstring(ok ? L"PASS" : L"FAIL") + L"  " + detail;
    line += L"\r\n";
    line += std::wstring(gameOk ? L"PASS" : L"FAIL") + L"  Game Booster services: " +
            gameDetail;
    std::string utf8;
    int len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len > 0) {
        utf8.resize((size_t)len);
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8.data(), len, nullptr, nullptr);
        utf8.resize(strlen(utf8.c_str()));
    }
    HANDLE handle = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(handle, utf8.c_str(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(handle);
    }

    InitOutput();
    Out(L"Service deactivation end-to-end test");
    Out(std::wstring(L"Elevation: ") +
        (IsProcessElevated() ? L"administrator" : L"standard user"));
    Out(line);
    return ok && gameOk ? 0 : 1;
}

// Draws the Storage page into an off-screen bitmap at a chosen scale and returns
// the hit rects, so the DPI/media matrix is checked against the real paint code
// rather than a second copy of the arithmetic.
static bool RenderStorageHits(AppState& st, float scale, std::vector<Hit>& hits) {
    g_scale = scale;
    g_fonts.Create(scale);
    const int w = (int)std::lround(1024.0 * scale);
    const int h = (int)std::lround(768.0 * scale);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateCompatibleBitmap(screen, w, h);
    if (!dib) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    st.page = Page::Storage;
    PaintApp(mem, w, h, st, hits);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return true;
}

static const Hit* StorageHit(const std::vector<Hit>& hits, int id) {
    for (const auto& hit : hits)
        if (hit.id == id) return &hit;
    return nullptr;
}

static bool StorageGeometryTest(std::wstring& detail) {
    const float scales[] = {1.0f, 1.5f, 2.0f};
    bool ok = true;
    std::wstring why;
    const float oldScale = g_scale;

    for (float scale : scales) {
        AppState st;
        auto add = [&](wchar_t letter, const wchar_t* type, bool ssd, bool known) {
            StorageDevice d;
            d.root = std::wstring(1, letter) + L":\\";
            d.label = std::wstring(L"TEST-") + (ssd ? L"SSD" : L"HDD");
            d.typeName = type;
            d.fileSystem = L"NTFS";
            d.bus = ssd ? L"NVMe" : L"SATA";
            d.model = L"matrix test volume";
            d.total = 100ull << 30;
            d.freeBytes = 40ull << 30;
            d.accessible = true;
            d.mediaKnown = known;
            d.ssd = ssd;
            st.storage.push_back(d);
        };
        // Eight rows force a scrollbar at every scale. C is mechanical, D solid-state,
        // E removable, F unknown; G and H fill the first viewport, I and J the rest.
        add(L'C', L"Fixed", false, true);
        add(L'D', L"Fixed", true, true);
        add(L'E', L"Removable", false, true);
        add(L'F', L"Fixed", false, false);
        add(L'G', L"Fixed", true, true);
        add(L'H', L"Fixed", false, true);
        add(L'I', L"Removable", true, true);
        add(L'J', L"Fixed", false, true);

        std::vector<Hit> hits;
        if (!RenderStorageHits(st, scale, hits)) {
            ok = false;
            why += L"no bitmap at ";
            why += std::to_wstring(scale);
            why += L"; ";
            continue;
        }

        wchar_t at[80];
        swprintf(at, 80, L"%.0f%%", scale * 100.0f);
        auto require = [&](bool condition, const wchar_t* what) {
            if (condition) return;
            ok = false;
            why += at;
            why += L" ";
            why += what;
            why += L"; ";
        };

        const Hit* c = StorageHit(hits, HitStorageRowBase + (L'C' - L'A'));
        const Hit* d = StorageHit(hits, HitStorageRowBase + (L'D' - L'A'));
        const Hit* e = StorageHit(hits, HitStorageRowBase + (L'E' - L'A'));
        const Hit* f = StorageHit(hits, HitStorageRowBase + (L'F' - L'A'));
        require(c && d && e && f, L"the four visible media rows have buttons");
        require(c && c->enabled, L"the mechanical volume is actionable");
        require(d && d->enabled, L"the solid-state volume is actionable");
        require(e && e->enabled, L"the removable volume is actionable");
        require(f && !f->enabled, L"unknown media is present but disabled");
        require(st.storageScrollMax > 0, L"eight rows scroll");
        require(st.storageThumb.top >= st.storageTrack.top &&
                    st.storageThumb.bottom <= st.storageTrack.bottom &&
                    st.storageThumb.bottom > st.storageThumb.top,
                L"the scroll thumb stays inside its track");
        require(st.storagePageStep > 0, L"the wheel page step is positive");

        // One wheel notch, expressed in the same scaled row pitch the window uses. C
        // scrolls out of the viewport; D is the first row still visible and must move up.
        const int before = d ? (int)d->rect.top : 0;
        st.scrollStorage = (int)std::lround(78.0 * scale);
        hits.clear();
        RenderStorageHits(st, scale, hits);
        const Hit* moved = StorageHit(hits, HitStorageRowBase + (L'D' - L'A'));
        require(moved && (int)moved->rect.top < before,
                L"a wheel-sized scroll moves the row by its scaled pitch");

        // The thumb at the end of its travel is lower and still inside the track.
        st.scrollStorage = st.storageScrollMax;
        hits.clear();
        RenderStorageHits(st, scale, hits);
        require(st.storageThumb.top > st.storageTrack.top &&
                    st.storageThumb.bottom <= st.storageTrack.bottom,
                L"dragging the thumb to the end stays inside the track");

        // While defrag.exe runs the page must expose Cancel and disable Refresh and
        // every volume button, not merely redraw them as disabled.
        st.storageBusy = true;
        st.storagePercent = 42;
        st.storageAction = L"Defragmenting C:";
        st.storageMessage = L"Defragmenting C: - 00:42 elapsed - 42%";
        st.scrollStorage = 0;
        hits.clear();
        RenderStorageHits(st, scale, hits);
        const Hit* cancel = StorageHit(hits, HitStorageCancel);
        const Hit* refresh = StorageHit(hits, HitStorageRefresh);
        const Hit* busyC = StorageHit(hits, HitStorageRowBase + (L'C' - L'A'));
        require(cancel && cancel->enabled, L"Cancel is the enabled action while busy");
        require(refresh && !refresh->enabled, L"Refresh is disabled while busy");
        require(busyC && !busyC->enabled, L"volume buttons are disabled while busy");
    }

    g_scale = oldScale;
    g_fonts.Create(oldScale);
    detail = why.empty() ? L"100%, 150% and 200%: rows, scrollbar and Cancel hit-test" : why;
    return ok;
}

static int RunSelfTest() {
    Out(L"Zanphar's WinSuite self-test");
    Out(L"==============================");
    Out(std::wstring(L"Elevation: ") +
        (IsProcessElevated() ? L"administrator" : L"standard user (CLI modes never prompt for UAC)"));
    Out(std::wstring(L"Launch prompt: ") +
        (PrefersStandardMode() ? L"suppressed - remembered standard mode"
                               : L"asks for administrator rights"));

    // The exe's Properties > Details tab comes from this block; make sure a resource
    // rebuild never quietly drops it.
    std::wstring versionBlock = OwnVersionBlock();
    Check(!versionBlock.empty() &&
              versionBlock.find(kVersion) != std::wstring::npos,
          L"exe carries a version resource",
          versionBlock.empty() ? L"no VERSIONINFO found" : versionBlock);

    SysInfo info = CollectSystemInfo();
    Out(L"System: " + info.osName + L" " + info.osBuild + L", " + info.arch);
    Out(L"CPU: " + info.cpuName);
    wchar_t buf[256];
    swprintf(buf, 256, L"%u physical / %u logical cores", info.physicalCpus, info.logicalCpus);
    Out(buf);
    Out(L"Memory: " + FormatBytes(info.ramTotal) + L", " + FormatBytes(info.ramAvail) + L" free");
    for (const auto& d : info.drives) {
        swprintf(buf, 256, L"Drive %ls %ls: %ls free of %ls", d.root.c_str(),
                 d.label.c_str(), FormatBytes(d.freeBytes).c_str(),
                 FormatBytes(d.total).c_str());
        Out(buf);
    }
    Out(L"Uptime: " + FormatDuration(info.uptimeSeconds));
    Check(!info.osName.empty() && info.ramTotal > 0 && !info.drives.empty(),
          L"system information collected");

    // Poison the cached free space, then prove the per-second refresh really re-reads it
    // (this is what makes the disk cards move after a cleanup).
    for (auto& d : info.drives) d.freeBytes = 0;
    RefreshDriveSpace(info);
    bool drivesLive = !info.drives.empty();
    for (const auto& d : info.drives) {
        ULARGE_INTEGER freeAvail = {}, total = {}, totalFree = {};
        if (!GetDiskFreeSpaceExW(d.root.c_str(), &freeAvail, &total, &totalFree) ||
            d.freeBytes == 0) {
            drivesLive = false;
            continue;
        }
        uint64_t diff = d.freeBytes > freeAvail.QuadPart ? d.freeBytes - freeAvail.QuadPart
                                                         : freeAvail.QuadPart - d.freeBytes;
        if (diff > (16ull << 20)) drivesLive = false;
    }
    Check(drivesLive, L"drive free space is re-read, not cached");

    Out(L"");
    Out(L"Storage devices:");
    auto storage = CollectStorageDevices();
    bool storageOk = !storage.empty();
    bool hasFixed = false;
    for (const auto& d : storage) {
        std::wstring line =
            L"  - " + d.root + (d.label.empty() ? std::wstring() : L" " + d.label) + L": " +
            d.typeName;
        if (!d.fileSystem.empty()) line += L", " + d.fileSystem;
        if (!d.bus.empty()) line += L", " + d.bus;
        if (!d.model.empty()) line += L", " + d.model;
        line += L" - ";
        if (d.total)
            line += FormatBytes(d.freeBytes) + L" free of " + FormatBytes(d.total);
        else
            line += d.accessible ? L"capacity unavailable" : L"no media";
        Out(line);

        if (d.typeName.empty() || (d.total && d.freeBytes > d.total)) storageOk = false;
        if (d.typeName == L"Fixed") hasFixed = true;
    }
    swprintf(buf, 256, L"%d volumes", (int)storage.size());
    Check(storageOk && hasFixed, L"all storage devices enumerated", buf);

    // The page's buttons and their hit rects are checked at all three supported DPI
    // scales with synthetic mechanical, solid-state, removable and unknown media. This
    // is the geometry the window paints, not a parallel copy of its arithmetic.
    std::wstring storageGeometryDetail;
    Check(StorageGeometryTest(storageGeometryDetail),
          L"Storage rows, scrollbar and Cancel hit-test at 100/150/200%",
          storageGeometryDetail);

    StorageDevice unknownMedia;
    unknownMedia.root = L"Z:\\";
    unknownMedia.typeName = L"Fixed";
    unknownMedia.total = 1ull << 30;
    unknownMedia.mediaKnown = false;
    StorageDevice removableMedia;
    removableMedia.root = L"Y:\\";
    removableMedia.typeName = L"Removable";
    removableMedia.total = 1ull << 30;
    removableMedia.mediaKnown = true;
    StorageDevice networkMedia;
    networkMedia.root = L"X:\\";
    networkMedia.typeName = L"Network";
    networkMedia.total = 1ull << 30;
    networkMedia.mediaKnown = true;
    const std::wstring unknownRefusal = StorageOptimizeRefusal(unknownMedia);
    const std::wstring networkRefusal = StorageOptimizeRefusal(networkMedia);
    const std::wstring removableRefusal = StorageOptimizeRefusal(removableMedia);
    const OptimizeOutcome unknownOutcome = OptimizeVolume(unknownMedia);
    Check(!unknownRefusal.empty() && !networkRefusal.empty() && removableRefusal.empty() &&
              !unknownOutcome.ok && !unknownOutcome.cancelled,
          L"unknown and network media are refused before defrag.exe starts",
          unknownRefusal + L" | " + networkRefusal);

    info = CollectSystemInfo();  // restore real figures for the rest of the run

    double first = SampleCpuUsagePercent();
    double second = SampleCpuUsagePercent();
    swprintf(buf, 256, L"%.1f %%, %.1f %%", first, second);
    Check(first >= 0.0 && first <= 100.0 && second >= 0.0 && second <= 100.0,
          L"CPU sampling within range", buf);

    MemStats mem = CollectMemStats();
    swprintf(buf, 256, L"%ls installed, %ls available, %ls committed of %ls, %lu processes",
             FormatBytes(mem.total).c_str(), FormatBytes(mem.avail).c_str(),
             FormatBytes(mem.committed).c_str(), FormatBytes(mem.commitLimit).c_str(),
             (unsigned long)mem.processCount);
    Out(L"Memory: " + std::wstring(buf));
    bool memSane = mem.total > 0 && mem.avail <= mem.total && mem.commitLimit > 0 &&
                   mem.processCount > 0 && mem.threadCount > 0;
    Check(memSane, L"memory figures collected");
    Out(std::wstring(L"Deep clean: ") +
        (DeepCleanAvailable() ? L"available (administrator)" : L"needs administrator rights"));

    auto procs = TopMemoryProcesses(8);
    bool procsSorted = !procs.empty();
    for (size_t i = 1; i < procs.size(); ++i)
        if (procs[i - 1].workingSet < procs[i].workingSet) procsSorted = false;
    swprintf(buf, 256, L"%lu processes listed, largest %ls (%ls)", (unsigned long)procs.size(),
             procs.empty() ? L"-" : procs[0].name.c_str(),
             procs.empty() ? L"-" : FormatBytes(procs[0].workingSet).c_str());
    Check(procsSorted, L"top memory users listed largest first", buf);

    uint64_t reclaimed = 0;
    std::wstring trimError;
    bool trimmed = TrimProcessWorkingSet(GetCurrentProcessId(), &reclaimed, trimError);
    Check(trimmed, L"working-set trim works on this process",
          trimmed ? L"own working set trimmed, " + FormatBytes(reclaimed) + L" released"
                  : trimError);

    Out(L"");
    Out(L"Cleanup folders:");
    auto groups = BuildCleanGroups();
    for (auto& g : groups) {
        ScanCleanGroup(g);
        if (!g.exists) {
            Out(L"  - " + g.name + L": not present");
            continue;
        }
        swprintf(buf, 256, L"  - %ls: %ls in %lu files (%ls)", g.name.c_str(),
                 FormatBytes(g.bytes).c_str(), (unsigned long)g.files, g.path.c_str());
        Out(buf);
    }
    Check(!groups.empty(), L"cleanup targets resolved");
    bool anyExists = false;
    for (const auto& g : groups) anyExists = anyExists || g.exists;
    Check(anyExists, L"at least one cleanup target exists");

    // Downloads holds the user's own files, so it must never arrive armed.
    bool downloadsListed = false;
    bool downloadsArmed = false;
    for (const auto& g : groups) {
        if (g.id != L"downloads") continue;
        downloadsListed = true;
        downloadsArmed = g.selected;
    }
    Check(downloadsListed && !downloadsArmed, L"Downloads is listed and unticked by default",
          !downloadsListed ? L"not in the cleanup list"
                           : (downloadsArmed ? L"armed before the user chose it"
                                             : L"present, off by default"));

    // Ticking the user's own files must ask first, and the question must carry the
    // count and the destination; caches must never raise a dialog.
    CleanGroup personalRow;
    personalRow.id = L"downloads";
    personalRow.name = L"Your Downloads folder";
    personalRow.personal = true;
    personalRow.selected = true;
    personalRow.bytes = 12ull * 1024 * 1024;
    personalRow.files = 34;
    std::wstring personalPrompt = PersonalCleanPrompt({personalRow});
    bool namesTarget = personalPrompt.find(L"Your Downloads folder") != std::wstring::npos;
    bool namesSize = personalPrompt.find(L"12.0 MB") != std::wstring::npos &&
                     personalPrompt.find(L"34 files") != std::wstring::npos;
    bool namesBin = personalPrompt.find(L"Recycle Bin") != std::wstring::npos;
    CleanGroup cacheRow;
    cacheRow.id = L"user_temp";
    cacheRow.name = L"Your temporary files";
    cacheRow.bytes = 10;
    cacheRow.files = 1;
    bool quietForCaches = PersonalCleanPrompt({cacheRow}).empty();
    Check(namesTarget && namesSize && namesBin && quietForCaches,
          L"personal clean asks first, caches do not",
          L"prompt names target, size and Recycle Bin: " +
              std::wstring(namesTarget && namesSize && namesBin ? L"yes" : L"no") +
              L", cache prompt: " + (quietForCaches ? L"none" : L"present"));

    // The Recycle Bin is a row of its own, measured by the shell rather than walked. Like
    // Downloads it is the user's own data, but emptying it cannot be undone: it must stay
    // unticked by default and its confirmation must say what is about to be destroyed.
    bool binListed = false, binArmed = false;
    for (const auto& g : groups) {
        if (g.id != L"recycle_bin") continue;
        binListed = g.recycleBin && g.personal;
        binArmed = g.selected;
    }
    CleanGroup binRow;
    binRow.id = L"recycle_bin";
    binRow.name = L"Recycle Bin";
    binRow.personal = true;
    binRow.recycleBin = true;
    binRow.bytes = 800ull * 1024 * 1024;
    binRow.files = 120;
    std::wstring binPrompt = PersonalCleanPrompt({binRow});
    bool binNamesSize = binPrompt.find(FormatBytes(binRow.bytes)) != std::wstring::npos;
    bool binWarns = binPrompt.find(L"deleted for good") != std::wstring::npos;
    bool binNeverRecycled = binPrompt.find(L"moved to the Recycle Bin") == std::wstring::npos;
    Check(binListed && !binArmed && binNamesSize && binWarns && binNeverRecycled,
          L"Recycle Bin is listed, unticked and empties only after a warning",
          std::wstring(L"listed: ") + (binListed ? L"yes" : L"no") +
              L", armed by default: " + (binArmed ? L"yes" : L"no") +
              L", prompt names the size: " + (binNamesSize ? L"yes" : L"no") +
              L", warns that it is permanent: " + (binWarns ? L"yes" : L"no"));

    // Ticks outlive the launch: an opt-out comes back, a row nobody touched keeps its
    // built-in default, and the user's own folder never comes back armed. A scratch ini
    // is used so the real settings file is never written from the self-test.
    wchar_t selectionDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, selectionDir);
    std::wstring selectionIni = std::wstring(selectionDir) + L"ZanpharsWinSuite-selection.ini";
    DeleteFileW(selectionIni.c_str());
    std::vector<CleanGroup> chosen = BuildCleanGroups();
    for (auto& g : chosen) {
        if (g.id == L"user_temp") g.selected = false;  // a cache the user opted out of
        if (g.id == L"downloads") g.selected = true;   // stored, but must not come back
    }
    SaveCleanSelection(selectionIni, chosen);

    std::vector<CleanGroup> restoredRows = BuildCleanGroups();
    LoadCleanSelection(selectionIni, restoredRows);
    bool optOutKept = false, defaultKept = false, downloadsUnarmed = false;
    for (const auto& g : restoredRows) {
        if (g.id == L"user_temp") optOutKept = !g.selected;
        if (g.id == L"crash_dumps") defaultKept = g.selected;
        if (g.id == L"downloads") downloadsUnarmed = !g.selected;
    }
    Check(optOutKept && defaultKept && downloadsUnarmed,
          L"cleanup ticks are remembered, Downloads never is",
          std::wstring(L"opt-out restored: ") + (optOutKept ? L"yes" : L"no") +
              L", untouched row kept its default: " + (defaultKept ? L"yes" : L"no") +
              L", Downloads armed after being saved: " + (downloadsUnarmed ? L"no" : L"yes"));

    // The reset control drops the stored choices: every cache comes back ticked and no
    // key is left behind for the next launch to read.
    std::vector<CleanGroup> optedOut = BuildCleanGroups();
    for (auto& g : optedOut)
        if (g.id == L"user_temp" || g.id == L"crash_dumps") g.selected = false;
    SaveCleanSelection(selectionIni, optedOut);
    ResetCleanSelection(selectionIni);
    std::vector<CleanGroup> resetRows = BuildCleanGroups();
    LoadCleanSelection(selectionIni, resetRows);
    bool resetTemp = false, resetDumps = false;
    for (const auto& g : resetRows) {
        if (g.id == L"user_temp") resetTemp = g.selected;
        if (g.id == L"crash_dumps") resetDumps = g.selected;
    }
    wchar_t leftover[8] = {};
    bool keysGone = GetPrivateProfileStringW(L"Cleanup", L"user_temp", L"", leftover, 8,
                                             selectionIni.c_str()) == 0;
    DeleteFileW(selectionIni.c_str());
    Check(resetTemp && resetDumps && keysGone, L"cleanup ticks reset to the built-in defaults",
          std::wstring(L"caches ticked again: ") + (resetTemp && resetDumps ? L"yes" : L"no") +
              L", stored keys left: " + (keysGone ? L"none" : L"some"));

    // Bulk ticks: every target that is actually there, never the user's own files, and
    // clearing takes every row back off.
    CleanGroup presentCache;
    presentCache.id = L"user_temp";
    presentCache.exists = true;
    presentCache.selected = false;
    CleanGroup missingCache;
    missingCache.id = L"prefetch";
    missingCache.exists = false;
    missingCache.selected = false;
    CleanGroup bulkPersonal;
    bulkPersonal.id = L"downloads";
    bulkPersonal.personal = true;
    bulkPersonal.exists = true;
    bulkPersonal.selected = false;
    std::vector<CleanGroup> bulkRows = {presentCache, missingCache, bulkPersonal};
    ApplyBulkCleanSelection(bulkRows, true);
    bool selectedPresent = bulkRows[0].selected;
    bool skippedMissing = !bulkRows[1].selected;
    bool skippedPersonal = !bulkRows[2].selected;
    ApplyBulkCleanSelection(bulkRows, false);
    bool clearedAll = !bulkRows[0].selected && !bulkRows[1].selected && !bulkRows[2].selected;
    // A pass that is stopped measures nothing rather than reporting part of a folder as
    // the whole of it.
    CleanGroup cancelledRow;
    cancelledRow.id = L"user_temp";
    cancelledRow.name = L"cancelled target";
    cancelledRow.path = KnownFolderPath(FOLDERID_LocalAppData);
    cancelledRow.exists = true;
    std::atomic<bool> stopNow{true};
    ScanCleanGroup(cancelledRow, &stopNow);
    Check(!cancelledRow.scanned && cancelledRow.bytes == 0 && cancelledRow.files == 0,
          L"a cancelled scan leaves its row unmeasured",
          std::wstring(L"scanned: ") + (cancelledRow.scanned ? L"yes" : L"no") +
              L", bytes: " + FormatBytes(cancelledRow.bytes) + L", files: " +
              FormatCount(cancelledRow.files));

    Check(selectedPresent && skippedMissing && skippedPersonal && clearedAll,
          L"bulk ticks cover present targets and never Downloads",
          std::wstring(L"present ticked: ") + (selectedPresent ? L"yes" : L"no") +
              L", missing left off: " + (skippedMissing ? L"yes" : L"no") +
              L", Downloads left off: " + (skippedPersonal ? L"yes" : L"no") +
              L", clear all: " + (clearedAll ? L"yes" : L"no"));

    // The row order the page draws: biggest measured figure first, a stopped pass's earlier
    // figure still counted, and rows with nothing to compare left below in built-in order.
    auto figureRow = [](const wchar_t* id, uint64_t bytes, bool scanned, bool exists) {
        CleanGroup g;
        g.id = id;
        g.bytes = bytes;
        g.files = 0;
        g.scanned = scanned;
        g.exists = exists;
        return g;
    };
    AppState orderState;
    orderState.groups = {figureRow(L"a", 100, true, true),
                         figureRow(L"b", 5000, true, true),
                         figureRow(L"c", 700, false, true),  // stays by its earlier figure
                         figureRow(L"d", 900, true, true),
                         figureRow(L"e", 0, true, true),     // measured, and empty
                         figureRow(L"f", 0, false, true),    // never measured
                         figureRow(L"h", 0, false, false)};  // not on disk
    RefreshCleanupOrder(orderState);
    std::wstring shownOrder;
    bool coversAll = orderState.cleanOrder.size() == orderState.groups.size();
    std::vector<int> seenRows(orderState.groups.size(), 0);
    for (int index : orderState.cleanOrder) {
        if (index < 0 || index >= (int)orderState.groups.size()) {
            coversAll = false;
            continue;
        }
        ++seenRows[(size_t)index];
        shownOrder += orderState.groups[(size_t)index].id;
    }
    for (int times : seenRows) coversAll = coversAll && times == 1;
    // The Order switch: the same list in the order it was built, and back again.
    std::vector<int> builtIn;
    for (int i = 0; i < (int)orderState.groups.size(); ++i) builtIn.push_back(i);
    orderState.cleanSortBySize = false;
    RefreshCleanupOrder(orderState);
    bool switchToBuiltIn = orderState.cleanOrder == builtIn;
    orderState.cleanSortBySize = true;
    RefreshCleanupOrder(orderState);
    bool switchBack = orderState.cleanOrder.size() == orderState.groups.size() &&
                      orderState.cleanOrder[(size_t)0] == 1;
    Check(shownOrder == L"bdcaefh" && coversAll && switchToBuiltIn && switchBack,
          L"cleanup rows sort by size, or in built-in order on request",
          std::wstring(L"by size: ") + shownOrder + L", every row once: " +
              (coversAll ? L"yes" : L"no") + L", built-in order: " +
              (switchToBuiltIn ? L"yes" : L"no") + L", switched back: " +
              (switchBack ? L"yes" : L"no"));

    // The Order choice is remembered across launches, and Reset to defaults - which drops
    // the ticks - leaves it alone because it lives in its own section.
    wchar_t viewDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, viewDir) > 0) {
        std::wstring viewPath = std::wstring(viewDir) + L"ZanpharsWinSuite-view-test.ini";
        DeleteFileW(viewPath.c_str());
        bool freshDefaultsToSize = LoadCleanupSortBySize(viewPath);
        SaveCleanupSortBySize(viewPath, false);
        bool builtInRemembered = !LoadCleanupSortBySize(viewPath);
        ResetCleanSelection(viewPath);
        bool survivesTickReset = !LoadCleanupSortBySize(viewPath);
        SaveCleanupSortBySize(viewPath, true);
        bool sizeRemembered = LoadCleanupSortBySize(viewPath);
        DeleteFileW(viewPath.c_str());
        Check(freshDefaultsToSize && builtInRemembered && survivesTickReset && sizeRemembered,
              L"cleanup order choice is remembered across launches",
              std::wstring(L"defaults to biggest first: ") +
                  (freshDefaultsToSize ? L"yes" : L"no") +
                  L", built-in remembered: " + (builtInRemembered ? L"yes" : L"no") +
                  L", survives a tick reset: " + (survivesTickReset ? L"yes" : L"no") +
                  L", back to biggest first: " + (sizeRemembered ? L"yes" : L"no"));
    } else {
        Skip(L"cleanup order choice is remembered across launches",
             L"this machine has no temp folder to write the scratch settings to");
    }

    // The Settings page's tray options: off unless chosen, and remembered when they are.
    if (GetTempPathW(MAX_PATH, viewDir) > 0) {
        std::wstring windowIni = std::wstring(viewDir) + L"ZanpharsWinSuite-window-test.ini";
        DeleteFileW(windowIni.c_str());
        bool freshOff = !LoadWindowPref(windowIni, L"CloseToTray") &&
                        !LoadWindowPref(windowIni, L"MinimizeToTray") &&
                        !LoadWindowPref(windowIni, L"StartInTray");
        // A missing toast key is the one that has to mean on, so an upgrade keeps the
        // reports it already had instead of going quiet.
        bool toastsOnByDefault = LoadToastPref(windowIni);
        // Close-to-tray and start-in-tray on, minimising and toasts turned off.
        SaveWindowPrefs(windowIni, true, false, true, false);
        bool closeKept = LoadWindowPref(windowIni, L"CloseToTray");
        bool minimizeStayedOff = !LoadWindowPref(windowIni, L"MinimizeToTray");
        bool startupKept = LoadWindowPref(windowIni, L"StartInTray");
        bool toastsOff = !LoadToastPref(windowIni);
        SaveWindowPrefs(windowIni, false, true, false, true);
        bool flipped = !LoadWindowPref(windowIni, L"CloseToTray") &&
                       LoadWindowPref(windowIni, L"MinimizeToTray") &&
                       !LoadWindowPref(windowIni, L"StartInTray") && LoadToastPref(windowIni);
        DeleteFileW(windowIni.c_str());
        Check(freshOff && toastsOnByDefault && closeKept && minimizeStayedOff && startupKept &&
                  toastsOff && flipped,
              L"tray options default correctly and are remembered",
              std::wstring(L"fresh profile off: ") + (freshOff ? L"yes" : L"no") +
                  L", toasts on by default: " + (toastsOnByDefault ? L"yes" : L"no") +
                  L", close-to-tray kept: " + (closeKept ? L"yes" : L"no") +
                  L", minimize left off: " + (minimizeStayedOff ? L"yes" : L"no") +
                  L", start-in-tray kept: " + (startupKept ? L"yes" : L"no") +
                  L", toasts switched off: " + (toastsOff ? L"yes" : L"no") +
                  L", flipped: " + (flipped ? L"yes" : L"no"));
    } else {
        Skip(L"tray options default to off and are remembered",
             L"this machine has no temp folder to write the scratch settings to");
    }

    // The read-only agreement cannot lose a clause without this noticing.
    const std::wstring licence = kLicenseText;
    bool licensee = licence.find(L"Copyright (c) 2026-2027, Charles McDonald") != std::wstring::npos;
    bool grant = licence.find(L"Redistribution and use in source and binary forms") !=
                 std::wstring::npos;
    bool thirdClause = licence.find(L"3. Neither the name of the copyright holder") !=
                       std::wstring::npos;
    bool disclaimer = licence.find(L"THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS") !=
                      std::wstring::npos;
    Check(licensee && grant && thirdClause && disclaimer,
          L"licence view carries the full BSD 3-Clause text",
          std::wstring(L"copyright holder: ") + (licensee ? L"yes" : L"no") +
              L", grant clause: " + (grant ? L"yes" : L"no") + L", third clause: " +
              (thirdClause ? L"yes" : L"no") + L", disclaimer: " +
              (disclaimer ? L"yes" : L"no"));

    std::wstring detail;
    bool tempOk = TempDeleteTest(detail);
    Check(detail.empty() ? false : tempOk, L"safe deletion inside temp folder only", detail);

    detail.clear();
    bool recycleOk = CleanupRecycleTest(detail);
    Check(recycleOk, L"Downloads target recycles while caches still delete", detail);

    detail.clear();
    bool largeOk = LargeFileTest(detail);
    Check(largeOk, L"large-file scan, deletion guards and recycle", detail);

    Out(L"");
    auto startup = LoadStartupItems();
    swprintf(buf, 256, L"%d startup entries found:", (int)startup.size());
    Out(buf);
    for (const auto& item : startup) {
        Out(L"  - [" + std::wstring(item.enabled ? L"on " : L"off") + L"] " + item.name +
            L"  (" + item.locationLabel + L")");
    }
    Check(true, L"startup entries enumerated");

    detail.clear();
    bool toggleOk = StartupToggleTest(detail);
    Check(toggleOk, L"startup item can be disabled and re-enabled end to end", detail);

    Out(L"");
    Out(L"Services that start with Windows (third party):");
    auto services = LoadServices();
    for (const auto& service : services) {
        std::wstring line = std::wstring(L"  - [") + (service.deactivated ? L"off" : L"on ") +
                            L"] " + service.display + L"  (" + service.name + L", " +
                            StartTypeNameForLog(service.startType) + L")";
        if (!service.imagePath.empty()) line += L"  " + service.imagePath;
        Out(line);
    }
    swprintf(buf, 256, L"%d services listed", (int)services.size());
    if (services.empty())
        Skip(L"third-party auto-start services enumerated",
             L"this machine starts no third-party service with Windows");
    else
        Check(true, L"third-party auto-start services enumerated", buf);

    bool named = true;
    for (const auto& service : services)
        if (service.name.empty() || service.display.empty()) named = false;
    Check(named, L"every service entry has a name and a display name");

    // The classifier decides what may be offered, so prove it on real-world shapes.
    bool classified =
        !IsThirdPartyServicePath(L"C:\\Windows\\System32\\svchost.exe -k netsvcs") &&
        !IsThirdPartyServicePath(L"\"%SystemRoot%\\system32\\svchost.exe\" -k netsvcs") &&
        !IsThirdPartyServicePath(L"svchost.exe -k netsvcs") &&
        IsThirdPartyServicePath(L"\"C:\\Program Files\\Vendor\\agent.exe\" -service") &&
        IsThirdPartyServicePath(L"%ProgramFiles%\\Vendor\\agent.exe --run");
    Check(classified, L"service paths classified as Windows or third party");

    bool guardsHold = true;
    for (const auto& service : services) {
        if (service.canDeactivate && (!service.thirdParty || service.driver)) guardsHold = false;
        if (!service.deactivated && service.startType != SERVICE_AUTO_START) guardsHold = false;
        if (service.deactivated && service.originalStart == SERVICE_DISABLED) guardsHold = false;
    }
    Check(guardsHold, L"no Windows component or driver offered for deactivation");

    bool noSecurity = true;
    for (const auto& service : services) {
        std::wstring haystack = service.name + L" " + service.display + L" " + service.imagePath;
        std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                       [](wchar_t c) { return (wchar_t)towlower(c); });
        for (const wchar_t* word : {L"defender", L"mcafee", L"antivirus", L"kaspersky"})
            if (haystack.find(word) != std::wstring::npos) noSecurity = false;
    }
    Check(noSecurity, L"security software is never offered for deactivation");

    // A name that cannot exist: proves the failure path is clean without ever
    // touching a real service's configuration.
    ServiceEntry missing;
    missing.name = L"ZanpharSelfTestNoSuchService";
    missing.display = L"Zanphar self-test";
    missing.canDeactivate = true;
    ServiceChange refused = DeactivateService(missing);
    Check(!refused.ok && !refused.error.empty(),
          L"an unknown service is refused without changing anything", refused.error);

    detail.clear();
    bool regOk = RegistryRoundTrip(detail);
    Check(regOk, L"disable/enable preserves a registry run value", detail);

    detail.clear();
    bool prefOk = ElevationPreferenceTest(detail);
    Check(prefOk, L"elevation prompt preference round-trips", detail);

    // ------------------------------------------------------ Services engine
    Out(L"");
    Out(L"Services page engine:");
    std::vector<ServiceRow> allServices = LoadAllServices();
    bool everyServiceNamed = !allServices.empty();
    int windowsRows = 0, thirdPartyRows = 0, measuredRows = 0;
    bool classificationAgrees = true;
    for (const auto& row : allServices) {
        if (row.name.empty() || row.display.empty()) everyServiceNamed = false;
        if (row.imagePath.empty()) continue;
        ++measuredRows;
        bool third = IsThirdPartyServicePath(row.imagePath);
        if (row.windows == third) classificationAgrees = false;
        if (row.windows)
            ++windowsRows;
        else
            ++thirdPartyRows;
    }
    swprintf(buf, 256, L"%d listed, %d Windows, %d third party, %d with a binary",
             (int)allServices.size(), windowsRows, thirdPartyRows, measuredRows);
    Check(everyServiceNamed && measuredRows > 0,
          L"every installed service is listed with a display name", buf);
    Check(classificationAgrees && windowsRows > 0 && thirdPartyRows > 0,
          L"services are split into Windows and third-party binaries", buf);

    ServiceOp missingOp;
    bool missingRefused = !SetServiceStart(L"ZanpharSelfTestNoSuchService", SERVICE_DISABLED,
                                           false, missingOp) &&
                          !missingOp.error.empty();
    Check(missingRefused, L"changing the start type of a missing service fails cleanly",
          missingOp.error);
    Check(ServiceStartTypeLabel(SERVICE_AUTO_START) == L"Automatic" &&
              ServiceStartTypeLabel(SERVICE_DEMAND_START) == L"Manual" &&
              ServiceStartTypeLabel(SERVICE_DISABLED) == L"Disabled" &&
              ServiceStateLabel(SERVICE_RUNNING) == L"Running",
          L"start types and states are labelled for the row");

    // Both pages narrow their list with the same case-insensitive matcher, so prove it on
    // the names this PC actually reports rather than on made-up strings.
    {
        std::vector<int> everything = FilterServiceRows(allServices, L"");
        std::vector<int> noService = FilterServiceRows(allServices, L"zz-no-such-service");
        std::wstring needle = allServices.empty() ? std::wstring() : allServices.front().display;
        if (needle.size() > 4) needle.resize(4);
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](wchar_t c) { return (wchar_t)towlower(c); });
        std::vector<int> serviceMatches = FilterServiceRows(allServices, needle);

        std::vector<ProcRow> processRows = LoadProcessRows();
        std::wstring procNeedle;
        for (const auto& row : processRows) {
            if (row.pid > 4 && !row.name.empty()) {
                procNeedle = row.name.substr(0, std::min<size_t>(3, row.name.size()));
                break;
            }
        }
        std::transform(procNeedle.begin(), procNeedle.end(), procNeedle.begin(),
                       [](wchar_t c) { return (wchar_t)towlower(c); });
        std::vector<int> processMatches = FilterTaskRows(processRows, procNeedle);
        std::vector<int> noProcess = FilterTaskRows(processRows, L"zz-no-such-process");

        if (allServices.empty() || processRows.empty() || procNeedle.empty()) {
            Skip(L"the list search boxes narrow by name, case-insensitively",
                 L"this PC returned no services or processes to search");
        } else {
            swprintf(buf, 256,
                     L"%d/%d services for \"%ls\", %d/%d processes for \"%ls\"",
                     (int)serviceMatches.size(), (int)allServices.size(), needle.c_str(),
                     (int)processMatches.size(), (int)processRows.size(), procNeedle.c_str());
            bool filtersWork = everything.size() == allServices.size() && noService.empty() &&
                               !serviceMatches.empty() && !processMatches.empty() &&
                               noProcess.empty();
            Check(filtersWork, L"the list search boxes narrow by name, case-insensitively", buf);
        }
    }

    // The Services and Task Manager lists fit whole rows: a 400 px viewport of 62 px rows
    // with an 8 px gap shows five rows and ends on the fifth row's edge. Every wheel
    // offset rounds to a row, and a list shorter than the viewport cannot scroll at all.
    WholeRowList rowLayout = FitWholeRows(100, 500, 62, 8, 40, 137);
    const int rowBottom = rowLayout.bottom;
    const int rowScroll = rowLayout.scroll;
    const int rowMax = rowLayout.maxScroll;
    rowLayout = FitWholeRows(100, 500, 62, 8, 2, 137);
    const bool wholeRowsOk = rowBottom == 442 && rowScroll % 70 == 0 && rowMax % 70 == 0 &&
                             rowScroll <= rowMax && rowLayout.scroll == 0 &&
                             rowLayout.maxScroll == 0;
    swprintf(buf, 256, L"bottom %d, scroll %d of %d; a short list stays at 0", rowBottom,
             rowScroll, rowMax);
    Check(wholeRowsOk, L"the service and task lists fit whole rows", buf);

    // The bulk buttons act on ticks that are held by name and by pid, not by row position,
    // so a refresh that reorders either list cannot move a tick onto another row.
    {
        AppState ticks;
        std::vector<ProcRow> live = LoadProcessRows();
        DWORD tickPid = 0, otherPid = 0;
        for (const auto& row : live) {
            if (row.pid <= 4) continue;
            if (tickPid == 0)
                tickPid = row.pid;
            else if (row.pid != tickPid) {
                otherPid = row.pid;
                break;
            }
        }
        std::wstring tickName;
        size_t spare = allServices.size();
        if (!allServices.empty()) {
            tickName = allServices.back().name;
            ticks.serviceChecked.push_back(tickName);
            for (size_t i = 0; i < allServices.size(); ++i)
                if (allServices[i].name != tickName) {
                    spare = i;
                    break;
                }
        }
        if (tickPid) ticks.taskChecked.push_back({tickPid, 0});

        if (tickName.empty() || tickPid == 0) {
            Skip(L"bulk ticks follow services and processes by identity",
                 L"this PC returned no services or processes to tick");
        } else {
            bool ticksWork =
                IsServiceChecked(ticks, tickName) && IsTaskChecked(ticks, tickPid, 0);
            if (spare < allServices.size())
                ticksWork = ticksWork && !IsServiceChecked(ticks, allServices[spare].name);
            if (otherPid != 0) ticksWork = ticksWork && !IsTaskChecked(ticks, otherPid, 0);
            swprintf(buf, 256, L"service \"%ls\", pid %lu, every other row unticked",
                     tickName.c_str(), (unsigned long)tickPid);
            Check(ticksWork, L"bulk ticks follow services and processes by identity", buf);
        }
    }

    // A tick is not the pid alone. Windows hands the number to a new process as soon as the
    // old one ends, and a tick that outlived its process would put that stranger in front
    // of End task and Force end.
    {
        AppState ticks;
        ticks.taskChecked.push_back({4242, 1000});
        const bool sameProcess = IsTaskChecked(ticks, 4242, 1000);
        const bool reusedPid = !IsTaskChecked(ticks, 4242, 2000);
        Check(sameProcess && reusedPid,
              L"a task tick follows one process instance, not a reused pid",
              reusedPid ? L"pid 4242 ticked for creation 1000, not for 2000"
                        : L"the tick moved onto a process that only shares the pid");
    }

    // Service names are unique without regard to case, so a tick has to read back whatever
    // capitals a later load reports.
    {
        AppState ticks;
        ticks.serviceChecked.push_back(L"Spooler");
        const bool caseFree = IsServiceChecked(ticks, L"spooler") &&
                              IsServiceChecked(ticks, L"SPOOLER") &&
                              !IsServiceChecked(ticks, L"Spooler2");
        Check(caseFree, L"a service tick ignores the case of the name",
              caseFree ? L"Spooler ticked; spooler and SPOOLER read back ticked"
                       : L"a different capital lost the tick");
    }

    // ---------------------------------------------------- Task Manager engine
    Out(L"");
    Out(L"Task Manager engine:");
    STARTUPINFOW childStartup = {};
    childStartup.cb = sizeof(childStartup);
    PROCESS_INFORMATION child = {};
    std::wstring childLine = L"cmd.exe /c ping -n 120 127.0.0.1";
    bool spawned = CreateProcessW(nullptr, childLine.data(), nullptr, nullptr, FALSE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &childStartup,
                                  &child) != FALSE;
    bool affinitySet = false, prioritySet = false, childListed = false, childEnded = false;
    std::wstring taskDetail;
    if (!spawned) {
        taskDetail = L"could not start a scratch process: " + WinErrorText(GetLastError());
    } else {
        CloseHandle(child.hThread);
        Sleep(250);
        std::wstring error;
        bool affinityOk = SetTaskAffinity(child.dwProcessId, 1, error);
        DWORD_PTR current = 0, systemMask = 0;
        affinitySet = affinityOk &&
                      GetProcessAffinityMask(child.hProcess, &current, &systemMask) &&
                      current == 1;
        bool priorityOk = SetTaskPriority(child.dwProcessId, IDLE_PRIORITY_CLASS, error);
        prioritySet = priorityOk && GetPriorityClass(child.hProcess) == IDLE_PRIORITY_CLASS;

        std::vector<ProcRow> live = LoadProcessRows();
        for (const auto& row : live) {
            if (row.pid != child.dwProcessId) continue;
            childListed = row.affinity == 1 && row.priority == IDLE_PRIORITY_CLASS;
            break;
        }

        std::wstring endError;
        childEnded = EndRunningTask(child.dwProcessId, true, endError) &&
                     WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0;
        CloseHandle(child.hProcess);
        if (!affinityOk || !priorityOk) taskDetail = error;
        else if (!childEnded) taskDetail = endError;
    }
    Check(affinitySet && prioritySet && childListed && childEnded,
          L"a running task takes new cores and priority, then force-ends",
          taskDetail.empty()
              ? std::wstring(L"cores: ") + (affinitySet ? L"set" : L"no") +
                    L", priority: " + (prioritySet ? L"set" : L"no") +
                    L", listed with new values: " + (childListed ? L"yes" : L"no") +
                    L", ended: " + (childEnded ? L"yes" : L"no")
              : taskDetail);
    Check(AffinityText(0) == L"-" && AffinityText(5) == L"CPU 0,2" &&
              PriorityLabel(BELOW_NORMAL_PRIORITY_CLASS) == L"Below normal",
          L"affinity and priority read back in words", AffinityText(5));

    // Rules live in the settings file, and the Run entry makes the agent apply them again
    // at every sign-in. Both are exercised and then put back the way they were.
    if (GetTempPathW(MAX_PATH, viewDir) > 0) {
        std::wstring rulesIni = std::wstring(viewDir) + L"ZanpharsWinSuite-rules-test.ini";
        DeleteFileW(rulesIni.c_str());
        TaskRule rule;
        rule.exe = L"zalphtest.exe";
        rule.affinity = 3;
        rule.priority = BELOW_NORMAL_PRIORITY_CLASS;
        bool saved = SaveTaskRules(rulesIni, {rule});
        std::vector<TaskRule> reloaded = LoadTaskRules(rulesIni);
        bool roundTrip = saved && reloaded.size() == 1 &&
                         reloaded[0].exe == L"zalphtest.exe" && reloaded[0].affinity == 3 &&
                         reloaded[0].priority == BELOW_NORMAL_PRIORITY_CLASS;
        DeleteFileW(rulesIni.c_str());
        Check(roundTrip, L"saved task rules round-trip through the settings file",
              roundTrip ? L"affinity 3 and below-normal came back"
                        : L"the rule did not come back as it was written");
    } else {
        Skip(L"saved task rules round-trip through the settings file",
             L"this machine has no temp folder to write to");
    }

    std::wstring agentPath = SuiteAgentPath();
    if (!agentPath.empty() && GetFileAttributesW(agentPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        bool agentWasThere = TaskAgentRegistered();
        std::wstring agentError;
        bool agentSet = RegisterTaskAgent(true, agentError);
        bool agentSeen = TaskAgentRegistered();
        bool agentCleared = RegisterTaskAgent(false, agentError);
        bool agentGone = !TaskAgentRegistered();
        if (agentWasThere) RegisterTaskAgent(true, agentError);
        Check(agentSet && agentSeen && agentCleared && agentGone,
              L"task agent is registered for sign-in and removed again",
              agentSet ? std::wstring(L"Run entry written and removed, previous state "
                                      L"restored")
                       : agentError);
    } else {
        Skip(L"task agent is registered for sign-in and removed again",
             L"WinSuiteAgent.exe is not next to the suite");
    }

    // --------------------------------------- integrated engines (read-only)
    // Nothing here changes a service, an adapter or a browser: the pages' read paths
    // have to work on any PC without touching it.
    Out(L"");
    Out(L"Integrated engines:");
    wchar_t engineBuf[256];

    std::vector<BrowserEntry> catalog = BuildBrowserCatalog();
    int installedBrowsers = 0;
    std::wstring browserNames;
    for (const auto& entry : catalog) {
        if (!entry.installed) continue;
        ++installedBrowsers;
        if (!browserNames.empty()) browserNames += L", ";
        browserNames += entry.name;
    }
    swprintf(engineBuf, 256, L"%d catalogued, %d installed", (int)catalog.size(),
             installedBrowsers);
    Check(catalog.size() >= 8, L"browser tracers catalogued", engineBuf);
    Out(L"Browsers found: " + (browserNames.empty() ? std::wstring(L"none") : browserNames));

    if (installedBrowsers > 0) {
        BrowserEntry probe;
        for (const auto& entry : catalog)
            if (entry.installed) {
                probe = entry;
                break;
            }
        ScanBrowserEntry(probe);
        swprintf(engineBuf, 256, L"%ls in %lu file(s) for %ls", FormatBytes(probe.bytes).c_str(),
                 (unsigned long)probe.files, probe.name.c_str());
        Check(!probe.traces.empty(), L"browser trace measurement runs", engineBuf);
    }

    std::vector<DnsAdapter> adapters = LoadDnsAdapters();
    swprintf(engineBuf, 256, L"%d adapter(s), first: %ls", (int)adapters.size(),
             adapters.empty() ? L"none" : adapters.front().name.c_str());
    if (adapters.empty())
        Skip(L"DNS adapters enumerated", L"this machine has no configured adapter");
    else
        Check(true, L"DNS adapters enumerated", engineBuf);

    BoostState boostState = LoadBoostState();
    Check(!boostState.currentPlan.empty(), L"power plan readable", boostState.currentPlan);

    std::wstring dwordDetail;
    bool dwordOk = BoostDwordRoundTrip(dwordDetail);
    Check(dwordOk, L"boost Game DVR values round-trip as REG_DWORD", dwordDetail);

    std::wstring serviceDetail;
    bool serviceOk = BoostServiceJournalRoundTrip(serviceDetail);
    Check(serviceOk, L"boost service records keep the start type, the state and the delay",
          serviceDetail);

    // The network catalog is read-only here: no registry value is changed, so the check
    // means the same thing on a build machine with no adapter as on a gaming desktop.
    std::vector<NetTweakView> tweaks = LoadNetTweaks();
    bool catalogOk = tweaks.size() == 4;
    int tweakAvailable = 0;
    for (const auto& tweak : tweaks) {
        if (tweak.title.empty() || tweak.summary.empty() || tweak.writes.empty() ||
            tweak.state.empty())
            catalogOk = false;
        if (tweak.applied && !tweak.available) catalogOk = false;
        if (tweak.id == L"nagle" && tweak.targetCount % 2 != 0) catalogOk = false;
        if (tweak.available) ++tweakAvailable;
    }
    swprintf(engineBuf, 256, L"%d tweaks, %d available on this PC", (int)tweaks.size(),
             tweakAvailable);
    Check(catalogOk, L"network tweak catalog intact", engineBuf);

    std::wstring netDetail;
    bool netJournalOk = NetJournalRoundTrip(netDetail);
    Check(netJournalOk, L"network journal saves and restores a value", netDetail);

    // The page's latency check, measured over loopback so the check never depends on the
    // internet or on an adapter being configured. A build machine where ICMP is refused
    // outright is skipped rather than failed, the way a missing adapter is above.
    LatencyResult loopback = MeasureLatency(L"127.0.0.1", 0);  // 0 must clamp to one echo
    if (loopback.ok) {
        swprintf(engineBuf, 256, L"%ls -> %ls, %d/%d replies, %d ms avg (%d-%d)",
                 loopback.host.c_str(), loopback.address.c_str(), loopback.received,
                 loopback.sent, loopback.avgMs, loopback.minMs, loopback.maxMs);
        Check(loopback.sent == 1 && loopback.received == 1 && loopback.host == L"127.0.0.1" &&
                  loopback.address == L"127.0.0.1" && loopback.avgMs >= loopback.minMs &&
                  loopback.avgMs <= loopback.maxMs && loopback.error.empty(),
              L"latency check pings a host and clamps the sample count", engineBuf);
    } else {
        Skip(L"latency check pings a host and clamps the sample count", loopback.error);
    }

    // The comparison line is pure text, so it is checked without a network: the pair reads
    // as one span with the difference beside it, and packet loss is mentioned only when it
    // moved. A missing reading produces no line at all.
    LatencyResult sampleBefore, sampleAfter;
    sampleBefore.ok = true;
    sampleBefore.avgMs = 11;
    sampleBefore.sent = 4;
    sampleBefore.received = 4;
    sampleAfter = sampleBefore;
    sampleAfter.avgMs = 14;
    const std::wstring comparison = LatencyComparison(sampleBefore, sampleAfter);
    bool comparisonOk = comparison == L"11 ms to 14 ms - 3 ms higher on average";
    sampleAfter.avgMs = 8;
    comparisonOk = comparisonOk && LatencyComparison(sampleBefore, sampleAfter) ==
                                       L"11 ms to 8 ms - 3 ms lower on average";
    sampleAfter.avgMs = 11;
    comparisonOk = comparisonOk && LatencyComparison(sampleBefore, sampleAfter) ==
                                       L"11 ms to 11 ms - no change in the average";
    sampleAfter.avgMs = 14;
    sampleAfter.received = 3;
    comparisonOk = comparisonOk &&
                   LatencyComparison(sampleBefore, sampleAfter) ==
                       L"11 ms to 14 ms - 3 ms higher on average - but more packet loss";
    comparisonOk = comparisonOk && LatencyComparison(LatencyResult(), sampleAfter).empty();
    Check(comparisonOk, L"latency comparison reads the pair as one span", comparison);

    // Game Booster: the profile must stay limited to the four safe rows, and the advanced
    // three must never be swept into a one-click apply.
    std::vector<GameTweakView> gameTweaks = LoadGameTweaks();
    bool gameCatalogOk = gameTweaks.size() == 7;
    int gameInProfile = 0, gameAdvanced = 0, gameAvailable = 0;
    for (const auto& tweak : gameTweaks) {
        if (tweak.title.empty() || tweak.summary.empty() || tweak.how.empty() ||
            tweak.valueCount <= 0)
            gameCatalogOk = false;
        if (tweak.applied && !tweak.available) gameCatalogOk = false;
        if (tweak.inProfile)
            ++gameInProfile;
        else
            ++gameAdvanced;
        if (tweak.available) ++gameAvailable;
    }
    if (gameInProfile != 4 || gameAdvanced != 3) gameCatalogOk = false;
    swprintf(engineBuf, 256, L"%d tweaks, %d in the profile, %d advanced, %d available",
             (int)gameTweaks.size(), gameInProfile, gameAdvanced, gameAvailable);
    Check(gameCatalogOk, L"Game Booster catalog keeps advanced tweaks out of the profile",
          engineBuf);

    // Game Booster's background services: the whole requested list is present, every row
    // carries its explanation and its live state, and none of them can ever be swept into
    // the one-click profile.
    std::vector<GameServiceView> gameServices = LoadGameServices();
    bool servicesOk = gameServices.size() == 28;
    int servicesInstalled = 0, servicesParked = 0;
    for (const auto& row : gameServices) {
        if (row.title.empty() || row.summary.empty() || row.state.empty()) servicesOk = false;
        if (row.applied && row.active > 0) servicesOk = false;
        if (row.applied && row.installed == 0) servicesOk = false;
        if (row.installed > 0) ++servicesInstalled;
        if (row.applied) ++servicesParked;
    }
    // Windows always ships most of this list, so a read that finds none of them means the
    // service manager could not be read at all.
    if (servicesInstalled == 0) servicesOk = false;
    swprintf(engineBuf, 256, L"%d service rows, %d with services on this PC, %d already off",
             (int)gameServices.size(), servicesInstalled, servicesParked);
    Check(servicesOk, L"Game Booster lists the background services with their live state",
          engineBuf);

    std::wstring gameServiceDetail;
    bool serviceJournalOk = GameServiceJournalRoundTrip(gameServiceDetail);
    Check(serviceJournalOk, L"Game Booster saves and reads back a service's original state",
          gameServiceDetail);

    std::wstring gameDetail;
    bool gameJournalOk = GameJournalRoundTrip(gameDetail);
    Check(gameJournalOk, L"Game Booster journal restores a value and its type", gameDetail);

    MemoryPrefs prefs = LoadMemoryPrefs();
    // Flip both dialog choices so the round-trip proves they are stored, then put the
    // user's own choices back.
    MemoryPrefs probe = prefs;
    probe.confirmRegular = !prefs.confirmRegular;
    probe.confirmDeep = !prefs.confirmDeep;
    SaveMemoryPrefs(probe);
    MemoryPrefs reloaded = LoadMemoryPrefs();
    SaveMemoryPrefs(prefs);
    swprintf(engineBuf, 256,
             L"auto=%d threshold=%d idle=%d clipboard=%d dialogs=%d/%d",
             reloaded.autoClean ? 1 : 0, reloaded.threshold, reloaded.idleClean ? 1 : 0,
             reloaded.clearClipboard ? 1 : 0, reloaded.confirmRegular ? 1 : 0,
             reloaded.confirmDeep ? 1 : 0);
    Check(prefs.threshold == reloaded.threshold && prefs.autoClean == reloaded.autoClean &&
              reloaded.confirmRegular == probe.confirmRegular &&
              reloaded.confirmDeep == probe.confirmDeep,
          L"memory options and clean dialogs round-trip", engineBuf);

    Out(L"");
    // Every count on one line, so a CI log can be grepped for the outcome
    // instead of counting [PASS] lines by eye.
    swprintf(buf, 256, L"RESULT: %d passed, %d skipped, %d failed.", g_passes, g_skipped,
             g_failures);
    Out(buf);
    return g_failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------------- shots
static bool SaveBmp24(const std::wstring& path, int w, int h, const void* bgraTopDown) {
    int rowSize = (w * 3 + 3) & ~3;
    DWORD imageSize = (DWORD)(rowSize * h);
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + imageSize;

    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;  // bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = imageSize;

    std::vector<BYTE> pixels((size_t)rowSize * h, 0);
    const BYTE* src = (const BYTE*)bgraTopDown;
    for (int y = 0; y < h; ++y) {
        const BYTE* srcRow = src + (size_t)y * w * 4;
        BYTE* dstRow = pixels.data() + (size_t)(h - 1 - y) * rowSize;
        for (int x = 0; x < w; ++x) {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 0];
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
            dstRow[x * 3 + 2] = srcRow[x * 4 + 2];
        }
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, &fh, sizeof(fh), &written, nullptr) &&
              WriteFile(file, &ih, sizeof(ih), &written, nullptr) &&
              WriteFile(file, pixels.data(), imageSize, &written, nullptr);
    CloseHandle(file);
    return ok;
}

static bool RenderShot(const std::wstring& path, int w, int h, AppState& st) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    std::vector<Hit> hits;
    PaintApp(mem, w, h, st, hits);
    SelectObject(mem, old);
    bool ok = SaveBmp24(path, w, h, bits);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

// Test-only DPI/media matrix for the Storage page. It renders the real paint path at
// each supported scale with mechanical, solid-state, removable and unknown volumes,
// plus one busy frame that proves the Cancel control is present.
static int RunStorageShots(const std::wstring& dir) {
    AppState st;
    st.elevated = IsProcessElevated();
    st.promptSuppressed = !st.elevated && PrefersStandardMode();
    auto add = [&](wchar_t letter, const wchar_t* type, bool ssd, bool known) {
        StorageDevice d;
        d.root = std::wstring(1, letter) + L":\\";
        d.label = std::wstring(L"TEST-") + (ssd ? L"SSD" : L"HDD");
        d.typeName = type;
        d.fileSystem = L"NTFS";
        d.bus = ssd ? L"NVMe" : L"SATA";
        d.model = L"matrix test volume";
        d.total = 100ull << 30;
        d.freeBytes = 40ull << 30;
        d.accessible = true;
        d.mediaKnown = known;
        d.ssd = ssd;
        st.storage.push_back(d);
    };
    add(L'C', L"Fixed", false, true);
    add(L'D', L"Fixed", true, true);
    add(L'E', L"Removable", false, true);
    add(L'F', L"Fixed", false, false);
    add(L'G', L"Fixed", true, true);
    add(L'H', L"Fixed", false, true);
    add(L'I', L"Removable", true, true);
    add(L'J', L"Fixed", false, true);
    st.page = Page::Storage;
    st.storageMessage = L"Test matrix: HDD, SSD, removable and unknown media.";

    bool ok = true;
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        g_scale = scale;
        g_fonts.Create(scale);
        const int w = (int)std::lround(1024.0 * scale);
        const int h = (int)std::lround(768.0 * scale);
        wchar_t name[64];
        swprintf(name, 64, L"storage-matrix-%.0f.bmp", scale * 100.0f);
        const std::wstring path = dir + L"\\" + name;
        bool rendered = RenderShot(path, w, h, st);
        Out(std::wstring(rendered ? L"wrote " : L"failed ") + path);
        ok = ok && rendered;

        st.storageBusy = true;
        st.storagePercent = 42;
        st.storageAction = L"Defragmenting C:";
        st.storageMessage = L"Defragmenting C: - 00:42 elapsed - 42%";
        swprintf(name, 64, L"storage-busy-%.0f.bmp", scale * 100.0f);
        const std::wstring busyPath = dir + L"\\" + name;
        rendered = RenderShot(busyPath, w, h, st);
        Out(std::wstring(rendered ? L"wrote " : L"failed ") + busyPath);
        ok = ok && rendered;
        st.storageBusy = false;
        st.storagePercent = -1;
        st.storageAction.clear();
        st.storageMessage = L"Test matrix: HDD, SSD, removable and unknown media.";
    }
    return ok ? 0 : 1;
}

static int RunShots(const std::wstring& dir) {
    g_scale = 1.0f;
    g_fonts.Create(g_scale);

    AppState st;
    st.elevated = IsProcessElevated();
    st.promptSuppressed = !st.elevated && PrefersStandardMode();
    st.sys = CollectSystemInfo();
    st.storage = CollectStorageDevices();
    st.cpu.Update();
    Sleep(150);
    st.cpu.Update();
    st.groups = BuildCleanGroups();
    for (auto& g : st.groups) ScanCleanGroup(g);
    RefreshCleanupOrder(st);
    st.hasScanned = true;
    st.startup = LoadStartupItems();
    st.services = LoadServices();
    st.serviceRows = LoadAllServices();
    st.servicesLoaded = true;
    if (!st.serviceRows.empty()) st.serviceSelected = 0;
    st.servicesMessage =
        L"Listing every installed Windows service with its start type and state.";
    st.taskRows = LoadProcessRows();
    st.tasksLoaded = true;
    // The shot picks a real, openable process rather than the first row, so the card above
    // the list shows its chips populated instead of "select a process".
    for (size_t i = 0; i < st.taskRows.size(); ++i) {
        if (!st.taskRows[i].accessible || st.taskRows[i].pid <= 4) continue;
        st.taskSelected = (int)i;
        st.taskCpuMask = st.taskRows[i].affinity;
        st.taskPriority = st.taskRows[i].priority;
        break;
    }
    st.tasksMessage = L"Select a process to set its cores, its priority or a saved rule.";

    st.mem = CollectMemStats();
    st.memProcs = TopMemoryProcesses(12);

    st.browsers = BuildBrowserCatalog();
    for (auto& browser : st.browsers)
        if (browser.installed) ScanBrowserEntry(browser);
    st.browserScanned = true;
    st.browserMessage = L"Measured on this PC.";
    st.dnsAdapters = LoadDnsAdapters();
    st.netTweaks = LoadNetTweaks();
    st.memPrefs = LoadMemoryPrefs();
    st.boost = LoadBoostState();
    st.gameTweaks = LoadGameTweaks();
    st.gameServices = LoadGameServices();
    LoadDnsSettings(st);

    st.largeRoots = LargeFileRoots();
    LargeFileScan largeOptions;
    largeOptions.minBytes = 100ull << 20;
    largeOptions.maxResults = 10;
    ScanLargeFiles(st.largeRoots, largeOptions, st.largeFiles);
    st.largeScanned = true;
    for (size_t i = 0; i < st.largeFiles.size() && i < 2; ++i) st.largeFiles[i].selected = true;

    struct Shot {
        Page page;
        const wchar_t* name;
    };
    const Shot shots[] = {
        {Page::Dashboard, L"dashboard.bmp"},
        {Page::Cleanup, L"cleanup.bmp"},
        {Page::Browsers, L"browsers.bmp"},
        {Page::Memory, L"memory.bmp"},
        {Page::Dns, L"dns.bmp"},
        {Page::Network, L"network.bmp"},
        {Page::Boost, L"gamebooster.bmp"},
        {Page::LargeFiles, L"largefiles.bmp"},
        {Page::Startup, L"startup.bmp"},
        {Page::Deactivate, L"deactivate.bmp"},
        {Page::Services, L"services.bmp"},
        {Page::Tasks, L"tasks.bmp"},
        {Page::System, L"system.bmp"},
        {Page::Storage, L"storage.bmp"},
        {Page::Settings, L"settings.bmp"},
    };
    bool ok = true;
    for (const auto& shot : shots) {
        st.page = shot.page;
        st.hoverId = HitNone;
        std::wstring path = dir + L"\\" + shot.name;
        bool rendered = RenderShot(path, 1024, 768, st);
        Out(std::wstring(rendered ? L"wrote " : L"failed ") + path);
        ok = ok && rendered;
    }

    // The read-only agreement on its own, so its layout can be checked without opening
    // the app.
    st.showLicense = true;
    st.licenseScroll = 0;
    std::wstring licencePath = dir + L"\\licence.bmp";
    bool licenceRendered = RenderShot(licencePath, 1024, 768, st);
    Out(std::wstring(licenceRendered ? L"wrote " : L"failed ") + licencePath);
    ok = ok && licenceRendered;
    return ok ? 0 : 1;
}

// --------------------------------------------------------- single instance
// The suite owns one window, one tray icon and one set of background workers, so a second
// launch replaces the running copy instead of opening beside it. The old copy is asked to
// quit; one that does not answer (an older build, or one that is busy) is terminated, so
// the window the user just asked for is always the one that stays.
static void AskCopyToQuit(DWORD pid) {
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner != (DWORD)param) return TRUE;
            PostMessageW(window, g_quitMessage, 0, 0);
            // Older builds do not know the quit message; a close is the next best request.
            // Both only ask - the process is terminated below if it ignores them.
            PostMessageW(window, WM_CLOSE, 0, 0);
            return TRUE;
        },
        (LPARAM)pid);
}

static bool ClosePreviousCopies() {
    wchar_t path[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH * 2)) return true;
    std::wstring exe = path;
    size_t slash = exe.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exe = exe.substr(slash + 1);
    if (exe.empty()) return true;

    std::vector<DWORD> others;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID == GetCurrentProcessId()) continue;
                if (_wcsicmp(entry.szExeFile, exe.c_str()) != 0) continue;
                others.push_back(entry.th32ProcessID);
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    if (others.empty()) return true;

    std::vector<HANDLE> waiting;
    bool closedAll = true;
    for (DWORD pid : others) {
        AskCopyToQuit(pid);
        HANDLE process = OpenProcess(
            SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process) {
            waiting.push_back(process);
            continue;
        }
        // A copy that has already gone is fine. One we may not even open - typically an
        // elevated window when this launch is running standard - cannot be replaced.
        if (GetLastError() == ERROR_ACCESS_DENIED) closedAll = false;
    }

    for (HANDLE process : waiting) {
        if (WaitForSingleObject(process, 1500) == WAIT_OBJECT_0) continue;
        TerminateProcess(process, 0);
        if (WaitForSingleObject(process, 3000) != WAIT_OBJECT_0) closedAll = false;
    }
    for (HANDLE process : waiting) CloseHandle(process);
    return closedAll;
}

// -------------------------------------------------------------------- entry
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    typedef BOOL(WINAPI * SetDpiContextFn)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto setContext =
            (SetDpiContextFn)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setContext) setContext((HANDLE)-4);  // PER_MONITOR_AWARE_V2
    }

    UiStartup();
    bool selftest = false;
    bool servicetest = false;
    bool servicetestNoop = false;
    bool noElevate = false;
    std::wstring shotDir;
    std::wstring storageShotDir;
    std::vector<std::wstring> forwarded;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--selftest") selftest = true;
        if (arg == L"--servicetest") servicetest = true;
        if (arg == L"--servicetest-noop") servicetestNoop = true;
        if (arg == L"--no-elevate") noElevate = true;
        if (arg == L"--shot" && i + 1 < argc) {
            shotDir = argv[++i];
            forwarded.push_back(arg);
            forwarded.push_back(shotDir);
            continue;
        }
        if (arg == L"--storage-shots" && i + 1 < argc) {
            storageShotDir = argv[++i];
            continue;
        }
        if (arg == L"--selftest-log" && i + 1 < argc) {
            g_logPath = argv[++i];
            continue;
        }
        forwarded.push_back(arg);
    }
    if (argv) LocalFree(argv);

    // Started by the service manager during --servicetest: exit before any window,
    // elevation or self-test work can happen.
    if (servicetestNoop) {
        UiShutdown();
        return 0;
    }

    bool elevated = IsProcessElevated();
    bool guiMode = !selftest && !servicetest && shotDir.empty() && storageShotDir.empty();
    bool promptSuppressed = !elevated && PrefersStandardMode();
    if (guiMode && !elevated && !noElevate && !promptSuppressed) {
        DWORD launchError = 0;
        if (RelaunchElevated(forwarded, &launchError)) {
            UiShutdown();
            return 0;  // the elevated copy owns the window from here
        }
        // UAC declined: remember the choice so the next launch goes straight to the app.
        // Any other failure (elevation unavailable) is not a user decision, so it is
        // not remembered.
        if (launchError == ERROR_CANCELLED) {
            SetStandardModePreference(true);
            promptSuppressed = true;
        }
        // Either way, carry on with the rights we have.
    }

    // Only one window of the suite may exist. The copy the user just started closes the
    // one already running and carries on, so stale rules, ticks and builds cannot be
    // mistaken for this session's. Registering the quit message first means the copy that
    // is running now can be closed cleanly by the next launch.
    if (guiMode) g_quitMessage = RegisterWindowMessageW(L"ZanpharsWinSuiteQuit");
    if (guiMode && !ClosePreviousCopies()) {
        MessageBoxW(nullptr,
                    L"Another copy of Zanphar's WinSuite is running and could not be "
                    L"closed. Close it, then start this one again.",
                    L"Zanphar's WinSuite", MB_OK | MB_ICONWARNING);
        UiShutdown();
        return 0;
    }

    int result = 0;
    if (servicetest) {
        result = RunServiceTest();
        UiShutdown();
        return result;
    }
    if (selftest) {
        InitOutput();
        result = RunSelfTest();
        if (g_logFile) fclose(g_logFile);
        UiShutdown();
        return result;
    }
    if (!shotDir.empty()) {
        InitOutput();
        result = RunShots(shotDir);
        UiShutdown();
        return result;
    }
    if (!storageShotDir.empty()) {
        InitOutput();
        result = RunStorageShots(storageShotDir);
        UiShutdown();
        return result;
    }

    AppState app;
    app.elevated = elevated;
    app.promptSuppressed = promptSuppressed;
    g_app = &app;
    LoadSystemPage(app);
    app.closeToTray = LoadWindowPref(SettingsPath(), L"CloseToTray");
    app.minimizeToTray = LoadWindowPref(SettingsPath(), L"MinimizeToTray");
    app.startInTray = LoadWindowPref(SettingsPath(), L"StartInTray");
    app.toastNotifications = LoadToastPref(SettingsPath());
    app.groups = BuildCleanGroups();
    LoadCleanSelection(SettingsPath(), app.groups);  // ticks from the last session
    app.cleanSortBySize = LoadCleanupSortBySize(SettingsPath());  // order from last time
    RefreshCleanupOrder(app);  // nothing measured yet: rows take their remembered places
    app.startup = LoadStartupItems();
    app.memPrefs = LoadMemoryPrefs();
    app.boost = LoadBoostState();
    LoadDnsSettings(app);
    // The saved affinity/priority rules are applied to whatever is already running, and
    // the agent repeats this at every sign-in from the same settings file.
    app.taskRules = LoadTaskRules(SettingsPath());
    app.serviceRows = LoadAllServices();
    app.servicesLoaded = true;
    app.taskRows = LoadProcessRows();
    app.tasksLoaded = true;
    if (!app.taskRules.empty()) {
        ApplyTaskRules(app.taskRules);
        // A rule saved in an earlier session has to reach programs started after this
        // window opened too, which is the helper's job while it runs.
        EnsureTaskAgentRunning();
    }

    // Explorer broadcasts this when it restarts; the window procedure uses it to put the
    // tray icon back if the window was hidden there.
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    if (!CreateMainWindow(inst)) {
        UiShutdown();
        return 1;
    }
    SetTimer(g_hwnd, 1, 1000, nullptr);
    if (app.dnsWatchdog && app.dnsApplied && !app.dnsPrimary.empty()) StartDnsWatchdog();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_fonts.Destroy();
    UiShutdown();
    return 0;
}
