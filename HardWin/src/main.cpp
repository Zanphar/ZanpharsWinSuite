// ============================================================================
//  main.cpp — the HardWin window, and the two command-line modes a build or a
//  support session can use: --audit prints what every check reads without
//  changing anything, --selftest proves the catalog and the journal without
//  touching a Windows setting.
//
//  The window is twelve rows in the house palette: the setting's name, what it
//  buys and whether this machine has it. A row toggles on a click; the footer
//  hardens or reverts everything the catalog covers.
// ============================================================================

#include "harden.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include <cstddef>

using namespace hw;

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

const wchar_t* kClassName = L"HardWinWindow";
const wchar_t* kInstanceMutex = L"Local\\HardWin.SingleInstance";

UINT  g_dpi = 96;
HFONT g_fontTitle = nullptr;
HFONT g_fontBody = nullptr;
HFONT g_fontBodyBold = nullptr;
HFONT g_fontSmall = nullptr;

int S(int logical) { return MulDiv(logical, static_cast<int>(g_dpi), 96); }

HFONT MakeFont(int points, int weight) {
    return CreateFontW(-MulDiv(points, static_cast<int>(g_dpi), 72), 0, 0, 0, weight, FALSE,
                       FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void BuildFonts() {
    for (HFONT* font : {&g_fontTitle, &g_fontBody, &g_fontBodyBold, &g_fontSmall}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
    g_fontTitle = MakeFont(17, FW_SEMIBOLD);
    g_fontBody = MakeFont(9, FW_NORMAL);
    g_fontBodyBold = MakeFont(9, FW_SEMIBOLD);
    g_fontSmall = MakeFont(8, FW_NORMAL);
}

UINT QueryDpi(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    if (auto fn = reinterpret_cast<GetDpiForWindowFn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")))) {
        if (const UINT dpi = fn(window)) return dpi;
    }
    return 96;
}

// ---------------------------------------------------------------------------
//  Window state and layout
// ---------------------------------------------------------------------------

struct Layout {
    RECT rows[16] = {};
    int  rowCount = 0;
    RECT groupHeader[3] = {};
    int  groupCount = 0;
    // The row list is a viewport, not the whole window: it scrolls when the
    // screen is too short for twelve rows at this DPI, which is exactly what a
    // 768-pixel display at 150 % is (about 485 logical pixels of work area).
    int  listTop = 0;
    int  listBottom = 0;
    int  noteTop = 0;
    int  contentHeight = 0;
    int  maxScroll = 0;
    int  rowAdvance = 0;
    RECT scrollBar = {};
    RECT scrollThumb = {};
    RECT btnHarden = {};
    RECT btnRevert = {};
    RECT btnRecheck = {};
    RECT btnElevate = {};
    RECT linkSelectAll = {};
    RECT linkClear = {};
    bool hasElevate = false;
};

struct App {
    Engine             engine;
    std::vector<State> states;
    std::vector<char>  selected;   // one tick per check; the bulk buttons act here
    int                scroll = 0; // pixels the row list is scrolled up
    std::wstring       message;
    bool               elevated = false;
    int                hoverRow = -1;
    int                hotButton = 0;   // 1 harden, 2 revert, 3 re-check, 4 elevate,
                                        // 5 select all, 6 clear ticks
    bool               firstRefresh = true;
    Layout             layout;
};

App* g_app = nullptr;

// ---------------------------------------------------------------------------
//  Drawing primitives
// ---------------------------------------------------------------------------

void Fill(HDC dc, const RECT& rect, COLORREF colour, int radius) {
    HBRUSH brush = CreateSolidBrush(colour);
    HPEN pen = CreatePen(PS_SOLID, 1, colour);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void Stroke(HDC dc, const RECT& rect, COLORREF colour, int radius) {
    HPEN pen = CreatePen(PS_SOLID, 1, colour);
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

const wchar_t* StateWord(const State& state) {
    if (state.hardened) return L"Hardened";
    if (state.needsAdmin) return L"needs administrator";
    return L"Open";
}

COLORREF StateColour(const State& state) {
    if (state.hardened) return kOk;
    if (state.needsAdmin) return kTextDim;
    return kWarn;
}

// ---------------------------------------------------------------------------
//  Layout, hit testing
// ---------------------------------------------------------------------------

void BuildLayout(HWND window) {
    RECT client = {};
    GetClientRect(window, &client);
    Layout layout;

    // Header block: title, subtitle and the status line with its two links.
    layout.listTop = S(80);

    const int buttonHeight = S(32);
    const int buttonTop = client.bottom - S(16) - buttonHeight;
    // The footer note sits between the list and the buttons, and the list ends
    // well above it, so a short window never paints a row over the controls.
    layout.noteTop = buttonTop - S(20);
    const int availableBottom = layout.noteTop - S(8);

    const int rowHeight = S(34);
    const int rowAdvance = rowHeight + S(2);
    const int headerHeight = S(16);
    const int headerAdvance = headerHeight + S(2);
    layout.rowAdvance = rowAdvance;

    // Content height first, so the scroll position can be clamped and snapped
    // to a whole row before any rectangle is placed.
    int contentHeight = 0;
    const auto& catalog = Checks();
    for (size_t i = 0; i < catalog.size(); ++i) {
        if (i == 0 || catalog[i].group != catalog[i - 1].group) contentHeight += headerAdvance;
        contentHeight += rowAdvance;
    }
    layout.contentHeight = contentHeight;

    // The viewport ends on a row's bottom edge: the list never leaves half a
    // card hanging over the footer note. Rows are counted by walking the same
    // sequence the drawing loop walks, because a group heading costs height too
    // - dividing the space by the row pitch alone rounds away a row that does
    // fit, which is a visible empty band above the footer at 150 % in a short
    // window.
    const int available = availableBottom - layout.listTop;
    int viewport = 0;
    for (size_t i = 0; i < catalog.size(); ++i) {
        int bottom = viewport;
        if (i == 0 || catalog[i].group != catalog[i - 1].group) bottom += headerAdvance;
        bottom += rowAdvance;
        if (bottom - S(2) > available) break;
        viewport = bottom;
    }
    if (viewport == 0) viewport = rowAdvance - S(2);   // at least one row, however short
    else viewport -= S(2);
    layout.listBottom = layout.listTop + viewport;

    // One extra row of travel keeps the last row clear of the bottom edge when
    // the list is scrolled all the way down - but only when there is genuinely
    // more list than window, or a window that shows every row would offer a
    // scrollbar that has nowhere to go.
    const int extra = contentHeight + rowAdvance - viewport;
    layout.maxScroll =
        contentHeight > viewport && extra > 0
            ? ((extra + rowAdvance - 1) / rowAdvance) * rowAdvance
            : 0;
    if (g_app->scroll > layout.maxScroll) g_app->scroll = layout.maxScroll;
    if (g_app->scroll < 0) g_app->scroll = 0;
    if (layout.rowAdvance > 0) g_app->scroll = (g_app->scroll / rowAdvance) * rowAdvance;

    int y = layout.listTop - g_app->scroll;
    for (size_t i = 0; i < catalog.size() && layout.rowCount < 16; ++i) {
        const bool firstGroup = i == 0 || catalog[i].group != catalog[i - 1].group;
        if (firstGroup) {
            RECT header = {S(26), y, client.right - S(34), y + headerHeight};
            layout.groupHeader[layout.groupCount++] = header;
            y += headerAdvance;
        }
        RECT row = {S(26), y, client.right - S(34), y + rowHeight};
        layout.rows[layout.rowCount++] = row;
        y += rowAdvance;
    }

    if (layout.maxScroll > 0) {
        layout.scrollBar.left = client.right - S(18);
        layout.scrollBar.right = layout.scrollBar.left + S(4);
        layout.scrollBar.top = layout.listTop;
        layout.scrollBar.bottom = layout.listBottom;
        const int track = layout.scrollBar.bottom - layout.scrollBar.top;
        int thumb = MulDiv(track, viewport, contentHeight);
        if (thumb < S(24)) thumb = S(24);
        const int travel = track - thumb;
        const int offset = MulDiv(travel, g_app->scroll, layout.maxScroll);
        layout.scrollThumb.left = layout.scrollBar.left;
        layout.scrollThumb.right = layout.scrollBar.right;
        layout.scrollThumb.top = layout.scrollBar.top + offset;
        layout.scrollThumb.bottom = layout.scrollThumb.top + thumb;
    }

    int right = client.right - S(26);
    auto place = [&](int width, RECT* out) {
        out->right = right;
        out->left = right - S(width);
        out->top = buttonTop;
        out->bottom = buttonTop + buttonHeight;
        right = out->left - S(10);
    };
    place(130, &layout.btnHarden);
    place(110, &layout.btnRevert);
    place(94, &layout.btnRecheck);

    layout.hasElevate = !IsElevated();
    if (layout.hasElevate) {
        layout.btnElevate.left = S(26);
        layout.btnElevate.top = buttonTop;
        layout.btnElevate.right = S(26) + S(198);
        layout.btnElevate.bottom = buttonTop + buttonHeight;
    }

    // Select all / Clear sit at the right end of the status line, the way the
    // suite's cleanup page carries its own bulk links.
    layout.linkClear.left = client.right - S(26) - S(44);
    layout.linkClear.top = S(58);
    layout.linkClear.right = client.right - S(26);
    layout.linkClear.bottom = S(58) + S(18);
    layout.linkSelectAll.left = layout.linkClear.left - S(14) - S(68);
    layout.linkSelectAll.top = layout.linkClear.top;
    layout.linkSelectAll.right = layout.linkClear.left - S(14);
    layout.linkSelectAll.bottom = layout.linkClear.bottom;

    g_app->layout = layout;
}

bool Inside(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
           point.y < rect.bottom;
}

int HitRow(POINT point) {
    // A row scrolled out of the viewport is not clickable, so a press near the
    // footer can never reach a row that is only partly drawn or not drawn at all.
    if (point.y < g_app->layout.listTop || point.y >= g_app->layout.listBottom) return -1;
    for (int i = 0; i < g_app->layout.rowCount; ++i)
        if (Inside(g_app->layout.rows[i], point)) return i;
    return -1;
}

int HitButton(POINT point) {
    if (Inside(g_app->layout.btnHarden, point)) return 1;
    if (Inside(g_app->layout.btnRevert, point)) return 2;
    if (Inside(g_app->layout.btnRecheck, point)) return 3;
    if (g_app->layout.hasElevate && Inside(g_app->layout.btnElevate, point)) return 4;
    if (Inside(g_app->layout.linkSelectAll, point)) return 5;
    if (Inside(g_app->layout.linkClear, point)) return 6;
    return 0;
}

// ---------------------------------------------------------------------------
//  Painting
// ---------------------------------------------------------------------------

// The tick box that makes a row part of the selection. Bulk buttons only ever
// touch rows carrying one, so a press can never reach further than the list
// shows as ticked.
void PaintTick(HDC dc, const RECT& rect, bool ticked) {
    RECT box = {rect.left + S(12), rect.top + S(9), rect.left + S(12) + S(14),
                rect.top + S(9) + S(14)};
    if (!ticked) {
        Fill(dc, box, kBackground, S(4));
        Stroke(dc, box, kStroke, S(4));
        return;
    }
    Fill(dc, box, kAccent, S(4));
    HPEN pen = CreatePen(PS_SOLID, S(2), kOnAccent);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, box.left + S(3), box.top + S(7), nullptr);
    LineTo(dc, box.left + S(6), box.top + S(10));
    LineTo(dc, box.left + S(11), box.top + S(4));
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

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
    HDC dc = BeginPaint(window, &ps);

    RECT client = {};
    GetClientRect(window, &client);
    HDC buffer = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);

    HBRUSH background = CreateSolidBrush(kBackground);
    FillRect(buffer, &client, background);
    DeleteObject(background);

    const auto& catalog = Checks();
    const bool ready = g_app->states.size() == catalog.size();
    int hardened = 0;
    for (const State& state : g_app->states)
        if (state.hardened) ++hardened;
    const int total = static_cast<int>(catalog.size());

    RECT title = {S(26), S(18), client.right - S(26), S(18) + S(26)};
    PutText(buffer, title, L"HardWin", g_fontTitle, kText, DT_LEFT | DT_SINGLELINE);

    RECT subtitle = {S(26), S(44), client.right - S(26), S(44) + S(16)};
    PutText(buffer, subtitle,
            L"Hardening, tuning and recovery for Windows 10 and 11 - every change "
            L"journalled, every change reversible.",
            g_fontSmall, kTextDim, DT_LEFT | DT_SINGLELINE);

    int ticked = 0;
    for (const char on : g_app->selected)
        if (on) ++ticked;

    RECT status = {S(26), S(58), g_app->layout.linkSelectAll.left - S(20), S(58) + S(18)};
    std::wstring summary = ready ? std::to_wstring(hardened) + L" of " + std::to_wstring(total) +
                                       L" hardened   -   " + std::to_wstring(ticked) +
                                       L" selected"
                                 : L"reading the machine...";
    if (ready && !g_app->elevated)
        summary += L"   -   standard user: the administrator checks stay open until you elevate";
    PutText(buffer, status, summary, g_fontBody,
            ready && hardened == total ? kOk : kTextDim,
            DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    PutText(buffer, g_app->layout.linkSelectAll, L"Select all", g_fontSmall,
            g_app->hotButton == 5 ? kText : kTextDim, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    PutText(buffer, g_app->layout.linkClear, L"Clear", g_fontSmall,
            g_app->hotButton == 6 ? kText : kTextDim, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    // Rows and their group headers are clipped to the viewport, so a short
    // window shows fewer rows rather than drawing over the footer.
    HRGN clip = CreateRectRgn(S(20), g_app->layout.listTop, client.right - S(14),
                              g_app->layout.listBottom);
    SelectClipRgn(buffer, clip);

    for (int i = 0; ready && i < g_app->layout.rowCount; ++i) {
        const Check& check = catalog[i];
        const State& state = g_app->states[i];
        const RECT& rect = g_app->layout.rows[i];
        // A sliver of a row at the viewport edge reads as a broken card, so a
        // row is drawn only once at least half of it is in view.
        const int visible = (rect.bottom < g_app->layout.listBottom ? rect.bottom
                                                                   : g_app->layout.listBottom) -
                            (rect.top > g_app->layout.listTop ? rect.top : g_app->layout.listTop);
        if (visible * 2 < rect.bottom - rect.top) continue;

        Fill(buffer, rect, i == g_app->hoverRow ? kCardHover : kCard, S(8));
        Stroke(buffer, rect, g_app->selected[i] ? kAccent : kStroke, S(8));
        PaintTick(buffer, rect, g_app->selected[i] != 0);

        RECT name = {rect.left + S(36), rect.top + S(2), rect.right - S(180), rect.top + S(19)};
        PutText(buffer, name, check.name, g_fontBodyBold, kText,
                DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT why = {rect.left + S(36), rect.top + S(19), rect.right - S(180), rect.bottom - S(2)};
        PutText(buffer, why, check.why, g_fontSmall, kTextDim,
                DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT value = {rect.right - S(160), rect.top + S(2), rect.right - S(14), rect.top + S(19)};
        PutText(buffer, value, state.current, g_fontSmall, kTextDim,
                DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT word = {rect.right - S(160), rect.top + S(19), rect.right - S(14),
                     rect.bottom - S(2)};
        PutText(buffer, word, StateWord(state), g_fontSmall, StateColour(state),
                DT_RIGHT | DT_SINGLELINE);
    }

    int groupIndex = -1;
    for (int i = 0; ready && i < g_app->layout.rowCount; ++i) {
        if (i == 0 || catalog[i].group != catalog[i - 1].group) {
            ++groupIndex;
            if (groupIndex >= g_app->layout.groupCount) break;
            const RECT& rect = g_app->layout.groupHeader[groupIndex];
            // A heading whose first row did not make it into the viewport would
            // hang over the footer with nothing under it.
            const RECT& first = g_app->layout.rows[i];
            const int rowVisible =
                (first.bottom < g_app->layout.listBottom ? first.bottom
                                                         : g_app->layout.listBottom) -
                (first.top > g_app->layout.listTop ? first.top : g_app->layout.listTop);
            if (rect.bottom <= g_app->layout.listTop || rowVisible * 2 < first.bottom - first.top)
                continue;
            PutText(buffer, rect, GroupName(catalog[i].group), g_fontSmall, kTextDim,
                    DT_LEFT | DT_SINGLELINE | DT_BOTTOM);
        }
    }

    SelectClipRgn(buffer, nullptr);
    DeleteObject(clip);

    // A slim bar on the right says there is more of the list than the window
    // has room for, the same cue the suite's other scrolling pages use.
    if (g_app->layout.maxScroll > 0) {
        Fill(buffer, g_app->layout.scrollBar, kCard, S(4));
        Fill(buffer, g_app->layout.scrollThumb, kStroke, S(4));
    }

    RECT note = {S(26), g_app->layout.noteTop, client.right - S(26),
                 g_app->layout.noteTop + S(16)};
    const std::wstring footer =
        g_app->message.empty()
            ? L"Tick the rows you want, then harden or revert the selection. Nothing is "
              L"written without the original value being saved first."
            : g_app->message;
    PutText(buffer, note, footer, g_fontSmall, g_app->message.empty() ? kTextDim : kText,
            DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    PaintButton(buffer, g_app->layout.btnHarden, L"Harden selected", true,
                g_app->hotButton == 1);
    PaintButton(buffer, g_app->layout.btnRevert, L"Revert selected", false,
                g_app->hotButton == 2);
    PaintButton(buffer, g_app->layout.btnRecheck, L"Re-check", false, g_app->hotButton == 3);
    if (g_app->layout.hasElevate)
        PaintButton(buffer, g_app->layout.btnElevate, L"Restart as administrator", false,
                    g_app->hotButton == 4);

    BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    EndPaint(window, &ps);
}

// ---------------------------------------------------------------------------
//  Actions
// ---------------------------------------------------------------------------

void Refresh(HWND window) {
    g_app->states = g_app->engine.AuditAll();
    g_app->elevated = IsElevated();
    // Ticks belong to the catalog, not to a run of the audit: a re-read keeps
    // whatever was ticked, and a catalog that grew starts the new rows unticked.
    if (g_app->selected.size() != Checks().size())
        g_app->selected.resize(Checks().size(), 0);

    // A row with a saved original that no longer reads hardened has been
    // changed back by something else since this tool set it. Saying so on the
    // first read is the difference between a silent drift and a known one.
    if (g_app->firstRefresh) {
        g_app->firstRefresh = false;
        int drifted = 0;
        for (const State& state : g_app->states)
            if (state.journalled && !state.hardened) ++drifted;
        if (drifted > 0)
            g_app->message = std::to_wstring(drifted) +
                             (drifted == 1 ? L" row has" : L" rows have") +
                             L" a saved original but no longer reads hardened - tick it and "
                             L"press Revert selected to put the original back.";
    }

    BuildLayout(window);
    InvalidateRect(window, nullptr, FALSE);
}

std::vector<int> Ticked() {
    std::vector<int> picked;
    for (size_t i = 0; i < g_app->selected.size(); ++i)
        if (g_app->selected[i]) picked.push_back(static_cast<int>(i));
    return picked;
}

std::wstring CountText(size_t count, const wchar_t* singular, const wchar_t* plural) {
    return std::to_wstring(count) + L" " + (count == 1 ? singular : plural);
}

// The ticked checks, in catalog order, for a batch call into the engine.
std::vector<const Check*> TickedChecks(const std::vector<int>& picked) {
    const auto& catalog = Checks();
    std::vector<const Check*> checks;
    checks.reserve(picked.size());
    for (const int index : picked) checks.push_back(&catalog[index]);
    return checks;
}

// Hardens exactly the ticked rows. The skipping and the refusal list come from
// Engine::ApplyChecks, so this function only words the result.
void HardenSelection(HWND window) {
    const std::vector<int> picked = Ticked();
    if (picked.empty()) {
        g_app->message = L"Tick the rows you want to harden first.";
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    const Engine::Batch batch = g_app->engine.ApplyChecks(TickedChecks(picked));
    std::wstring message = L"Hardened " + std::to_wstring(batch.changed) + L" of " +
                           CountText(picked.size(), L"selected row", L"selected rows") + L".";
    if (batch.skipped)
        message += L" " + std::to_wstring(batch.skipped) +
                   (batch.skipped == 1 ? L" was already hardened." : L" were already hardened.");
    if (!batch.refused.empty()) message += L" Refused:" + batch.refused;
    g_app->message = message;
    Refresh(window);
}

// Puts back exactly the ticked rows that have something saved. A row with no
// journal entry is a skip, and that rule lives in Engine::RevertChecks.
void RevertSelection(HWND window) {
    const std::vector<int> picked = Ticked();
    if (picked.empty()) {
        g_app->message = L"Tick the rows you want to put back first.";
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    const Engine::Batch batch = g_app->engine.RevertChecks(TickedChecks(picked));
    std::wstring message = L"Put back " + std::to_wstring(batch.changed) + L" of " +
                           CountText(picked.size(), L"selected row", L"selected rows") + L".";
    if (batch.skipped)
        message += L" " + std::to_wstring(batch.skipped) + L" had no saved value to restore.";
    if (!batch.refused.empty()) message += L" Refused:" + batch.refused;
    g_app->message = message;
    Refresh(window);
}

// The window may never grow past the work area of the monitor it sits on: at
// 150 % on a 768-pixel display the work area is about 485 logical pixels, and a
// fixed 690-pixel window would simply hang off the bottom of the screen.
void ClampToWorkArea(HWND window, int* width, int* height) {
    MONITORINFO info = {};
    info.cbSize = sizeof info;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    RECT work = {};
    if (!GetMonitorInfoW(monitor, &info)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    else work = info.rcWork;
    const int maxWidth = work.right - work.left - S(8);
    const int maxHeight = work.bottom - work.top - S(8);
    if (*width > maxWidth) *width = maxWidth;
    if (*height > maxHeight) *height = maxHeight;
    if (*width < S(620)) *width = S(620);
    if (*height < S(430)) *height = S(430);
}

// The client height the whole catalog needs at this DPI: header block, every
// row and group heading, one row of slack so the list can end on a row's bottom
// edge, and the footer. A tall screen gets a window that shows everything; a
// short one is clamped to the work area and scrolls instead.
int DesiredClientHeight() {
    int content = 0;
    const auto& catalog = Checks();
    for (size_t i = 0; i < catalog.size(); ++i) {
        if (i == 0 || catalog[i].group != catalog[i - 1].group) content += S(18) + S(2);
        content += S(34) + S(2);
    }
    return S(80) + content + S(36) + S(76);
}

// Grow a desired client rectangle into the frame that yields it at this DPI.
// AdjustWindowRectEx measures the caption and borders at the system DPI, so on
// a 125 % or 150 % monitor it under-reports the frame and the client comes up
// short by the difference - which is exactly a row of the list.
void AdjustFrameForDpi(RECT* frame) {
    using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto adjustForDpi = reinterpret_cast<AdjustForDpiFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi")));
    if (adjustForDpi && adjustForDpi(frame, WS_OVERLAPPEDWINDOW, FALSE, 0, g_dpi)) return;

    AdjustWindowRectEx(frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
    if (g_dpi != 96) {
        frame->left = MulDiv(frame->left, static_cast<int>(g_dpi), 96);
        frame->top = MulDiv(frame->top, static_cast<int>(g_dpi), 96);
        frame->right = MulDiv(frame->right, static_cast<int>(g_dpi), 96);
        frame->bottom = MulDiv(frame->bottom, static_cast<int>(g_dpi), 96);
    }
}

// The frame that shows the whole catalog at the current DPI: 940 logical pixels
// wide and tall enough for header, every row, every group heading and the
// footer, capped by the work area of the monitor the window is on. Used both at
// startup and when the window crosses onto a monitor with a different scale, so
// there is one answer to "how big should this window be".
void DesiredFrame(HWND window, int* width, int* height) {
    RECT frame = {0, 0, S(940), DesiredClientHeight()};
    AdjustFrameForDpi(&frame);
    *width = frame.right - frame.left;
    *height = frame.bottom - frame.top;
    ClampToWorkArea(window, width, height);
}

// The size the window opens at. The list scrolls rather than the window growing
// past the screen.
void ApplyDesiredSize(HWND window) {
    int width = 0;
    int height = 0;
    DesiredFrame(window, &width, &height);
    RECT current = {};
    GetWindowRect(window, &current);
    SetWindowPos(window, nullptr, current.left, current.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
//  The elevation hand-off
// ---------------------------------------------------------------------------

// The elevated copy is a new process holding a fresh, empty tick list, so the
// rows the user staged travel on its command line as "--apply=id,id". The ids
// are the catalog's stable keys - the same ones the journal is keyed on - so a
// staged row keeps its meaning even if a later build reorders or grows the
// catalog. Anything this build cannot name is dropped rather than guessed at.
const wchar_t* kApplySwitch = L"--apply=";

std::wstring ApplyArguments(const std::vector<const Check*>& checks) {
    std::wstring arguments = kApplySwitch;
    for (size_t i = 0; i < checks.size(); ++i) {
        if (i) arguments.push_back(L',');
        arguments += checks[i]->id;
    }
    return arguments;
}

std::vector<std::wstring> ParseApplyIds(const std::wstring& argument) {
    const std::wstring prefix = kApplySwitch;
    std::vector<std::wstring> ids;
    if (argument.size() < prefix.size() || argument.compare(0, prefix.size(), prefix) != 0)
        return ids;
    std::wstring id;
    for (size_t i = prefix.size(); i <= argument.size(); ++i) {
        if (i == argument.size() || argument[i] == L',') {
            if (!id.empty()) ids.push_back(id);
            id.clear();
            continue;
        }
        id.push_back(argument[i]);
    }
    return ids;
}

// The catalog rows those ids name, so the receiving window can tick exactly
// what the sending window had ticked.
std::vector<int> RowsForIds(const std::vector<std::wstring>& ids) {
    std::vector<int> rows;
    const auto& catalog = Checks();
    for (const std::wstring& id : ids) {
        for (size_t i = 0; i < catalog.size(); ++i) {
            if (id == catalog[i].id) {
                rows.push_back(static_cast<int>(i));
                break;
            }
        }
    }
    return rows;
}

// Hand the ticked rows to an elevated copy and get out of its way. Nothing
// ticked is a plain restart, which is all this button used to be able to do.
void RelaunchElevated(HWND window) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    const std::vector<int> picked = Ticked();
    const std::wstring parameters =
        picked.empty() ? std::wstring() : ApplyArguments(TickedChecks(picked));

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof info;
    info.lpVerb = L"runas";
    info.lpFile = exe;
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) {
        DestroyWindow(window);
        return;
    }
    g_app->message =
        L"The administrator prompt was declined, so the checks that need it stay open.";
    InvalidateRect(window, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            // The DPI is only knowable once there is a window on a monitor, so
            // the size the caller passed in was measured at 96. Re-fit here,
            // before the first paint, and RunGui fits it again once this has
            // run so nothing is ever shown at the placeholder size.
            g_dpi = QueryDpi(window);
            BuildFonts();
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark,
                                  sizeof dark);
            Refresh(window);
            ApplyDesiredSize(window);
            return 0;
        }
        case WM_DPICHANGED: {
            // The window has landed on a monitor with a different scale. Height
            // is worked out from the catalog at the new DPI rather than by
            // growing the old frame: scaling preserves whatever the user had
            // dragged the window to, which is how a window ends up showing half
            // a row at 150 % or wasting a hand's width of empty list at 100 %.
            // Then the new monitor's work area caps it, and if the cap bites
            // the row list scrolls - whole rows, as everywhere else.
            g_dpi = HIWORD(wParam);
            BuildFonts();
            int width = 0;
            int height = 0;
            DesiredFrame(window, &width, &height);
            // The suggested rectangle carries the position Windows wants; it is
            // optional so the message can also be driven without one.
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            RECT current = {};
            GetWindowRect(window, &current);
            const int x = suggested ? suggested->left : current.left;
            const int y = suggested ? suggested->top : current.top;
            SetWindowPos(window, nullptr, x, y, width, height,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            BuildLayout(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = S(620);
            info->ptMinTrackSize.y = S(430);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (g_app->layout.maxScroll <= 0) return 0;
            // One notch is one row, so the list never stops between two cards.
            const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            g_app->scroll -= notches * g_app->layout.rowAdvance;
            BuildLayout(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT:
            Paint(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (g_app) BuildLayout(window);
            return 0;
        case WM_MOUSEMOVE: {
            const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int row = HitRow(point);
            const int button = HitButton(point);
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
            const int row = HitRow(point);
            if (row >= 0) {
                if (row < static_cast<int>(g_app->selected.size())) {
                    g_app->selected[row] = g_app->selected[row] ? 0 : 1;
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            }
            switch (HitButton(point)) {
                case 1: HardenSelection(window); return 0;
                case 2: RevertSelection(window); return 0;
                case 3:
                    g_app->message = L"Read again.";
                    Refresh(window);
                    return 0;
                case 4: RelaunchElevated(window); return 0;
                case 5:
                    g_app->selected.assign(g_app->selected.size(), 1);
                    g_app->message = L"Every row ticked.";
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                case 6:
                    g_app->selected.assign(g_app->selected.size(), 0);
                    g_app->message = L"Ticks cleared.";
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                default: break;
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  Console modes
// ---------------------------------------------------------------------------

// The console modes are launched from cmd.exe, from a build script and from a
// pipe equally often, so the handle they write to is whatever stdout already
// is. A console needs wide characters; a pipe or a file needs UTF-8 bytes.
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
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr,
                                          nullptr);
    if (bytes <= 0) return;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(),
                        bytes, nullptr, nullptr);
    WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void CloseConsole(HANDLE out) {
    // stdout belongs to whoever launched the program; only a console this mode
    // opened for itself is closed again.
    if (out && out != INVALID_HANDLE_VALUE && out != GetStdHandle(STD_OUTPUT_HANDLE))
        CloseHandle(out);
}

std::wstring Pad(const std::wstring& text, size_t width) {
    std::wstring out = text;
    while (out.size() < width) out.push_back(L' ');
    return out;
}

int AuditMode() {
    const HANDLE out = OpenConsole();
    Engine engine;
    const auto& catalog = Checks();
    const std::vector<State> states = engine.AuditAll();

    Say(out, L"\r\nHardWin - hardening audit (read only; nothing was changed)\r\n");
    Say(out, IsElevated() ? L"  running as administrator\r\n"
                          : L"  running as a standard user; the HKLM rows need elevation\r\n");

    Group last = Group::Security;
    int hardened = 0;
    for (size_t i = 0; i < catalog.size(); ++i) {
        if (i == 0 || catalog[i].group != last) {
            Say(out, L"\r\n  " + std::wstring(GroupName(catalog[i].group)) + L"\r\n");
            last = catalog[i].group;
        }
        if (states[i].hardened) ++hardened;
        const std::wstring word = states[i].hardened   ? L"[ HARDENED ]"
                                  : states[i].needsAdmin ? L"[  ADMIN   ]"
                                                         : L"[   OPEN   ]";
        Say(out, L"    " + word + L"  " + Pad(catalog[i].name, 40) + L"  " + states[i].current +
                     L"\r\n");
    }
    Say(out, L"\r\n  " + std::to_wstring(hardened) + L" of " + std::to_wstring(catalog.size()) +
                  L" hardened.\r\n");
    CloseConsole(out);
    return 0;
}

// Registers the window class and hands back the icons it loaded, so the window
// and the self-test's throwaway window are the same class.
bool RegisterMainClass(HINSTANCE instance, HICON* big, HICON* small) {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof windowClass;
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kClassName;
    // The icon embedded by app.rc: a padlock behind the initials. The default
    // application icon is only a fallback for a build whose resource failed.
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(2));
    if (!windowClass.hIcon) windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(2),
                                                       IMAGE_ICON, S(16), S(16),
                                                       LR_DEFAULTCOLOR));
    if (!windowClass.hIconSm) windowClass.hIconSm = windowClass.hIcon;
    if (!RegisterClassExW(&windowClass)) return false;
    *big = windowClass.hIcon;
    *small = windowClass.hIconSm;
    return true;
}

// The window's own DPI handling, driven in-process: a hidden window of the real
// class is given two different starting heights and told, in between, that its
// monitor runs at another scale. Scaling the old frame makes the answer depend
// on whichever height the test happened to pick; refitting from the catalog
// leaves one answer per scale, which is what this asserts. Nothing is shown and
// no display setting is touched.
std::vector<TestResult> UiSelfTests(HINSTANCE instance) {
    std::vector<TestResult> results;
    const wchar_t* name = L"a DPI change refits the window from the catalog";

    HICON big = nullptr;
    HICON small = nullptr;
    if (!RegisterMainClass(instance, &big, &small)) {
        results.push_back({name, Outcome::Fail, L"the window class would not register"});
        return results;
    }

    const bool had_app = g_app != nullptr;
    if (!g_app) g_app = new App();
    const UINT original_dpi = g_dpi;
    HWND window = CreateWindowExW(0, kClassName, L"HardWin self-test", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, nullptr, nullptr,
                                  instance, nullptr);
    if (!window) {
        results.push_back({name, Outcome::Fail, L"the test window would not open"});
        if (!had_app) {
            delete g_app;
            g_app = nullptr;
        }
        return results;
    }

    // Settle the window at `dpi` after first resizing it to `before` pixels tall.
    auto height_after = [&](UINT dpi, int before) {
        SetWindowPos(window, nullptr, 0, 0, 900, before,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT suggested = {};
        GetWindowRect(window, &suggested);
        SendMessageW(window, WM_DPICHANGED, MAKELONG(dpi, dpi),
                     reinterpret_cast<LPARAM>(&suggested));
        RECT after = {};
        GetWindowRect(window, &after);
        return static_cast<int>(after.bottom - after.top);
    };

    const int short96 = height_after(96, 520);
    const int tall96 = height_after(96, 900);
    const int short150 = height_after(144, 520);
    const int tall150 = height_after(144, 900);

    std::wstring failure;
    if (short96 != tall96)
        failure = L"at 100 % the height followed the previous size: " +
                  std::to_wstring(short96) + L" vs " + std::to_wstring(tall96);
    else if (short150 != tall150)
        failure = L"at 150 % the height followed the previous size: " +
                  std::to_wstring(short150) + L" vs " + std::to_wstring(tall150);
    else if (short96 == short150)
        failure = L"the scale changed and the height did not";
    else if (g_app->layout.rowAdvance <= 0 || g_app->layout.listBottom <= g_app->layout.listTop)
        failure = L"the list area collapsed after the change";

    DestroyWindow(window);
    g_dpi = original_dpi;
    if (!had_app) {
        delete g_app;
        g_app = nullptr;
    }

    results.push_back({name, failure.empty() ? Outcome::Pass : Outcome::Fail, failure});
    return results;
}

// The consent prompt creates a new process, and the command line is the only
// thing that crosses it: if the writer and the reader of that switch disagree,
// the elevated copy applies nothing and the user's ticks vanish silently. So the
// pair is round-tripped against the real catalog instead of trusted.
std::vector<TestResult> HandoffSelfTests() {
    std::vector<TestResult> results;
    const wchar_t* name = L"the elevation hand-off keeps the ticked rows";

    std::vector<const Check*> picked;
    for (const Check& check : Checks()) {
        if (picked.size() == 3) break;
        picked.push_back(&check);
    }

    const std::vector<std::wstring> ids = ParseApplyIds(ApplyArguments(picked));
    std::wstring failure;
    if (ids.size() != picked.size()) {
        failure = L"wrote " + std::to_wstring(picked.size()) + L" ids and read back " +
                  std::to_wstring(ids.size());
    } else {
        for (size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] != picked[i]->id) {
                failure = std::wstring(L"id ") + picked[i]->id + L" came back as " + ids[i];
                break;
            }
        }
    }
    if (failure.empty() && !ParseApplyIds(kApplySwitch).empty())
        failure = L"the switch with no ids produced rows";
    if (failure.empty() && RowsForIds(ids).size() != picked.size())
        failure = L"the ids did not map back onto catalog rows";
    if (failure.empty() && !RowsForIds({L"no-such-check"}).empty())
        failure = L"an unknown id was accepted";

    results.push_back({name, failure.empty() ? Outcome::Pass : Outcome::Fail, failure});
    return results;
}

int SelfTestMode(HINSTANCE instance) {
    const HANDLE out = OpenConsole();
    Say(out, L"\r\nHardWin self-test\r\n\r\n");
    int passed = 0, skipped = 0, failed = 0;
    std::vector<TestResult> results = RunSelfTest();
    for (TestResult result : HandoffSelfTests()) results.push_back(std::move(result));
    for (TestResult result : UiSelfTests(instance)) results.push_back(std::move(result));
    for (const TestResult& result : results) {
        if (result.outcome == Outcome::Pass) {
            ++passed;
            Say(out, std::wstring(L"  [PASS] ") + result.name + L"\r\n");
        } else if (result.outcome == Outcome::Skip) {
            ++skipped;
            Say(out, std::wstring(L"  [SKIP] ") + result.name +
                         (result.detail.empty() ? L"" : L" - " + result.detail) + L"\r\n");
        } else {
            ++failed;
            Say(out, std::wstring(L"  [FAIL] ") + result.name +
                         (result.detail.empty() ? L"" : L" - " + result.detail) + L"\r\n");
        }
    }
    Say(out, L"\r\n  RESULT: " + std::to_wstring(passed) + L" passed, " +
                  std::to_wstring(skipped) + L" skipped, " + std::to_wstring(failed) +
                  L" failed.\r\n");
    CloseConsole(out);
    return failed == 0 ? 0 : 1;
}

int HelpMode() {
    const HANDLE out = OpenConsole();
    Say(out, L"\r\nHardWin - hardening, tuning and recovery for Windows 10 and 11.\r\n\r\n"
             L"  HardWin.exe             open the window\r\n"
             L"  HardWin.exe --audit     print what every check reads, change nothing\r\n"
             L"  HardWin.exe --apply=id,id  tick those rows and harden them; the elevation\r\n"
             L"                             hand-off uses this, and nothing else does\r\n"
             L"  HardWin.exe --selftest  prove the catalog and the journal\r\n"
             L"  HardWin.exe --help      this text\r\n\r\n"
             L"  Journal\n"
             L"    HKLM\\SOFTWARE\\ZanpharsWinSuite\\HardWin\\journal   administrator checks\r\n"
             L"    HKCU\\SOFTWARE\\ZanpharsWinSuite\\HardWin\\journal   per-user checks\r\n"
             L"  Revert puts back whatever was there, including removing a value that was\r\n"
             L"  not there before.\r\n\r\n"
             L"  Every write, refusal and rollback is appended to\r\n"
             L"    %LOCALAPPDATA%\\HardWin\\hardwin.log\r\n");
    CloseConsole(out);
    return 0;
}

int RunGui(HINSTANCE instance, int showCommand, const std::vector<std::wstring>& staged) {
    // One window only. Two copies would interleave writes on the same journal
    // and each would audit a machine the other is changing underneath it.
    //
    // A launch that carries staged rows is the one exception: it is the elevated
    // copy of a window that is still on screen, because the parent only closes
    // itself after the consent prompt comes back - and by then the elevated
    // process is already starting. So that launch waits for the window it is
    // replacing instead of bailing out on the mutex, which is how the hand-off
    // used to end with nothing written and no window at all.
    const bool handoff = !staged.empty();
    HANDLE instance_lock = nullptr;
    for (int attempt = 0; attempt < 200 && !instance_lock; ++attempt) {
        HANDLE lock = CreateMutexW(nullptr, TRUE, kInstanceMutex);
        if (!lock) return 1;
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            instance_lock = lock;
            break;
        }
        CloseHandle(lock);
        if (!handoff) {
            if (HWND previous = FindWindowW(kClassName, nullptr)) {
                ShowWindow(previous, SW_RESTORE);
                SetForegroundWindow(previous);
            }
            return 0;
        }
        Sleep(100);
    }
    if (!instance_lock) {
        MessageBoxW(nullptr,
                    L"Another HardWin window stayed open, so the rows carried over from it "
                    L"were not applied. Close it and apply again.",
                    L"HardWin", MB_ICONINFORMATION | MB_OK);
        return 1;
    }

    HICON bigIcon = nullptr;
    HICON smallIcon = nullptr;
    if (!RegisterMainClass(instance, &bigIcon, &smallIcon)) return 1;

    g_app = new App();
    g_app->elevated = IsElevated();

    // The placeholder size is never seen: g_dpi is 96 until the window exists,
    // so any S() here is the wrong number on a scaled display. WM_CREATE fits
    // the window to its real DPI, and the fit below repeats it with the DPI
    // this thread now knows for certain, still before ShowWindow.
    HWND window = CreateWindowExW(0, kClassName, L"HardWin", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, nullptr, nullptr,
                                  instance, nullptr);
    if (!window) return 1;

    // Re-fit once more from the window's own monitor, not from a guess made
    // before it had one: this is the DPI every row will be laid out at.
    g_dpi = QueryDpi(window);
    BuildFonts();
    ApplyDesiredSize(window);

    // Centre it on the work area.
    RECT screen = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
    RECT size = {};
    GetWindowRect(window, &size);
    SetWindowPos(window, nullptr,
                 screen.left + ((screen.right - screen.left) - (size.right - size.left)) / 2,
                 screen.top + ((screen.bottom - screen.top) - (size.bottom - size.top)) / 2, 0,
                 0, SWP_NOSIZE | SWP_NOZORDER);

    // Both sizes are set on the window as well as the class, so the title bar
    // and the taskbar button carry the icon whatever the shell cached.
    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    // Tick the rows the previous window handed over, then run the same hardening
    // the footer runs: the skipping, the refusals and the wording all stay in
    // one place, and the window shows the result rather than a silent exit.
    if (!staged.empty()) {
        const std::vector<int> rows = RowsForIds(staged);
        for (const int row : rows) g_app->selected[row] = 1;
        if (rows.empty()) {
            g_app->message =
                L"Nothing from the previous window matches this catalog, so nothing was applied.";
            InvalidateRect(window, nullptr, FALSE);
        } else {
            HardenSelection(window);
        }
    }

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    delete g_app;
    g_app = nullptr;
    if (instance_lock) {
        ReleaseMutex(instance_lock);
        CloseHandle(instance_lock);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode;
    std::vector<std::wstring> staged;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (ParseApplyIds(arg).size() || arg == kApplySwitch) {
            staged = ParseApplyIds(arg);
            continue;
        }
        if (arg == L"--selftest" || arg == L"--audit" || arg == L"--help" || arg == L"-h") {
            mode = arg;
            break;
        }
    }
    if (argv) LocalFree(argv);

    if (mode == L"--selftest") return SelfTestMode(instance);
    if (mode == L"--audit") return AuditMode();
    if (mode == L"--help" || mode == L"-h") return HelpMode();
    return RunGui(instance, showCommand, staged);
}
