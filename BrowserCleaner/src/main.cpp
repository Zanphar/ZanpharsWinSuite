// ============================================================================
//  Browser Cleaner — Anti-Tracking
//  A from-scratch, dependency-free C++ / Win32 utility inspired by
//  IObit's "Browser Anti-Tracking" (BrowserCleaner.exe).
//
//  Removes browser tracking data from Internet Explorer (history, cookies,
//  cache and session recovery), Chromium-based browsers (Chrome, Edge, Brave,
//  Vivaldi, Opera, Opera GX) and Mozilla Firefox (history, cookies, autofill,
//  cached files and session restore).
//
//  Architecture
//    Storage engine : inspect/delete trace files (std::filesystem)
//    Process module : detect/close running browsers (Toolhelp32)
//    Catalog        : browser + trace-item definitions
//    UI             : GDI+ dark theme, Per-Monitor-v2 DPI aware
//
//  Build: build.bat (MinGW-w64 g++)
// ============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // Windows 10 (DPI-aware APIs)
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace G  = Gdiplus;
using G::Graphics;
using G::GraphicsPath;
using G::SolidBrush;
using G::Pen;
using G::Color;
using G::PointF;
using G::RectF;
using G::Font;
using G::FontFamily;

template <typename T, size_t N>
constexpr size_t CountOf(T (&)[N]) { return N; }

// ============================================================================
//  Theme — flat dark palette
// ============================================================================

namespace Theme {
const Color Window     (255, 17, 19, 23);      // app background
const Color Card       (255, 30, 33, 39);      // card surface
const Color CardBorder (255, 44, 48, 57);
const Color ChipBorder (255, 55, 60, 70);
const Color ChipHover  (255, 45, 50, 60);
const Color Text       (255, 235, 238, 242);   // primary text
const Color TextSub    (255, 152, 161, 172);   // secondary text
const Color TextDim    (255, 95, 103, 114);    // disabled text
const Color Accent     (255, 59, 130, 246);    // primary action
const Color AccentHover(255, 82, 146, 248);
const Color AccentPress(255, 43, 105, 205);
const Color Success    (255, 61, 213, 155);    // cleaning feedback
const Color White      (255, 255, 255, 255);
}  // namespace Theme

// ============================================================================
//  Storage engine — what a "trace" is and where it lives
// ============================================================================

enum class TraceKind { File, Folder };
enum class TraceRoot { Profile, LocalAppData, RoamingAppData };

struct TracePath {
    TraceKind      kind;
    const wchar_t* rel;   // relative to the root below
    TraceRoot      root = TraceRoot::Profile;  // profile, unless IE needs an AppData root
};

struct TraceItem {
    const wchar_t*         label;  // short UI label
    std::vector<TracePath> paths;
};

// File traces also remove SQLite sidecars (-journal/-wal/-shm): browsers keep
// tracking data inside these databases, so the whole database file has to go.
static const TraceItem kChromiumItems[] = {
    { L"History",
      { { TraceKind::File, L"History" } } },
    { L"Cookies",
      { { TraceKind::File, L"Cookies" } } },
    { L"Autofill",
      { { TraceKind::File, L"Web Data" } } },
    { L"Passwords",
      { { TraceKind::File, L"Login Data" } } },
    { L"Cache",
      { { TraceKind::Folder, L"Cache" },
        { TraceKind::Folder, L"Code Cache" },
        { TraceKind::Folder, L"GPUCache" },
        { TraceKind::Folder, L"Media Cache" },
        { TraceKind::Folder, L"GrShaderCache" },
        { TraceKind::Folder, L"ShaderCache" },
        { TraceKind::Folder, L"Crashpad" } } },
    { L"Session",
      { { TraceKind::Folder, L"Sessions" },
        { TraceKind::File, L"Current Session" },
        { TraceKind::File, L"Current Tabs" },
        { TraceKind::File, L"Last Session" },
        { TraceKind::File, L"Last Tabs" } } },
};

static const TraceItem kFirefoxItems[] = {
    { L"History",
      { { TraceKind::File, L"places.sqlite" } } },
    { L"Cookies",
      { { TraceKind::File, L"cookies.sqlite" } } },
    { L"Autofill",
      { { TraceKind::File, L"formhistory.sqlite" } } },
    { L"Passwords",
      { { TraceKind::File, L"logins.json" } } },
    { L"Cache",
      { { TraceKind::Folder, L"cache2" },
        { TraceKind::Folder, L"startupCache" },
        { TraceKind::Folder, L"thumbnails" } } },
    { L"Session",
      { { TraceKind::Folder, L"sessionstore-backups" },
        { TraceKind::File, L"sessionstore.js" } } },
};

// IE keeps its data outside a Chromium-style profile: WinINet owns the history,
// cache and cookies, and the recovery folder holds the last session. WebCache is
// the database modern Windows uses for IE's history, so it belongs with History.
static const TraceItem kInternetExplorerItems[] = {
    { L"History",
      { { TraceKind::Folder, L"Microsoft\\Windows\\History", TraceRoot::LocalAppData },
        { TraceKind::Folder, L"Microsoft\\Windows\\WebCache", TraceRoot::LocalAppData } } },
    { L"Cookies",
      { { TraceKind::Folder, L"Microsoft\\Windows\\INetCookies", TraceRoot::LocalAppData },
        { TraceKind::Folder, L"Microsoft\\Windows\\Cookies", TraceRoot::RoamingAppData } } },
    { L"Cache",
      { { TraceKind::Folder, L"Microsoft\\Windows\\INetCache", TraceRoot::LocalAppData } } },
    { L"Session",
      { { TraceKind::Folder, L"Microsoft\\Windows\\Internet Explorer\\Recovery",
          TraceRoot::LocalAppData } } },
};

// ============================================================================
//  Catalog — supported browsers
// ============================================================================

enum class ProfileMode {
    ChromiumShards,  // one subfolder per profile (Default, Profile 1, ...)
    OperaRoot,       // profile data lives directly in the base folder
    FirefoxShards,   // every subfolder of Profiles\ is a profile
    Fixed,           // one base folder, with traces resolved from their own roots
};

struct BrowserAccent { unsigned char r, g, b; };

struct BrowserDef {
    const wchar_t*   name;
    const wchar_t*   exes;        // ';' separated process image names
    ProfileMode      mode;
    bool             roaming;     // true -> %APPDATA%, false -> %LOCALAPPDATA%
    const wchar_t*   baseRel;     // relative to the AppData root
    BrowserAccent    accent;      // avatar dot color
    const wchar_t*   initials;    // avatar text
    std::vector<TraceItem> items;
};

static std::vector<BrowserDef> BuildCatalog() {
    std::vector<BrowserDef> list;
    list.reserve(8);

    auto add = [&list](BrowserDef def, const TraceItem* items, size_t count) {
        def.items.assign(items, items + count);
        list.push_back(std::move(def));
    };

    add({ L"Google Chrome",   L"chrome.exe",   ProfileMode::ChromiumShards, false,
          L"Google\\Chrome\\User Data",               {0x42,0x85,0xF4}, L"Ch", {} },
         kChromiumItems, CountOf(kChromiumItems));
    add({ L"Microsoft Edge",  L"msedge.exe",   ProfileMode::ChromiumShards, false,
          L"Microsoft\\Edge\\User Data",              {0x00,0x78,0xD4}, L"Ed", {} },
         kChromiumItems, CountOf(kChromiumItems));
    add({ L"Internet Explorer", L"iexplore.exe", ProfileMode::Fixed, false,
          L"Microsoft\\Windows",                       {0x00,0x74,0xE8}, L"IE", {} },
         kInternetExplorerItems, CountOf(kInternetExplorerItems));
    add({ L"Brave Browser",   L"brave.exe",    ProfileMode::ChromiumShards, false,
          L"BraveSoftware\\Brave-Browser\\User Data", {0xFB,0x54,0x2B}, L"Br", {} },
         kChromiumItems, CountOf(kChromiumItems));
    add({ L"Vivaldi",         L"vivaldi.exe",  ProfileMode::ChromiumShards, false,
          L"Vivaldi\\User Data",                      {0xEE,0x46,0x35}, L"Vi", {} },
         kChromiumItems, CountOf(kChromiumItems));
    add({ L"Opera",           L"opera.exe",    ProfileMode::OperaRoot,      true,
          L"Opera Software\\Opera Stable",            {0xFF,0x1B,0x2D}, L"Op", {} },
         kChromiumItems, CountOf(kChromiumItems));
    add({ L"Opera GX",        L"opera.exe",    ProfileMode::OperaRoot,      true,
          L"Opera Software\\Opera GX Stable",         {0xFF,0x71,0x27}, L"GX", {} },
         kChromiumItems, CountOf(kChromiumItems));
    add({ L"Mozilla Firefox", L"firefox.exe",  ProfileMode::FirefoxShards,  true,
          L"Mozilla\\Firefox\\Profiles",              {0xFF,0x8A,0x3C}, L"FF", {} },
         kFirefoxItems, CountOf(kFirefoxItems));
    return list;
}

// ============================================================================
//  Helpers
// ============================================================================

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return (wchar_t)::towlower(c); });
    return s;
}

static bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

static fs::path AppendRel(fs::path base, const wchar_t* rel) {
    std::wstring r(rel);
    size_t start = 0;
    while (start <= r.size()) {
        const size_t pos = r.find(L'\\', start);
        if (pos == std::wstring::npos) {
            base /= r.substr(start);
            break;
        }
        base /= r.substr(start, pos - start);
        start = pos + 1;
    }
    return base;
}

static fs::path AppDataBase(bool roaming) {
    wchar_t* raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(
        roaming ? FOLDERID_RoamingAppData : FOLDERID_LocalAppData, 0, nullptr, &raw);
    fs::path out;
    if (hr == S_OK && raw) out = fs::path(raw);
    if (raw) CoTaskMemFree(raw);
    return out;
}

// All profile folders in which this browser stores user data.
static std::vector<fs::path> ProfileDirs(const BrowserDef& def) {
    std::vector<fs::path> out;
    const fs::path base = AppendRel(AppDataBase(def.roaming), def.baseRel);
    if (!Exists(base)) return out;

    std::error_code ec;
    switch (def.mode) {
    case ProfileMode::ChromiumShards:  // folders containing a Preferences file
        for (fs::directory_entry e : fs::directory_iterator(base, ec)) {
            std::error_code ec2;
            if (!e.is_directory(ec2) || ec2) continue;
            if (Exists(e.path() / L"Preferences") ||
                Exists(e.path() / L"Secure Preferences"))
                out.push_back(e.path());
        }
        break;

    case ProfileMode::FirefoxShards:   // every subfolder is a profile
        for (fs::directory_entry e : fs::directory_iterator(base, ec)) {
            std::error_code ec2;
            if (!e.is_directory(ec2) || ec2) continue;
            out.push_back(e.path());
        }
        break;

    case ProfileMode::OperaRoot:
    case ProfileMode::Fixed:
        out.push_back(base);
        break;
    }
    return out;
}

static std::wstring FormatNumber(unsigned long long n) {
    wchar_t digits[32];
    const int len = swprintf(digits, 32, L"%llu", n);
    std::wstring out;
    out.reserve(len + len / 3);
    for (int i = 0; i < len; ++i) {
        out += digits[i];
        if (const int tail = len - i - 1)
            if (tail % 3 == 0) out += L',';
    }
    return out;
}

static std::wstring FormatSize(unsigned long long bytes) {
    wchar_t buf[64];
    if (bytes >= (1ull << 30))      swprintf(buf, 64, L"%.1f GB", bytes / 1073741824.0);
    else if (bytes >= (1ull << 20)) swprintf(buf, 64, L"%.1f MB", bytes / 1048576.0);
    else if (bytes >= (1ull << 10)) swprintf(buf, 64, L"%.1f KB", bytes / 1024.0);
    else                            swprintf(buf, 64, L"%llu B", bytes);
    return buf;
}

// ============================================================================
//  Storage engine — measure and clean
// ============================================================================

struct TraceResult {
    unsigned long long files  = 0;
    unsigned long long bytes  = 0;
    unsigned long long locked = 0;   // present but not removable
};

static void Merge(TraceResult& into, const TraceResult& from) {
    into.files  += from.files;
    into.bytes  += from.bytes;
    into.locked += from.locked;
}

static TraceResult MeasureFile(const fs::path& p) {
    TraceResult r;
    std::error_code ec;
    const unsigned long long sz = fs::file_size(p, ec);
    if (!ec) { r.files = 1; r.bytes = sz; }
    return r;
}

static fs::path TraceBase(const TracePath& tp, const fs::path& profile) {
    switch (tp.root) {
    case TraceRoot::LocalAppData:   return AppDataBase(false);
    case TraceRoot::RoamingAppData: return AppDataBase(true);
    case TraceRoot::Profile:        break;
    }
    return profile;
}

static TraceResult Measure(const TracePath& tp, const fs::path& profile) {
    TraceResult r;
    const fs::path target = AppendRel(TraceBase(tp, profile), tp.rel);
    if (!Exists(target)) return r;

    if (tp.kind == TraceKind::File) {
        Merge(r, MeasureFile(target));
        for (const wchar_t* side : { L"-journal", L"-wal", L"-shm" }) {
            fs::path f = target;
            f += side;
            if (Exists(f)) Merge(r, MeasureFile(f));
        }
        return r;
    }

    std::error_code ec;
    for (fs::recursive_directory_iterator it(target, ec), end; it != end;
         it.increment(ec)) {
        std::error_code ec2;
        if (!it->is_regular_file(ec2) || ec2) continue;
        const unsigned long long sz = fs::file_size(it->path(), ec2);
        if (!ec2) { ++r.files; r.bytes += sz; }
    }
    return r;
}

static TraceResult Clean(const TracePath& tp, const fs::path& profile) {
    TraceResult r;
    const fs::path target = AppendRel(TraceBase(tp, profile), tp.rel);
    if (!Exists(target)) return r;

    auto removeCounted = [&](const fs::path& f) {
        std::error_code ec2;
        const unsigned long long sz = fs::file_size(f, ec2);
        if (fs::remove(f, ec2) && !ec2) { ++r.files; r.bytes += sz; }
        else ++r.locked;
    };

    if (tp.kind == TraceKind::File) {
        removeCounted(target);
        for (const wchar_t* side : { L"-journal", L"-wal", L"-shm" }) {
            fs::path f = target;
            f += side;
            if (Exists(f)) removeCounted(f);
        }
        return r;
    }

    std::error_code mec;
    for (fs::recursive_directory_iterator it(target, mec), end; it != end;
         it.increment(mec)) {
        std::error_code ec2;
        if (!it->is_regular_file(ec2) || ec2) continue;
        const unsigned long long sz = fs::file_size(it->path(), ec2);
        if (!ec2) { ++r.files; r.bytes += sz; }
    }
    std::error_code ec;
    fs::remove_all(target, ec);
    if (ec) ++r.locked;
    return r;
}

// Applies `op` (Measure or Clean) to every selected item of a browser,
// across every profile directory.
template <typename Op>
static TraceResult RunBrowser(const BrowserDef& def, Op op,
                              const std::vector<char>* filter) {
    TraceResult r;
    for (size_t i = 0; i < def.items.size(); ++i) {
        if (filter && !(*filter)[i]) continue;
        for (const fs::path& profile : ProfileDirs(def))
            for (const TracePath& tp : def.items[i].paths)
                Merge(r, op(tp, profile));
    }
    return r;
}

// ============================================================================
//  Process module — detect and close running browsers
// ============================================================================

static std::vector<DWORD> FindRunning(const std::wstring& exe) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (Lower(pe.szExeFile) == Lower(exe)) pids.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

static bool IsRunning(const BrowserDef& def) {
    const wchar_t* cursor = def.exes;
    while (*cursor) {
        std::wstring exe(cursor);
        const size_t semi = exe.find(L';');
        if (semi != std::wstring::npos) exe.resize(semi);
        if (!exe.empty() && !FindRunning(exe).empty()) return true;
        cursor += exe.size();
        if (*cursor == L';') ++cursor;
    }
    return false;
}

static void CloseBrowser(const BrowserDef& def) {
    const wchar_t* cursor = def.exes;
    while (*cursor) {
        std::wstring exe(cursor);
        const size_t semi = exe.find(L';');
        if (semi != std::wstring::npos) exe.resize(semi);
        for (const DWORD pid : FindRunning(exe)) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
            if (h) { TerminateProcess(h, 1); CloseHandle(h); }
        }
        cursor += exe.size();
        if (*cursor == L';') ++cursor;
    }
    Sleep(800);  // give the OS a moment to release file locks
}

// ============================================================================
//  Application state + actions
// ============================================================================

struct BrowserState {
    std::vector<char>  checked;     // parallel to BrowserDef::items
    bool   installed = false;
    bool   expanded  = false;
    bool   measured  = false;
    unsigned long long files = 0;
    unsigned long long bytes = 0;
};

struct App {
    std::vector<BrowserDef>   catalog;
    std::vector<BrowserState> state;
    std::wstring statusMain = L"Choose what to remove, then press Scan or Clean.";
    std::wstring statusSub  = L"Passwords and sessions stay untouched unless selected.";
    Color  statusColor = Color(255, 152, 161, 172);
    bool   busy = false;
};
static App g;

static bool Selected(const BrowserState& s) {
    return std::any_of(s.checked.begin(), s.checked.end(),
                       [](char c) { return c != 0; });
}

static bool AnySelected() {
    for (const BrowserState& s : g.state)
        if (Selected(s)) return true;
    return false;
}

static void SetStatus(std::wstring main, std::wstring sub, Color color) {
    g.statusMain = std::move(main);
    g.statusSub  = std::move(sub);
    g.statusColor = color;
}

// Confirm and close a running browser so its data files can be cleaned safely.
static void ConfirmCloseRunning(HWND hwnd, size_t browserIndex) {
    const BrowserDef& def = g.catalog[browserIndex];
    if (!IsRunning(def)) return;
    wchar_t msg[420];
    swprintf(msg, 420,
             L"%s is running.\n\n"
             L"It will be closed automatically so its data files can be "
             L"cleaned safely.\n\nProceed?", def.name);
    if (MessageBoxW(hwnd, msg, L"Browser Cleaner",
                    MB_OKCANCEL | MB_ICONWARNING) == IDOK)
        CloseBrowser(def);
}

static void ScanAll(HWND hwnd) {
    g.busy = true;
    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd);                       // paint "Working..." immediately

    unsigned long long files = 0, bytes = 0;
    for (size_t b = 0; b < g.catalog.size(); ++b) {
        BrowserState& s = g.state[b];
        const TraceResult r = RunBrowser(g.catalog[b],
            [](const TracePath& tp, const fs::path& p) { return Measure(tp, p); },
            &s.checked);
        s.measured = true;
        s.files = r.files;
        s.bytes = r.bytes;
        files += r.files;
        bytes += r.bytes;
    }

    SetStatus(FormatNumber(files) + L" browser traces found",
              FormatSize(bytes) + L" of tracking data on the selected items.",
              Theme::Accent);
    g.busy = false;
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void CleanAll(HWND hwnd) {
    g.busy = true;
    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd);

    for (size_t b = 0; b < g.catalog.size(); ++b) {
        if (!Selected(g.state[b])) continue;
        ConfirmCloseRunning(hwnd, b);         // may close the browser
    }

    unsigned long long files = 0, bytes = 0, locked = 0;
    for (size_t b = 0; b < g.catalog.size(); ++b) {
        BrowserState& s = g.state[b];
        if (!Selected(s)) continue;
        const TraceResult r = RunBrowser(g.catalog[b],
            [](const TracePath& tp, const fs::path& p) { return Clean(tp, p); },
            &s.checked);
        files  += r.files;
        bytes  += r.bytes;
        locked += r.locked;
    }

    // Re-measure so the cards reflect what actually remains on disk.
    for (size_t b = 0; b < g.catalog.size(); ++b) {
        BrowserState& s = g.state[b];
        const TraceResult left = RunBrowser(g.catalog[b],
            [](const TracePath& tp, const fs::path& p) { return Measure(tp, p); },
            &s.checked);
        s.files = left.files;
        s.bytes = left.bytes;
    }

    SetStatus(
        FormatNumber(files) + L" tracking traces removed \u00b7 " +
        FormatSize(bytes) + L" freed",
        locked ? (FormatNumber(locked) + L" files could not be removed (in use).")
               : std::wstring(L"Less tracking data is stored in your browsers now."),
        (files || bytes) ? Theme::Success : Theme::TextSub);
    g.busy = false;
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ============================================================================
//  UI — layout, painting, input
// ============================================================================

enum {
    ID_NONE      = 0,
    ID_CLEAN     = 100,
    ID_SCAN      = 101,
    ID_SELECTALL = 102,
    ID_CLEARALL  = 103,
    ID_CARD      = 200,   // header widget: ID_CARD + browser index
    //
    // item chips encode as 1000 + browserIndex*100 + itemIndex
};

struct Box { float x = 0, y = 0, w = 0, h = 0; };

struct Widget {
    int id = ID_NONE;
    Box  box;
};

enum TextAlign { AlignLeft, AlignRight, AlignCenter };

static constexpr float DESIGN_W = 620.0f;   // content width at 96 dpi
static const DWORD WIN_STYLE =
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

static bool IsChip(int id) {
    return id >= 1000 && id < 1000 + (int)g.catalog.size() * 100;
}

static bool IsCard(int id) { return id >= ID_CARD && id < 1000; }

struct Ui {
    UINT dpi = 96;
    float S(float px) const { return px * dpi / 96.0f; }

    FontFamily* family = nullptr;
    Font* title    = nullptr;
    Font* subtitle = nullptr;
    Font* browser  = nullptr;
    Font* chip     = nullptr;
    Font* small    = nullptr;
    Font* avatar   = nullptr;
    Font* status   = nullptr;

    // metrics (dpi scaled; set in Layout())
    float width = 0, height = 0;
    float pad = 0, headerH = 0, footerH = 0, contentTop = 0, footerY = 0;
    float cardH = 0, cardGap = 0;
    float chipH = 0, chipGap = 0, chipW = 0, chipLeft = 0, cardPadX = 0;
    float contentEnd = 0;           // absolute y where the card list ends
    static constexpr int CHIP_COLUMNS = 3;

    Box scanBtn{}, cleanBtn{}, selAll{}, clrAll{};
    std::vector<Widget> widgets;
    int hoverId = ID_NONE;
    int pressedId = ID_NONE;
};
static Ui ui;

// ---- fonts -----------------------------------------------------------------

static void FreeFonts() {
    delete ui.title;    ui.title = nullptr;
    delete ui.subtitle; ui.subtitle = nullptr;
    delete ui.browser;  ui.browser = nullptr;
    delete ui.chip;     ui.chip = nullptr;
    delete ui.small;    ui.small = nullptr;
    delete ui.avatar;   ui.avatar = nullptr;
    delete ui.status;   ui.status = nullptr;
    delete ui.family;   ui.family = nullptr;
}

static void CreateFonts() {
    FreeFonts();
    ui.family = new FontFamily(L"Segoe UI");
    auto mk = [&](float px, G::FontStyle style) {
        return new Font(ui.family, ui.S(px), style, G::UnitPixel);
    };
    ui.title    = mk(21.0f, G::FontStyleBold);
    ui.subtitle = mk(12.5f, G::FontStyleRegular);
    ui.browser  = mk(14.0f, G::FontStyleBold);
    ui.chip     = mk(12.0f, G::FontStyleRegular);
    ui.small    = mk(10.0f, G::FontStyleRegular);
    ui.avatar   = mk(12.0f, G::FontStyleBold);
    ui.status   = mk(12.5f, G::FontStyleRegular);
}

// ---- drawing primitives ------------------------------------------------------

static void FillRound(Graphics& gfx, const Box& r, float radius, const Color& c) {
    GraphicsPath p;
    float rad = radius;
    if (rad * 2 > r.h) rad = r.h / 2;
    if (rad * 2 > r.w) rad = r.w / 2;
    const float d = rad * 2;
    if (d <= 0.5f) {
        p.AddRectangle(RectF(r.x, r.y, r.w, r.h));
    } else {
        p.AddArc(r.x, r.y, d, d, 180, 90);
        p.AddArc(r.x + r.w - d, r.y, d, d, 270, 90);
        p.AddArc(r.x + r.w - d, r.y + r.h - d, d, d, 0, 90);
        p.AddArc(r.x, r.y + r.h - d, d, d, 90, 90);
        p.CloseFigure();
    }
    SolidBrush b(c);
    gfx.FillPath(&b, &p);
}

static void StrokeRound(Graphics& gfx, const Box& r, float radius,
                        const Color& c, float width) {
    GraphicsPath p;
    float rad = radius;
    if (rad * 2 > r.h) rad = r.h / 2;
    if (rad * 2 > r.w) rad = r.w / 2;
    const float d = rad * 2;
    if (d <= 0.5f) {
        p.AddRectangle(RectF(r.x, r.y, r.w, r.h));
    } else {
        p.AddArc(r.x, r.y, d, d, 180, 90);
        p.AddArc(r.x + r.w - d, r.y, d, d, 270, 90);
        p.AddArc(r.x + r.w - d, r.y + r.h - d, d, d, 0, 90);
        p.AddArc(r.x, r.y + r.h - d, d, d, 90, 90);
        p.CloseFigure();
    }
    Pen pen(c, width);
    gfx.DrawPath(&pen, &p);
}

static float TextW(Graphics& gfx, const Font& f, const std::wstring& s) {
    RectF m;
    gfx.MeasureString(s.c_str(), -1, &f, PointF(0, 0), &m);
    return m.Width;
}

static float TextH(Graphics& gfx, const Font& f, const std::wstring& s) {
    RectF m;
    gfx.MeasureString(s.c_str(), -1, &f, PointF(0, 0), &m);
    return m.Height;
}

static void Text(Graphics& gfx, const std::wstring& s, const Font& f,
                 const Color& c, float x, float y) {
    SolidBrush b(c);
    gfx.DrawString(s.c_str(), -1, &f, PointF(x, y), &b);
}

// Single line, vertically centered in `r`, horizontally per `align`.
static void TextRow(Graphics& gfx, const std::wstring& s, const Font& f,
                    const Color& c, const Box& r, TextAlign align = AlignLeft) {
    const float h = TextH(gfx, f, s);
    const float y = r.y + (r.h - h) / 2.0f;
    float x = r.x;
    if (align == AlignRight)  x = r.x + r.w - TextW(gfx, f, s);
    if (align == AlignCenter) x = r.x + (r.w - TextW(gfx, f, s)) / 2.0f;
    Text(gfx, s, f, c, x, y);
}

static void Line(Graphics& gfx, float x1, float y1, float x2, float y2,
                 const Color& c, float px) {
    Pen pen(c, px);
    pen.SetStartCap(G::LineCapRound);
    pen.SetEndCap(G::LineCapRound);
    gfx.DrawLine(&pen, x1, y1, x2, y2);
}

static void CheckMark(Graphics& gfx, float cx, float cy, float size) {
    const float arm = size / 2.0f;
    Line(gfx, cx - arm, cy, cx - arm * 0.25f, cy + arm * 0.85f, Theme::White, ui.S(2));
    Line(gfx, cx - arm * 0.25f, cy + arm * 0.85f, cx + arm, cy - arm * 0.6f,
         Theme::White, ui.S(2));
}

// ---- layout ------------------------------------------------------------------

// Total visual height of one card: header + (when expanded) the chip grid.
static float CardHeight(const BrowserState& s) {
    if (!s.expanded) return ui.cardH;
    const size_t rows =
        (s.checked.size() + ui.CHIP_COLUMNS - 1) / ui.CHIP_COLUMNS;
    return ui.cardH + ui.S(12) + rows * (ui.chipH + ui.chipGap) + ui.S(12);
}

// Rebuild metrics + widget boxes from ui.width/ui.height.
static void Layout() {
    ui.pad        = ui.S(26);
    ui.headerH    = ui.S(88);
    ui.footerH    = ui.S(120);
    ui.contentTop = ui.headerH + ui.S(8);
    ui.footerY    = ui.height - ui.footerH;
    ui.cardH      = ui.S(58);
    ui.cardGap    = ui.S(10);
    ui.chipH      = ui.S(34);
    ui.chipGap    = ui.S(8);
    ui.cardPadX   = ui.S(16);

    const float cardW  = ui.width - ui.pad * 2;
    ui.chipLeft = ui.cardPadX + ui.S(6);
    const float chipsW = cardW - ui.chipLeft - ui.cardPadX;
    ui.chipW = (chipsW - (ui.CHIP_COLUMNS - 1) * ui.chipGap) / ui.CHIP_COLUMNS;

    const float btnH = ui.S(40), btnGap = ui.S(12);
    const float cleanW = ui.S(210), scanW = ui.S(140);
    ui.cleanBtn = Box{ ui.width - ui.pad - cleanW,
                       ui.footerY + ui.S(10), cleanW, btnH };
    ui.scanBtn  = Box{ ui.cleanBtn.x - btnGap - scanW,
                       ui.footerY + ui.S(10), scanW, btnH };
    ui.selAll = Box{ ui.width - ui.pad - ui.S(204), ui.S(50), ui.S(98), ui.S(30) };
    ui.clrAll = Box{ ui.width - ui.pad - ui.S(100), ui.S(50), ui.S(90), ui.S(30) };

    // Cards stack from the top: each card occupies CardHeight() plus a gap.
    ui.widgets.clear();
    float y = ui.contentTop;
    for (size_t b = 0; b < g.catalog.size(); ++b) {
        const BrowserState& s = g.state[b];
        ui.widgets.push_back({ ID_CARD + (int)b,
                               Box{ ui.pad, y, cardW, ui.cardH } });
        if (s.expanded) {
            for (size_t i = 0; i < s.checked.size(); ++i) {
                const size_t row = i / ui.CHIP_COLUMNS;
                const size_t col = i % ui.CHIP_COLUMNS;
                const float x  = ui.pad + ui.chipLeft + col * (ui.chipW + ui.chipGap);
                const float cy = y + ui.cardH + ui.S(12) +
                                 row * (ui.chipH + ui.chipGap);
                ui.widgets.push_back({ (int)(1000 + b * 100 + i),
                                       Box{ x, cy, ui.chipW, ui.chipH } });
            }
        }
        y += CardHeight(s) + ui.cardGap;
    }
    ui.contentEnd = y - ui.cardGap;
}

static float WorkAreaHeight(HWND hwnd) {
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        return (float)(mi.rcWork.bottom - mi.rcWork.top);
    return 780.0f;
}

// Full client height needed to show header + content + footer.
static float NeededHeight() {
    return ui.contentEnd + ui.footerH;
}

// Recompute layout and resize the window so all content is visible
// (clamped to the monitor work area).
static void SizeToContent(HWND hwnd) {
    ui.width  = ui.S(DESIGN_W);
    ui.height = WorkAreaHeight(hwnd);   // measuring pass
    Layout();
    ui.height = std::min(NeededHeight(), WorkAreaHeight(hwnd));
    Layout();                           // final pass with the real height

    RECT want{ 0, 0, (LONG)ui.width, (LONG)ui.height };
    AdjustWindowRect(&want, WIN_STYLE, FALSE);
    SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left,
                 want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Every window opens in the middle of the work area rather than wherever Windows
// would have put it, so the program is where the eye already is.
static void CenterOnWorkArea(HWND hwnd) {
    RECT size = {};
    if (!GetWindowRect(hwnd, &size)) return;
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) &&
        !SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0))
        SetRect(&mi.rcWork, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    SetWindowPos(hwnd, nullptr,
                 mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (size.right - size.left)) / 2,
                 mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (size.bottom - size.top)) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---- hit testing ---------------------------------------------------------------

static int HoverAt(HWND hwnd) {
    DWORD pos = GetMessagePos();
    POINT pt{ GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
    ScreenToClient(hwnd, &pt);
    const float x = (float)pt.x, y = (float)pt.y;
    auto inside = [&](const Box& b) {
        return x >= b.x && y >= b.y && x < b.x + b.w && y < b.y + b.h;
    };
    if (inside(ui.scanBtn)  && !g.busy) return ID_SCAN;
    if (inside(ui.cleanBtn) && !g.busy && AnySelected()) return ID_CLEAN;
    if (inside(ui.selAll)   && !g.busy) return ID_SELECTALL;
    if (inside(ui.clrAll)   && !g.busy) return ID_CLEARALL;
    for (const Widget& w : ui.widgets)
        if (inside(w.box)) return w.id;
    return ID_NONE;
}

// ---- painting --------------------------------------------------------------------

static void DrawShell(Graphics& gfx) {
    gfx.SetSmoothingMode(G::SmoothingModeAntiAlias);
    gfx.SetTextRenderingHint(G::TextRenderingHintClearTypeGridFit);
    gfx.Clear(Theme::Window);
    Text(gfx, L"Browser Cleaner", *ui.title, Theme::Text, ui.pad, ui.S(20));
    Text(gfx, L"Remove tracking traces from installed browsers",
         *ui.subtitle, Theme::TextSub, ui.pad, ui.S(56));

    TextRow(gfx, L"Select all", *ui.small,
            g.busy ? Theme::TextDim : Theme::Accent, ui.selAll, AlignCenter);
    TextRow(gfx, L"Clear all", *ui.small,
            g.busy ? Theme::TextDim : Theme::Accent, ui.clrAll, AlignCenter);
}

static void DrawAvatar(Graphics& gfx, const Box& circle, const BrowserDef& def) {
    SolidBrush b(Color(255, def.accent.r, def.accent.g, def.accent.b));
    gfx.FillEllipse(&b, circle.x, circle.y, circle.w, circle.h);
    TextRow(gfx, def.initials, *ui.avatar, Theme::White,
            Box{ circle.x, circle.y - ui.S(1), circle.w, circle.h }, AlignCenter);
}

static void DrawCard(Graphics& gfx, size_t b, const Widget& header) {
    const BrowserDef& def = g.catalog[b];
    const BrowserState& s = g.state[b];
    const Box card{ ui.pad, header.box.y, header.box.w, CardHeight(s) };

    FillRound(gfx, card, ui.S(12), Theme::Card);
    StrokeRound(gfx, card, ui.S(12), Theme::CardBorder, 1.0f);

    DrawAvatar(gfx, Box{ card.x + ui.S(16),
                         card.y + (ui.cardH - ui.S(30)) / 2.0f,
                         ui.S(30), ui.S(30) }, def);

    const float nameX = card.x + ui.S(16) + ui.S(30) + ui.S(14);
    TextRow(gfx, def.name, *ui.browser,
            s.installed ? Theme::Text : Theme::TextDim,
            Box{ nameX, card.y, card.w - (nameX - card.x) - ui.S(40), ui.cardH });

    std::wstring right;
    Color col = Theme::TextSub;
    if (!s.installed) { right = L"Not installed"; col = Theme::TextDim; }
    else if (!s.expanded) {
        right = s.measured
            ? (FormatNumber(s.files) + L" traces \u00b7 " + FormatSize(s.bytes))
            : std::wstring(L"Ready");
    }
    if (!right.empty())
        TextRow(gfx, right, *ui.small, col,
                Box{ card.x + card.w - ui.S(220), card.y, ui.S(175), ui.cardH },
                AlignRight);

    // chevron
    {
        const float cx = card.x + card.w - ui.S(26);
        const float cy = card.y + ui.cardH / 2.0f - ui.S(2);
        const Color c  = s.installed ? Theme::TextSub : Theme::TextDim;
        if (s.expanded) {
            Line(gfx, cx, cy - ui.S(1), cx + ui.S(4), cy + ui.S(3), c, ui.S(1.6f));
            Line(gfx, cx + ui.S(4), cy + ui.S(3), cx, cy + ui.S(7), c, ui.S(1.6f));
        } else {
            Line(gfx, cx + ui.S(1), cy, cx + ui.S(5), cy + ui.S(4), c, ui.S(1.6f));
            Line(gfx, cx + ui.S(5), cy + ui.S(4), cx + ui.S(1), cy + ui.S(8), c, ui.S(1.6f));
        }
    }

    if (!s.expanded) return;

    // item chips
    for (const Widget& w : ui.widgets) {
        if (!IsChip(w.id)) continue;
        const size_t chipB = (size_t)((w.id - 1000) / 100);
        const size_t item  = (size_t)((w.id - 1000) % 100);
        if (chipB != b) continue;

        const bool checked = s.checked[item] != 0;
        const bool hovered = ui.hoverId == w.id;
        const Box& r = w.box;

        if (checked) {
            FillRound(gfx, r, ui.S(9), hovered ? Theme::AccentHover : Theme::Accent);
            CheckMark(gfx, r.x + ui.S(16), r.y + r.h / 2.0f + ui.S(1), ui.S(13));
            TextRow(gfx, def.items[item].label, *ui.chip, Theme::White,
                    Box{ r.x + ui.S(28), r.y, r.w - ui.S(28), r.h });
        } else {
            FillRound(gfx, r, ui.S(9), hovered ? Theme::ChipHover : Theme::Card);
            StrokeRound(gfx, r, ui.S(9),
                        hovered ? Theme::TextDim : Theme::ChipBorder, ui.S(1.2f));
            TextRow(gfx, def.items[item].label, *ui.chip, Theme::TextSub, r,
                    AlignCenter);
        }
    }
}

static void DrawButton(Graphics& gfx, const Box& r, const std::wstring& label,
                       bool hover, bool pressed, bool enabled, bool primary) {
    if (primary) {
        const Color fill = pressed ? Theme::AccentPress
                         : hover   ? Theme::AccentHover
                         : enabled ? Theme::Accent
                                   : Color(255, 44, 49, 58);
        FillRound(gfx, r, ui.S(10), fill);
        TextRow(gfx, label, *ui.browser,
                enabled ? Theme::White : Theme::TextDim, r, AlignCenter);
    } else {
        FillRound(gfx, r, ui.S(10),
                  pressed || hover ? Theme::ChipHover : Theme::Card);
        StrokeRound(gfx, r, ui.S(10),
                    hover ? Theme::ChipBorder : Theme::CardBorder, ui.S(1.2f));
        TextRow(gfx, label, *ui.browser,
                enabled ? Theme::Text : Theme::TextDim, r, AlignCenter);
    }
}

static void DrawFooter(Graphics& gfx) {
    // hairline separating the card list from the action strip
    Line(gfx, ui.pad, ui.footerY, ui.width - ui.pad, ui.footerY,
         Theme::CardBorder, 1.0f);

    // action buttons, top row of the footer
    const bool canClean  = AnySelected() && !g.busy;
    const bool scanHover  = ui.hoverId == ID_SCAN && !g.busy;
    const bool scanPress  = ui.pressedId == ID_SCAN && !g.busy;
    const bool cleanHover = ui.hoverId == ID_CLEAN;
    const bool cleanPress = ui.pressedId == ID_CLEAN;

    DrawButton(gfx, ui.scanBtn, g.busy ? L"Working\u2026" : L"Scan traces",
               scanHover, scanPress, !g.busy, false);
    DrawButton(gfx, ui.cleanBtn,
               AnySelected() ? L"Clean selected items" : L"Nothing selected",
               cleanHover, cleanPress, canClean, true);

    // result text underneath, full width
    Text(gfx, g.statusMain, *ui.status, g.statusColor, ui.pad,
         ui.footerY + ui.S(62));
    Text(gfx, g.statusSub, *ui.small, Theme::TextDim, ui.pad,
         ui.footerY + ui.S(88));
}

static void DrawScene(Graphics& gfx, int w, int h);   // defined below

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    // Double buffer so hover repaints never flicker.
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ prev = SelectObject(mem, bmp);
    {
        Graphics gfx(mem);
        DrawScene(gfx, w, h);
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, prev);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// Renders the complete scene. Shared by WM_PAINT and the --shot debug mode.
static void DrawScene(Graphics& gfx, int w, int h) {
    DrawShell(gfx);
    for (const Widget& w : ui.widgets)
        if (IsCard(w.id)) DrawCard(gfx, (size_t)(w.id - ID_CARD), w);
    DrawFooter(gfx);
    (void)w; (void)h;
}

// --shot=path.png : render the UI to a PNG off-screen and exit (debug only).
static void SaveShot(HWND hwnd, const std::wstring& path) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    HDC wdc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ prev = SelectObject(mem, bmp);
    {
        Graphics gfx(mem);
        DrawScene(gfx, w, h);
    }
    Gdiplus::Bitmap image(bmp, nullptr);
    CLSID png{};
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
    image.Save(path.c_str(), &png, nullptr);

    SelectObject(mem, prev);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, wdc);
}

// ---- input ----------------------------------------------------------------------


static void ToggleBrowserExpand(HWND hwnd, size_t b) {
    BrowserState& s = g.state[b];
    if (!s.installed) return;
    const bool expanding = !s.expanded;
    if (expanding) {
        s.expanded = true;
        if (NeededHeight() > WorkAreaHeight(hwnd)) {
            // accordion: collapse the others so the new card fits
            for (size_t o = 0; o < g.state.size(); ++o)
                if (o != b) g.state[o].expanded = false;
            if (NeededHeight() > WorkAreaHeight(hwnd)) s.expanded = false;
        }
    } else {
        s.expanded = false;
    }
    SizeToContent(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void OnInstantClick(HWND hwnd, int id) {
    size_t b = 0, i = 0;
    if (id == ID_SELECTALL) {
        for (BrowserState& s : g.state)
            if (s.installed) std::fill(s.checked.begin(), s.checked.end(), 1);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (id == ID_CLEARALL) {
        for (BrowserState& s : g.state)
            std::fill(s.checked.begin(), s.checked.end(), 0);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (IsCard(id)) { ToggleBrowserExpand(hwnd, (size_t)(id - ID_CARD)); return; }
    if (IsChip(id)) {
        b = (size_t)((id - 1000) / 100);
        i = (size_t)((id - 1000) % 100);
        g.state[b].checked[i] ^= 1;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        ui.dpi = GetDpiForWindow(hwnd);
        CreateFonts();
        SizeToContent(hwnd);
        CenterOnWorkArea(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, ui.hoverId != ID_NONE
                                             ? (LPCWSTR)IDC_HAND
                                             : (LPCWSTR)IDC_ARROW));
        return TRUE;

    case WM_MOUSEMOVE: {
        const int prev = ui.hoverId;
        ui.hoverId = HoverAt(hwnd);
        if (prev != ui.hoverId) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        ui.hoverId = HoverAt(hwnd);
        if (ui.hoverId == ID_SCAN || ui.hoverId == ID_CLEAN)
            ui.pressedId = ui.hoverId;
        else
            OnInstantClick(hwnd, ui.hoverId);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_CAPTURECHANGED:
        ui.pressedId = ID_NONE;
        return 0;

    case WM_LBUTTONUP: {
        const int was = ui.pressedId;
        ReleaseCapture();
        ui.pressedId = ID_NONE;
        ui.hoverId = HoverAt(hwnd);
        if (was == ID_SCAN && ui.hoverId == ID_SCAN) ScanAll(hwnd);
        else if (was == ID_CLEAN && ui.hoverId == ID_CLEAN) CleanAll(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        else if (wp == VK_RETURN && AnySelected() && !g.busy) CleanAll(hwnd);
        return 0;

    case WM_DPICHANGED: {
        ui.dpi = HIWORD(wp);
        CreateFonts();
        SizeToContent(hwnd);
        const RECT* suggest = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, suggest->left, suggest->top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_DESTROY:
        FreeFonts();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================================
//  Entry point
// ============================================================================

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    // Catalog + initial state: History / Cookies / Autofill / Cache checked;
    // Passwords and Session restore are opt-in.
    g.catalog = BuildCatalog();
    g.state.resize(g.catalog.size());
    for (size_t b = 0; b < g.catalog.size(); ++b) {
        BrowserState& s = g.state[b];
        s.checked.assign(g.catalog[b].items.size(), 0);
        s.installed = !ProfileDirs(g.catalog[b]).empty();
        for (size_t i = 0; i < s.checked.size(); ++i) {
            const std::wstring label = g.catalog[b].items[i].label;
            // Passwords and session restore are opt-in; IE has no autofill row,
            // so its four items must not be selected by a hard-coded index.
            s.checked[i] = (label == L"Passwords" || label == L"Session") ? 0 : 1;
        }
    }

    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();

    ULONG_PTR token = 0;
    G::GdiplusStartupInput si;
    G::GdiplusStartup(&token, &si, nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"BrowserCleanerWnd";
    RegisterClassExW(&wc);

    // Show the first installed browser expanded so the list looks alive.
    for (BrowserState& s : g.state)
        if (s.installed) { s.expanded = true; break; }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"Browser Cleaner \u2014 Anti-Tracking", WIN_STYLE,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Off-screen render + exit, for automated UI inspection.
    {
        const std::wstring cmd = GetCommandLineW();
        const size_t at = cmd.find(L"--shot=");
        if (at != std::wstring::npos) {
            std::wstring path = cmd.substr(at + 7);
            const size_t quote = path.find(L'\"');
            if (quote != std::wstring::npos) path.resize(quote);
            SaveShot(hwnd, path);
            DestroyWindow(hwnd);
        }
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    G::GdiplusShutdown(token);
    return (int)m.wParam;
}
