// ============================================================================
//  main.cpp — the PCSpeedTest window, and the two console modes a script or a
//  support session can use: --run prints one measured row per metric and
//  appends it to the CSV, --empty resets the CSV to its header without a
//  window, --selftest proves the engine without touching the
//  network.
//
//  The window is fifteen rows in the house palette: the metric, how it is
//  measured, and the figure as it lands. A click on a row measures just that
//  row; the footer button measures all fifteen. Every completed run is appended
//  to %LOCALAPPDATA%\PCSpeedTest\results.csv so successive runs can be compared
//  rather than remembered.
// ============================================================================

#include "pcspeedtest.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace pt;

namespace {

// ---------------------------------------------------------------------------
//  Palette and fonts
// ---------------------------------------------------------------------------

constexpr COLORREF kBackground = RGB(24, 25, 31);
constexpr COLORREF kCard = RGB(34, 36, 44);
constexpr COLORREF kCardHover = RGB(42, 44, 53);
constexpr COLORREF kStroke = RGB(57, 60, 71);
constexpr COLORREF kText = RGB(236, 238, 244);
constexpr COLORREF kTextDim = RGB(146, 152, 164);
constexpr COLORREF kAccent = RGB(84, 158, 255);
constexpr COLORREF kAccentHover = RGB(64, 134, 236);
constexpr COLORREF kOnAccent = RGB(250, 252, 255);
constexpr COLORREF kOk = RGB(88, 205, 143);
constexpr COLORREF kWarn = RGB(242, 178, 84);

const wchar_t* kClassName = L"PCSpeedTestWindow";

// One copyright line for the whole program: the window's footer, the two
// console modes and (worded for a version block) res/app.rc.
const wchar_t* kCopyright = L"Copyright \u00A9 2026-2027 Charles McDonald";

UINT  g_dpi = 96;
HFONT g_fontTitle = nullptr;
HFONT g_fontBody = nullptr;
HFONT g_fontBodyBold = nullptr;
HFONT g_fontSmall = nullptr;
HFONT g_fontFigure = nullptr;

int S(int logical) { return MulDiv(logical, static_cast<int>(g_dpi), 96); }

// Every window opens in the middle of the work area rather than wherever Windows
// would have put it, so the program is where the eye already is.
void CenterOnWorkArea(HWND window) {
    RECT size = {};
    if (!GetWindowRect(window, &size)) return;
    MONITORINFO info = {};
    info.cbSize = sizeof info;
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info) &&
        !SystemParametersInfoW(SPI_GETWORKAREA, 0, &info.rcWork, 0))
        SetRect(&info.rcWork, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    const int width = size.right - size.left;
    const int height = size.bottom - size.top;
    SetWindowPos(window, nullptr,
                 info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - width) / 2,
                 info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) - height) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

HFONT MakeFont(int points, int weight) {
    return CreateFontW(-MulDiv(points, static_cast<int>(g_dpi), 72), 0, 0, 0, weight, FALSE,
                       FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void BuildFonts() {
    for (HFONT* font : {&g_fontTitle, &g_fontBody, &g_fontBodyBold, &g_fontSmall, &g_fontFigure}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
    g_fontTitle = MakeFont(17, FW_SEMIBOLD);
    g_fontBody = MakeFont(9, FW_NORMAL);
    g_fontBodyBold = MakeFont(9, FW_SEMIBOLD);
    g_fontSmall = MakeFont(8, FW_NORMAL);
    g_fontFigure = MakeFont(11, FW_SEMIBOLD);
}

UINT QueryDpi(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    if (auto fn = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                  "GetDpiForWindow")))) {
        if (const UINT dpi = fn(window)) return dpi;
    }
    return 96;
}

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------

constexpr UINT kMessageReading = WM_APP + 1;  // LPARAM: new Update*
constexpr UINT kMessageFinished = WM_APP + 2;

struct Layout {
    RECT rows[15] = {};
    int  rowIndex[15] = {};
    int  rowCount = 0;
    RECT groupHeader[5] = {};
    int  groupCount = 0;
    int  listTop = 0;
    int  listBottom = 0;
    int  noteTop = 0;
    int  contentHeight = 0;
    int  maxScroll = 0;
    int  rowAdvance = 0;
    RECT scrollThumb = {};
    RECT btnPrimary = {};
    RECT btnHistory = {};
    RECT btnOpen = {};
};

struct App {
    Machine            machine;
    std::vector<Reading> readings;
    std::wstring       message;
    std::wstring       csvPath;
    bool               running = false;
    int                completed = 0;
    int                hoverRow = -1;
    int                hotButton = 0;  // 1 primary, 2 open results
    int                scroll = 0;
    Layout             layout;
};

App*              g_app = nullptr;
std::thread       g_worker;
std::atomic<bool> g_cancel{false};

// ---------------------------------------------------------------------------
//  Drawing primitives
// ---------------------------------------------------------------------------

void Fill(HDC dc, const RECT& rect, COLORREF colour, int radius) {
    HBRUSH brush = CreateSolidBrush(colour);
    HPEN   pen = CreatePen(PS_SOLID, 1, colour);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void Stroke(HDC dc, const RECT& rect, COLORREF colour, int radius) {
    HPEN   pen = CreatePen(PS_SOLID, 1, colour);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void PutText(HDC dc, const RECT& rect, const std::wstring& text, HFONT font, COLORREF colour,
             UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, colour);
    SetBkMode(dc, TRANSPARENT);
    RECT box = rect;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &box, format);
    SelectObject(dc, old);
}

// ---------------------------------------------------------------------------
//  Layout
// ---------------------------------------------------------------------------

void BuildLayout(HWND window) {
    RECT client = {};
    GetClientRect(window, &client);
    Layout layout;

    layout.listTop = S(132);

    const int buttonHeight = S(32);
    const int buttonTop = client.bottom - S(16) - buttonHeight;
    layout.noteTop = buttonTop - S(18);
    const int availableBottom = layout.noteTop - S(8);

    const int rowHeight = S(42);
    const int rowAdvance = rowHeight + S(2);
    const int headerHeight = S(16);
    const int headerAdvance = headerHeight + S(4);
    layout.rowAdvance = rowAdvance;

    const std::vector<Metric>& catalog = Metrics();

    int contentHeight = 0;
    for (size_t i = 0; i < catalog.size(); ++i) {
        if (i == 0 || catalog[i].group != catalog[i - 1].group) contentHeight += headerAdvance;
        contentHeight += rowAdvance;
    }
    layout.contentHeight = contentHeight;

    // The viewport ends on a row's bottom edge, counted the way the drawing
    // loop walks the list: a group heading costs height too, so dividing the
    // space by the row pitch alone would lose a row that does fit.
    const int available = availableBottom - layout.listTop;
    int viewport = 0;
    for (size_t i = 0; i < catalog.size(); ++i) {
        int bottom = viewport;
        if (i == 0 || catalog[i].group != catalog[i - 1].group) bottom += headerAdvance;
        bottom += rowAdvance;
        if (bottom - S(2) > available) break;
        viewport = bottom;
    }
    if (viewport == 0) viewport = rowAdvance - S(2);
    else viewport -= S(2);
    layout.listBottom = layout.listTop + viewport;

    const int extra = contentHeight + rowAdvance - viewport;
    layout.maxScroll =
        contentHeight > viewport && extra > 0
            ? ((extra + rowAdvance - 1) / rowAdvance) * rowAdvance
            : 0;
    if (g_app->scroll > layout.maxScroll) g_app->scroll = layout.maxScroll;
    if (g_app->scroll < 0) g_app->scroll = 0;
    if (rowAdvance > 0) g_app->scroll = (g_app->scroll / rowAdvance) * rowAdvance;

    int y = layout.listTop - g_app->scroll;
    for (size_t i = 0; i < catalog.size() && layout.rowCount < 15; ++i) {
        const bool firstGroup = i == 0 || catalog[i].group != catalog[i - 1].group;
        if (firstGroup) {
            RECT header = {S(26), y, client.right - S(34), y + headerHeight};
            layout.groupHeader[layout.groupCount++] = header;
            y += headerAdvance;
        }
        RECT row = {S(26), y, client.right - S(34), y + rowHeight};
        layout.rowIndex[layout.rowCount] = static_cast<int>(i);
        layout.rows[layout.rowCount++] = row;
        y += rowAdvance;
    }

    if (layout.maxScroll > 0) {
        const int left = client.right - S(18);
        const int track = layout.listBottom - layout.listTop;
        int       thumb = MulDiv(track, viewport, contentHeight);
        if (thumb < S(24)) thumb = S(24);
        const int travel = track - thumb;
        const int offset = MulDiv(travel, g_app->scroll, layout.maxScroll);
        layout.scrollThumb.left = left;
        layout.scrollThumb.right = left + S(4);
        layout.scrollThumb.top = layout.listTop + offset;
        layout.scrollThumb.bottom = layout.scrollThumb.top + thumb;
    }

    int right = client.right - S(26);
    layout.btnPrimary.right = right;
    layout.btnPrimary.left = right - S(126);
    layout.btnPrimary.top = buttonTop;
    layout.btnPrimary.bottom = buttonTop + buttonHeight;

    layout.btnHistory.right = layout.btnPrimary.left - S(10);
    layout.btnHistory.left = layout.btnHistory.right - S(122);
    layout.btnHistory.top = buttonTop;
    layout.btnHistory.bottom = buttonTop + buttonHeight;

    layout.btnOpen.right = layout.btnHistory.left - S(10);
    layout.btnOpen.left = layout.btnOpen.right - S(150);
    layout.btnOpen.top = buttonTop;
    layout.btnOpen.bottom = buttonTop + buttonHeight;

    g_app->layout = layout;
}

bool Inside(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
           point.y < rect.bottom;
}

int HitRow(POINT point) {
    if (point.y < g_app->layout.listTop || point.y >= g_app->layout.listBottom) return -1;
    for (int i = 0; i < g_app->layout.rowCount; ++i)
        if (Inside(g_app->layout.rows[i], point)) return g_app->layout.rowIndex[i];
    return -1;
}

int HitButton(POINT point) {
    if (Inside(g_app->layout.btnPrimary, point)) return 1;
    if (Inside(g_app->layout.btnHistory, point)) return 2;
    if (Inside(g_app->layout.btnOpen, point)) return 3;
    return 0;
}

// ---------------------------------------------------------------------------
//  Painting
// ---------------------------------------------------------------------------

void PaintButton(HDC dc, const RECT& rect, const std::wstring& text, bool primary, bool hot) {
    const int radius = S(8);
    if (primary) {
        Fill(dc, rect, hot ? kAccentHover : kAccent, radius);
        PutText(dc, rect, text, g_fontBodyBold, kOnAccent,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        Fill(dc, rect, hot ? kCardHover : kCard, radius);
        Stroke(dc, rect, kStroke, radius);
        PutText(dc, rect, text, g_fontBody, hot ? kText : kTextDim,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void Paint(HWND window) {
    PAINTSTRUCT ps;
    HDC         dc = BeginPaint(window, &ps);

    RECT client = {};
    GetClientRect(window, &client);
    HDC     buffer = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);

    HBRUSH background = CreateSolidBrush(kBackground);
    FillRect(buffer, &client, background);
    DeleteObject(background);

    const std::vector<Metric>& catalog = Metrics();

    RECT title = {S(26), S(18), client.right - S(26), S(18) + S(26)};
    PutText(buffer, title, L"PCSpeedTest", g_fontTitle, kText, DT_LEFT | DT_SINGLELINE);

    std::wstring summary = g_app->running
                               ? L"measuring - " + std::to_wstring(g_app->completed) + L" of " +
                                     std::to_wstring(catalog.size()) + L" done"
                               : L"fifteen figures for one machine, written to a CSV you can chart";
    RECT subtitle = {S(26), S(46), client.right - S(26), S(46) + S(16)};
    PutText(buffer, subtitle, summary, g_fontSmall, g_app->running ? kAccent : kTextDim,
            DT_LEFT | DT_SINGLELINE);

    // Three lines, because one line long enough for all of it would be
    // ellipsised on the machines with the longest processor names: the hardware
    // first, then the hardware Windows draws with, then the platform it runs.
    std::wstring hardware =
        g_app->machine.cpu + L"  -  " + std::to_wstring(g_app->machine.physicalCores) +
        L" cores / " + std::to_wstring(g_app->machine.cores) + L" threads  -  " +
        FormatValue(Reading{true, g_app->machine.ramGiB, L""},
                    Metric{L"RAM", L"", L"GiB", Group::Memory, ""});
    // The modules' own figures sit beside the installed size: a speed without the
    // memory it belongs to would be a riddle.
    if (!g_app->machine.ramSpeed.empty()) {
        hardware += L" (" + g_app->machine.ramSpeed;
        if (!g_app->machine.ramMaker.empty()) hardware += L", " + g_app->machine.ramMaker;
        hardware += L")";
    }
    RECT hardwareLine = {S(26), S(74), client.right - S(26), S(74) + S(16)};
    PutText(buffer, hardwareLine, hardware, g_fontSmall, kTextDim,
            DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    std::wstring graphics = g_app->machine.gpu;
    if (graphics.empty())
        graphics = L"no graphics adapter reported by Windows";
    else if (!g_app->machine.driver.empty())
        graphics += L"  -  driver " + g_app->machine.driver;
    RECT graphicsLine = {S(26), S(90), client.right - S(26), S(90) + S(16)};
    PutText(buffer, graphicsLine, graphics, g_fontSmall, kTextDim,
            DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    std::wstring platform = g_app->machine.os + L"  -  " + g_app->machine.arch;
    if (g_app->machine.linkGbps > 0.0)
        platform += L"  -  " + g_app->machine.link + L" " +
                    FormatValue(Reading{true, g_app->machine.linkGbps, L""},
                                Metric{L"", L"", L"Gbps", Group::Network, ""});
    RECT platformLine = {S(26), S(106), client.right - S(26), S(106) + S(16)};
    PutText(buffer, platformLine, platform, g_fontSmall, kTextDim,
            DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Rows, clipped to the viewport so a scrolled row never paints over the
    // footer.
    HRGN clip = CreateRectRgn(S(0), g_app->layout.listTop, client.right, g_app->layout.listBottom);
    SelectClipRgn(buffer, clip);
    // Group headings are painted from the rows' own catalog entries, so the
    // heading and the rows under it can never disagree.
    size_t nextHeader = 0;
    for (int i = 0; i < g_app->layout.rowCount; ++i) {
        const int index = g_app->layout.rowIndex[i];
        const Metric& metric = catalog[static_cast<size_t>(index)];
        if (index == 0 || metric.group != catalog[static_cast<size_t>(index - 1)].group) {
            if (nextHeader < static_cast<size_t>(g_app->layout.groupCount)) {
                const RECT& header = g_app->layout.groupHeader[nextHeader++];
                PutText(buffer, header, GroupName(metric.group), g_fontSmall, kTextDim,
                        DT_LEFT | DT_SINGLELINE);
            }
        }

        const RECT& row = g_app->layout.rows[i];
        const bool  hot = g_app->hoverRow == index;
        Fill(buffer, row, hot ? kCardHover : kCard, S(8));
        Stroke(buffer, row, hot ? kAccent : kStroke, S(8));

        RECT nameRect = {row.left + S(14), row.top + S(6), row.left + S(340), row.top + S(24)};
        PutText(buffer, nameRect, metric.name, g_fontBodyBold, kText,
                DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT detailRect = {row.left + S(14), row.top + S(24), row.left + S(360),
                           row.top + S(38)};
        PutText(buffer, detailRect, metric.detail, g_fontSmall, kTextDim,
                DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        const Reading& reading = g_app->readings[static_cast<size_t>(index)];
        const std::wstring value = FormatValue(reading, metric);
        RECT valueRect = {row.right - S(300), row.top + S(4), row.right - S(14), row.top + S(26)};
        PutText(buffer, valueRect,
                value.empty() ? (g_app->running ? L"..." : L"-") : value, g_fontFigure,
                reading.ok ? kOk : (g_app->running ? kTextDim : kWarn),
                DT_RIGHT | DT_SINGLELINE);

        RECT noteRect = {row.right - S(360), row.top + S(25), row.right - S(14), row.top + S(39)};
        PutText(buffer, noteRect, reading.note, g_fontSmall, kTextDim,
                DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectClipRgn(buffer, nullptr);
    DeleteObject(clip);

    if (g_app->layout.maxScroll > 0) {
        RECT track = {g_app->layout.scrollThumb.left, g_app->layout.listTop,
                      g_app->layout.scrollThumb.right, g_app->layout.listBottom};
        Fill(buffer, track, kCard, S(2));
        Fill(buffer, g_app->layout.scrollThumb, kStroke, S(2));
    }

    RECT note = {S(26), g_app->layout.noteTop, client.right - S(26) - S(280),
                 g_app->layout.noteTop + S(18)};
    PutText(buffer, note,
            g_app->message.empty() ? L"Every completed run is appended to results.csv; a row click measures just that row."
                                   : g_app->message,
            g_fontSmall, g_app->message.empty() ? kTextDim : kTextDim,
            DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    PaintButton(buffer, g_app->layout.btnPrimary, g_app->running ? L"Stop" : L"Run all",
                true, g_app->hotButton == 1);
    PaintButton(buffer, g_app->layout.btnHistory, L"View results", false, g_app->hotButton == 2);
    PaintButton(buffer, g_app->layout.btnOpen, L"Results folder", false, g_app->hotButton == 3);

    // The copyright sits under the status line, on the button row's spare left
    // end, so it belongs to the window rather than to the message beside it.
    RECT credit = {S(26), g_app->layout.btnPrimary.top, g_app->layout.btnOpen.left - S(16),
                   g_app->layout.btnPrimary.bottom};
    PutText(buffer, credit, kCopyright, g_fontSmall, kTextDim,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    EndPaint(window, &ps);
}

// ---------------------------------------------------------------------------
//  Running the measurements
// ---------------------------------------------------------------------------

void JoinWorker() {
    if (g_worker.joinable()) g_worker.join();
}

void StartRun(HWND window, int onlyIndex) {
    if (g_app->running) return;
    g_app->running = true;
    g_app->completed = 0;
    g_app->message = onlyIndex < 0 ? L"Measuring all fifteen, about eighteen seconds."
                                   : L"Measuring one row.";
    g_cancel = false;

    const Machine machine = g_app->machine;
    const HWND    target = window;

    JoinWorker();
    g_worker = std::thread([target, machine, onlyIndex] {
        const auto progress = [target](const Update& update) {
            if (update.finished) {
                PostMessageW(target, kMessageFinished, 0, 0);
                return;
            }
            auto* copy = new Update(update);
            PostMessageW(target, kMessageReading, 0, reinterpret_cast<LPARAM>(copy));
        };
        const Tick tick = [] { return !g_cancel.load(); };

        if (onlyIndex < 0) {
            MeasureAll(machine, progress, tick);
            return;
        }
        Update update;
        update.index = onlyIndex;
        update.reading = Measure(onlyIndex, machine, tick);
        progress(update);
        Update done;
        done.finished = true;
        progress(done);
    });

    InvalidateRect(window, nullptr, FALSE);
}

void FinishRun(HWND window) {
    JoinWorker();
    g_app->running = false;

    const bool full = g_app->readings.size() == Metrics().size();
    if (full) {
        std::vector<Reading> readings = g_app->readings;
        std::wstring         error;
        if (AppendCsv(g_app->machine, readings, &error))
            g_app->message = L"Measured and appended to results.csv.";
        else
            g_app->message = L"Measured, but the CSV could not be written: " + error;
    } else {
        g_app->message = L"One row measured; run all fifteen to add a CSV line.";
    }
    InvalidateRect(window, nullptr, FALSE);
}

void OpenResultsFolder(HWND window) {
    const std::wstring path = ResultsPath();
    if (path.empty()) {
        g_app->message = L"There is no %LOCALAPPDATA%\\PCSpeedTest folder yet.";
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    std::wstring folder = path;
    const size_t slash = folder.find_last_of(L'\\');
    if (slash != std::wstring::npos) folder.resize(slash);
    ShellExecuteW(window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
//  The results viewer
//
//  results.csv read back as one card per run, newest first: the timestamp, the
//  machine it ran on, and the fifteen figures in the order they were measured.
//  The file itself stays the record; this window is a way to look at it without
//  a spreadsheet, and hands the file to the default application when asked.
// ---------------------------------------------------------------------------

const wchar_t* kHistoryClass = L"PCSpeedTestHistoryWindow";

struct History {
    std::vector<HistoryRow> rows;
    std::wstring            message;
    int                     scroll = 0;
    int                     cardHeight = 0;
    int                     advance = 0;
    int                     listTop = 0;
    int                     listBottom = 0;
    int                     maxScroll = 0;
    RECT                    btnSheet = {};
    RECT                    btnFolder = {};
    RECT                    btnEmpty = {};
    RECT                    btnClose = {};
    int                     hot = 0;
    RECT                    thumb = {};
};

History g_history;
HWND    g_historyWindow = nullptr;

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 1) return std::wstring();
    std::wstring wide(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    return wide;
}

const wchar_t* ShortName(int index) {
    static const wchar_t* names[14] = {L"CPU 1T",   L"CPU N",   L"MEM copy", L"MEM lat",
                                       L"DISK w",   L"DISK r",  L"LINK",     L"LOOP",
                                       L"LAN",      L"WAN",     L"DOWN",     L"UP",
                                       L"GPU FLOP", L"GPU mem"};
    if (index < 0 || index >= 14) return L"";
    return names[index];
}

// "2026-11-18T06:26:43Z" as "2026-11-18 06:26 UTC"
std::wstring Stamp(const std::string& iso) {
    std::wstring text = Utf8ToWide(iso);
    if (text.size() >= 16) {
        text = text.substr(0, 10) + L" " + text.substr(11, 5) + L" UTC";
    }
    return text;
}

std::wstring CellValue(const HistoryRow& row, int index) {
    if (index < 0 || index >= static_cast<int>(row.present.size()) || !row.present[static_cast<size_t>(index)])
        return L"-";
    Reading reading;
    reading.ok = true;
    reading.value = row.values[static_cast<size_t>(index)];
    return FormatValue(reading, Metrics()[static_cast<size_t>(index)]);
}

void BuildHistoryLayout(HWND window) {
    RECT client = {};
    GetClientRect(window, &client);
    History& state = g_history;

    state.listTop = S(92);
    state.cardHeight = S(110);
    state.advance = state.cardHeight + S(8);

    const int buttonHeight = S(32);
    const int buttonTop = client.bottom - S(16) - buttonHeight;
    state.listBottom = buttonTop - S(14);

    const int viewport = state.listBottom - state.listTop;
    const int content = static_cast<int>(state.rows.size()) * state.advance;
    state.maxScroll = content > viewport ? content - viewport : 0;
    if (state.maxScroll > 0) state.maxScroll = ((state.maxScroll + state.advance - 1) /
                                                state.advance) * state.advance;
    if (state.scroll > state.maxScroll) state.scroll = state.maxScroll;
    if (state.scroll < 0) state.scroll = 0;
    if (state.advance > 0) state.scroll = (state.scroll / state.advance) * state.advance;

    if (state.maxScroll > 0) {
        const int left = client.right - S(18);
        const int track = viewport;
        int       thumb = MulDiv(track, viewport, content);
        if (thumb < S(24)) thumb = S(24);
        const int travel = track - thumb;
        const int offset = MulDiv(travel, state.scroll, state.maxScroll);
        state.thumb.left = left;
        state.thumb.right = left + S(4);
        state.thumb.top = state.listTop + offset;
        state.thumb.bottom = state.thumb.top + thumb;
    } else {
        state.thumb = RECT{};
    }

    int right = client.right - S(26);
    auto place = [&](int width, RECT* out) {
        out->right = right;
        out->left = right - S(width);
        out->top = buttonTop;
        out->bottom = buttonTop + buttonHeight;
        right = out->left - S(10);
    };
    place(96, &state.btnClose);
    place(120, &state.btnEmpty);
    place(132, &state.btnFolder);
    place(176, &state.btnSheet);
}

void PaintHistory(HWND window) {
    PAINTSTRUCT ps;
    HDC         dc = BeginPaint(window, &ps);

    RECT client = {};
    GetClientRect(window, &client);
    HDC     buffer = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);

    HBRUSH background = CreateSolidBrush(kBackground);
    FillRect(buffer, &client, background);
    DeleteObject(background);

    RECT title = {S(26), S(18), client.right - S(26), S(18) + S(26)};
    PutText(buffer, title, L"Results", g_fontTitle, kText, DT_LEFT | DT_SINGLELINE);

    std::wstring subtitle;
    if (g_history.rows.empty())
        subtitle = g_history.message.empty() ? L"No run has been recorded yet." : g_history.message;
    else
        subtitle = ResultsPath() + L"  -  " + std::to_wstring(g_history.rows.size()) +
                   (g_history.rows.size() == 1 ? L" run" : L" runs") + L", newest first";
    RECT subtitleRect = {S(26), S(48), client.right - S(26), S(48) + S(16)};
    PutText(buffer, subtitleRect, subtitle, g_fontSmall, kTextDim,
            DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT hint = {S(26), S(66), client.right - S(26), S(66) + S(16)};
    PutText(buffer, hint, L"One card per completed run of all fifteen; click a card to open results.csv.",
            g_fontSmall, kTextDim, DT_LEFT | DT_SINGLELINE);

    HRGN clip = CreateRectRgn(S(0), g_history.listTop, client.right, g_history.listBottom);
    SelectClipRgn(buffer, clip);

    const int count = static_cast<int>(g_history.rows.size());
    for (int slot = 0; slot < count; ++slot) {
        const int index = count - 1 - slot;
        const int top = g_history.listTop - g_history.scroll + slot * g_history.advance;
        if (top > g_history.listBottom || top + g_history.cardHeight < g_history.listTop) continue;

        const HistoryRow& row = g_history.rows[static_cast<size_t>(index)];
        RECT card = {S(26), top, client.right - S(34), top + g_history.cardHeight};
        Fill(buffer, card, kCardHover, S(8));
        Stroke(buffer, card, kStroke, S(8));

        RECT stampRect = {card.left + S(14), card.top + S(8), card.left + S(260), card.top + S(26)};
        PutText(buffer, stampRect, Stamp(row.timestamp), g_fontBodyBold, kText,
                DT_LEFT | DT_SINGLELINE);

        RECT hostRect = {card.right - S(250), card.top + S(9), card.right - S(14), card.top + S(25)};
        std::wstring host = Utf8ToWide(row.host);
        if (!host.empty())
            PutText(buffer, hostRect, host, g_fontSmall, kTextDim,
                    DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT cpuRect = {card.left + S(14), card.top + S(25), card.right - S(14), card.top + S(40)};
        std::wstring cpu = Utf8ToWide(row.cpu);
        if (row.threads > 0) {
            if (row.cores > 0 && row.cores != row.threads)
                cpu += L"  -  " + std::to_wstring(row.cores) + L" cores / " +
                       std::to_wstring(row.threads) + L" threads";
            else
                cpu += L"  -  " + std::to_wstring(row.threads) + L" threads";
        }
        if (row.ramGiB > 0.0)
            cpu += L"  -  " + std::to_wstring(static_cast<int>(row.ramGiB + 0.5)) + L" GiB";
        PutText(buffer, cpuRect, cpu, g_fontSmall, kTextDim,
                DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Fifteen figures as 4 columns, in catalogue order, so the two CPU rows
        // sit beside the two memory rows and so on. The fourth line holds three.
        const int columns = 4;
        const int cellWidth = (card.right - card.left - S(28)) / columns;
        for (int metric = 0; metric < static_cast<int>(Metrics().size()); ++metric) {
            const int column = metric % columns;
            const int line = metric / columns;
            const int left = card.left + S(14) + column * cellWidth;
            const int textTop = card.top + S(46) + line * S(16);

            const std::wstring label = ShortName(metric);
            RECT labelRect = {left, textTop, left + cellWidth, textTop + S(15)};
            PutText(buffer, labelRect, label, g_fontSmall, kTextDim,
                    DT_LEFT | DT_SINGLELINE);

            HGDIOBJ old = SelectObject(buffer, g_fontSmall);
            SIZE    extent = {};
            GetTextExtentPoint32W(buffer, label.c_str(), static_cast<int>(label.size()), &extent);
            SelectObject(buffer, old);

            RECT valueRect = {left + extent.cx + S(6), textTop, left + cellWidth, textTop + S(15)};
            PutText(buffer, valueRect, CellValue(row, metric), g_fontSmall,
                    row.present[static_cast<size_t>(metric)] ? kText : kTextDim,
                    DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    SelectClipRgn(buffer, nullptr);
    DeleteObject(clip);

    if (g_history.maxScroll > 0) {
        RECT track = {g_history.thumb.left, g_history.listTop, g_history.thumb.right,
                      g_history.listBottom};
        Fill(buffer, track, kCard, S(2));
        Fill(buffer, g_history.thumb, kStroke, S(2));
    }

    RECT note = {S(26), g_history.btnClose.top, g_history.btnSheet.left - S(16),
                 g_history.btnClose.bottom};
    PutText(buffer, note, L"Click a card to open results.csv.", g_fontSmall, kTextDim,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    PaintButton(buffer, g_history.btnSheet, L"Open in spreadsheet", false, g_history.hot == 1);
    PaintButton(buffer, g_history.btnFolder, L"Results folder", false, g_history.hot == 2);
    PaintButton(buffer, g_history.btnEmpty, L"Empty file", false, g_history.hot == 4);
    PaintButton(buffer, g_history.btnClose, L"Close", false, g_history.hot == 3);

    BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    EndPaint(window, &ps);
}

void OpenResultsPath(HWND window, const std::wstring& path) {
    if (path.empty()) return;
    ShellExecuteW(window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int HitHistoryButton(POINT point) {
    if (Inside(g_history.btnSheet, point)) return 1;
    if (Inside(g_history.btnFolder, point)) return 2;
    if (Inside(g_history.btnClose, point)) return 3;
    if (Inside(g_history.btnEmpty, point)) return 4;
    return 0;
}

// Emptying is destructive, so the confirmation names what goes and defaults to
// No. What is left is the header alone: the reader then says there are no runs
// rather than showing a file it cannot parse, and the next run appends to it.
void EmptyResults(HWND window) {
    if (g_history.rows.empty()) {
        MessageBoxW(window, L"results.csv holds no runs to remove.", L"Empty results.csv",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    const size_t       count = g_history.rows.size();
    const std::wstring question =
        L"Remove " + std::to_wstring(count) + (count == 1 ? L" run" : L" runs") +
        L" from results.csv?\r\n\r\nThe file keeps its header, so the next run appends to an "
        L"empty file. This cannot be undone.";
    if (MessageBoxW(window, question.c_str(), L"Empty results.csv",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    std::wstring error;
    if (!EmptyCsv(&error)) {
        MessageBoxW(window, error.c_str(), L"Empty results.csv", MB_OK | MB_ICONERROR);
        return;
    }

    g_history.rows = LoadHistory(&g_history.message);
    g_history.scroll = 0;
    g_history.hot = 0;
    BuildHistoryLayout(window);
    InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK HistoryProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            g_dpi = QueryDpi(window);
            BuildFonts();
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark,
                                  sizeof dark);
            BuildHistoryLayout(window);
            return 0;
        }
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wParam);
            BuildFonts();
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            RECT        current = {};
            GetWindowRect(window, &current);
            const int x = suggested ? suggested->left : current.left;
            const int y = suggested ? suggested->top : current.top;
            SetWindowPos(window, nullptr, x, y, S(880), S(640), SWP_NOZORDER | SWP_NOACTIVATE);
            BuildHistoryLayout(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = S(660);
            info->ptMinTrackSize.y = S(420);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (g_history.maxScroll <= 0) return 0;
            const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            g_history.scroll -= notches * g_history.advance;
            BuildHistoryLayout(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            BuildHistoryLayout(window);
            return 0;
        case WM_PAINT:
            PaintHistory(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int   hot = HitHistoryButton(point);
            if (hot != g_history.hot) {
                g_history.hot = hot;
                InvalidateRect(window, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track = {};
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = window;
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_history.hot = 0;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR: {
            POINT point = {0, 0};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            if (HitHistoryButton(point) != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            switch (HitHistoryButton(point)) {
                case 1: OpenResultsPath(window, ResultsPath()); return 0;
                case 2: {
                    std::wstring folder = ResultsPath();
                    const size_t slash = folder.find_last_of(L'\\');
                    if (slash != std::wstring::npos) folder.resize(slash);
                    OpenResultsPath(window, folder);
                    return 0;
                }
                case 3: DestroyWindow(window); return 0;
                case 4: EmptyResults(window); return 0;
                default: break;
            }
            if (point.y >= g_history.listTop && point.y < g_history.listBottom) {
                OpenResultsPath(window, ResultsPath());
                return 0;
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_historyWindow = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void OpenHistory(HWND owner) {
    if (g_historyWindow) {
        g_history.rows = LoadHistory(&g_history.message);
        g_history.scroll = 0;
        BuildHistoryLayout(g_historyWindow);
        InvalidateRect(g_historyWindow, nullptr, FALSE);
        SetForegroundWindow(g_historyWindow);
        return;
    }

    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof cls;
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = HistoryProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(2));
    cls.lpszClassName = kHistoryClass;
    RegisterClassExW(&cls);

    g_history.rows = LoadHistory(&g_history.message);
    g_history.scroll = 0;

    RECT frame = {0, 0, S(880), S(640)};
    AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);

    g_historyWindow = CreateWindowExW(WS_EX_APPWINDOW, kHistoryClass, L"PCSpeedTest - results",
                                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                      frame.right - frame.left, frame.bottom - frame.top, owner,
                                      nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_historyWindow) return;
    CenterOnWorkArea(g_historyWindow);
    ShowWindow(g_historyWindow, SW_SHOW);
    SetForegroundWindow(g_historyWindow);
}

// ---------------------------------------------------------------------------
//  Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            g_dpi = QueryDpi(window);
            BuildFonts();
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark,
                                  sizeof dark);
            BuildLayout(window);
            return 0;
        }
        case kMessageReading: {
            auto* update = reinterpret_cast<Update*>(lParam);
            if (update) {
                if (update->index >= 0 &&
                    update->index < static_cast<int>(g_app->readings.size()))
                    g_app->readings[static_cast<size_t>(update->index)] = update->reading;
                ++g_app->completed;
                delete update;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case kMessageFinished:
            FinishRun(window);
            return 0;
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wParam);
            BuildFonts();
            int width = S(800);
            int height = S(820);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            RECT        current = {};
            GetWindowRect(window, &current);
            const int x = suggested ? suggested->left : current.left;
            const int y = suggested ? suggested->top : current.top;
            SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            BuildLayout(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = S(660);
            info->ptMinTrackSize.y = S(460);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (g_app->layout.maxScroll <= 0) return 0;
            const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            g_app->scroll -= notches * g_app->layout.rowAdvance;
            BuildLayout(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            BuildLayout(window);
            return 0;
        case WM_PAINT:
            Paint(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int   row = HitRow(point);
            const int   button = HitButton(point);
            if (row != g_app->hoverRow || button != g_app->hotButton) {
                g_app->hoverRow = row;
                g_app->hotButton = button;
                InvalidateRect(window, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track = {};
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = window;
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_app->hoverRow = -1;
            g_app->hotButton = 0;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR: {
            POINT point = {0, 0};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            if (HitRow(point) >= 0 || HitButton(point) != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int   row = HitRow(point);
            if (row >= 0 && !g_app->running) {
                g_app->readings[static_cast<size_t>(row)] = Reading{};
                StartRun(window, row);
                return 0;
            }
            switch (HitButton(point)) {
                case 1:
                    if (g_app->running) {
                        g_cancel = true;
                        g_app->message = L"Stopping after the row in flight.";
                        InvalidateRect(window, nullptr, FALSE);
                    } else {
                        g_app->readings.assign(Metrics().size(), Reading{});
                        StartRun(window, -1);
                    }
                    return 0;
                case 2: OpenHistory(window); return 0;
                case 3: OpenResultsFolder(window); return 0;
                default: break;
            }
            return 0;
        }
        case WM_CLOSE:
            if (g_app->running) {
                g_cancel = true;
                g_app->message = L"Stopping; close again once the run has ended.";
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_cancel = true;
            JoinWorker();
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void RegisterMainClass() {
    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof cls;
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = WndProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(2));
    cls.hbrBackground = nullptr;
    cls.lpszClassName = kClassName;
    RegisterClassExW(&cls);
}

int RunGui(HINSTANCE instance) {
    RegisterMainClass();

    static App app;
    g_app = &app;
    app.machine = Describe();
    app.readings.assign(Metrics().size(), Reading{});
    app.csvPath = ResultsPath();
    app.message = app.csvPath.empty() ? L"No %LOCALAPPDATA%, so runs cannot be recorded."
                                      : L"Every completed run is appended to results.csv.";

    RECT frame = {0, 0, MulDiv(800, static_cast<int>(g_dpi), 96),
                  MulDiv(820, static_cast<int>(g_dpi), 96)};
    AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);

    HWND window = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"PCSpeedTest", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left,
                                  frame.bottom - frame.top, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    CenterOnWorkArea(window);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  Console modes
// ---------------------------------------------------------------------------

HANDLE OpenConsole() {
    const HANDLE standard = GetStdHandle(STD_OUTPUT_HANDLE);
    if (standard && standard != INVALID_HANDLE_VALUE &&
        GetFileType(standard) != FILE_TYPE_UNKNOWN)
        return standard;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    return CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                       nullptr);
}

void Say(HANDLE out, const std::wstring& text) {
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (GetFileType(out) == FILE_TYPE_CHAR) {
        WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes,
                        nullptr, nullptr);
    WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void CloseConsole(HANDLE out) {
    if (out && out != INVALID_HANDLE_VALUE && out != GetStdHandle(STD_OUTPUT_HANDLE))
        CloseHandle(out);
}

std::wstring Pad(const std::wstring& text, size_t width) {
    std::wstring out = text;
    while (out.size() < width) out.push_back(L' ');
    return out;
}

int RunConsole() {
    const HANDLE out = OpenConsole();
    const Machine machine = Describe();

    Say(out, L"PCSpeedTest - " + machine.cpu + L"\r\n");
    std::wstring hardware = L"               " + std::to_wstring(machine.physicalCores) +
                            L" cores / " + std::to_wstring(machine.cores) + L" threads, " +
                            FormatValue(Reading{true, machine.ramGiB, L""},
                                        Metric{L"", L"", L"GiB", Group::Memory, ""}) +
                            L" RAM";
    if (!machine.ramSpeed.empty()) {
        hardware += L" (" + machine.ramSpeed;
        if (!machine.ramMaker.empty()) hardware += L", " + machine.ramMaker;
        hardware += L")";
    }
    Say(out, hardware + L"\r\n");
    if (!machine.gpu.empty()) {
        std::wstring graphics = L"               " + machine.gpu;
        if (!machine.driver.empty()) graphics += L", driver " + machine.driver;
        Say(out, graphics + L"\r\n");
    }
    Say(out, L"               " + machine.os + L", " + machine.arch + L"\r\n\r\n");

    std::vector<Reading> readings(Metrics().size());
    const std::vector<Metric>& catalog = Metrics();
    for (size_t i = 0; i < catalog.size(); ++i) {
        Say(out, Pad(catalog[i].name, 22) + L"  ");
        readings[i] = Measure(static_cast<int>(i), machine, nullptr);
        const std::wstring value = FormatValue(readings[i], catalog[i]);
        Say(out, Pad(value.empty() ? L"unavailable" : value, 14) + L" " + readings[i].note + L"\r\n");
    }

    std::wstring error;
    Say(out, L"\r\n");
    if (AppendCsv(machine, readings, &error))
        Say(out, L"appended to " + ResultsPath() + L"\r\n");
    else
        Say(out, L"CSV not written: " + error + L"\r\n");
    Say(out, std::wstring(L"\r\n") + kCopyright + L"\r\n");

    CloseConsole(out);
    return 0;
}

// --empty: the console equivalent of the reader's Empty file, for a scheduled
// task. It keeps the header, prints how many runs went and where the file is,
// and reads the result back so the line printed is what the file now holds. A
// failure exits 1, so the task's last result is worth something.
int RunEmpty() {
    const HANDLE out = OpenConsole();
    int          code = 0;

    const std::wstring path = ResultsPath();
    std::wstring       before;
    const size_t       runs = LoadHistory(&before).size();

    std::wstring error;
    if (!EmptyCsv(&error)) {
        Say(out, L"results.csv was not emptied: " + error + L"\r\n");
        code = 1;
    } else {
        std::wstring after;
        const bool headerKept = LoadHistory(&after).empty() &&
                                after == L"results.csv holds a header and no runs.";
        Say(out, runs == 0 ? std::wstring(L"results.csv was already empty")
                           : L"results.csv emptied: " + std::to_wstring(runs) +
                                 (runs == 1 ? L" run removed" : L" runs removed"));
        Say(out, headerKept ? L", header kept.\r\n" : L", but the header is missing.\r\n");
        Say(out, L"  " + path + L"\r\n");
        if (!headerKept) code = 1;
    }

    Say(out, std::wstring(L"\r\n") + kCopyright + L"\r\n");
    CloseConsole(out);
    return code;
}

int RunSelfTest() {
    const HANDLE out = OpenConsole();
    int          passed = 0;
    int          failed = 0;
    for (const TestResult& result : SelfTests()) {
        Say(out, std::wstring(result.pass ? L"  [PASS] " : L"  [FAIL] ") + result.name + L" - " +
                     result.detail + L"\r\n");
        if (result.pass) ++passed;
        else ++failed;
    }
    Say(out, L"\r\n  RESULT: " + std::to_wstring(passed) + L" passed, " +
                 std::to_wstring(failed) + L" failed.\r\n");
    Say(out, std::wstring(L"\r\n  ") + kCopyright + L"\r\n");
    CloseConsole(out);
    return failed == 0 ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int    argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode;
    if (argv && argc > 1) mode = argv[1];
    if (argv) LocalFree(argv);

    if (mode == L"--selftest") return RunSelfTest();
    if (mode == L"--run") return RunConsole();
    if (mode == L"--empty") return RunEmpty();
    return RunGui(instance);
}
