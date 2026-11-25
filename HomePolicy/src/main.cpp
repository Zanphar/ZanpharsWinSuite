// ============================================================================
//  main.cpp — the Home Policy window.
//
//  A left column of the categories the official editor uses, a searchable list
//  of policy cards, and three choices on each card: Not configured, Enabled,
//  Disabled. Choices are staged and only written when Apply changes is pressed,
//  so a beginner can look around without changing anything by accident.
//
//  The window is one custom-painted surface in the suite palette - no child
//  controls to fight over focus and colours - and every metric is scaled by the
//  monitor's DPI.
// ============================================================================

#include "catalog.h"

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace hp;

namespace {

// ---------------------------------------------------------------------------
//  Palette, like the rest of the suite
// ---------------------------------------------------------------------------

constexpr COLORREF kBackground = RGB(24, 25, 31);
constexpr COLORREF kCard = RGB(34, 36, 44);
constexpr COLORREF kCardHover = RGB(40, 42, 52);
constexpr COLORREF kCardActive = RGB(44, 47, 58);
constexpr COLORREF kStroke = RGB(57, 60, 71);
constexpr COLORREF kText = RGB(236, 238, 244);
constexpr COLORREF kTextDim = RGB(146, 152, 164);
constexpr COLORREF kTextFaint = RGB(104, 110, 124);

// Shown in the footer's bottom row, under a hairline rule.
constexpr wchar_t kCopyright[] = L"\u00A9 2026-2027 Charles McDonald";
constexpr COLORREF kAccent = RGB(84, 158, 255);
constexpr COLORREF kAccentDeep = RGB(52, 110, 196);
constexpr COLORREF kOnAccent = RGB(250, 252, 255);
constexpr COLORREF kOk = RGB(88, 205, 143);
constexpr COLORREF kWarn = RGB(242, 178, 84);
constexpr COLORREF kError = RGB(240, 120, 120);
constexpr COLORREF kDisabledFill = RGB(74, 80, 96);

const wchar_t* kClassName = L"HomePolicyWindow";

UINT  g_dpi = 96;
HFONT g_fontTitle = nullptr;
HFONT g_fontSubtitle = nullptr;
HFONT g_fontCardTitle = nullptr;
HFONT g_fontBody = nullptr;
HFONT g_fontBodyBold = nullptr;
HFONT g_fontSmall = nullptr;
HFONT g_fontTiny = nullptr;

int S(int logical) { return MulDiv(logical, static_cast<int>(g_dpi), 96); }

HFONT MakeFont(int points, int weight) {
    return CreateFontW(-MulDiv(points, static_cast<int>(g_dpi), 72), 0, 0, 0, weight, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void BuildFonts() {
    for (HFONT* font : {&g_fontTitle, &g_fontSubtitle, &g_fontCardTitle, &g_fontBody,
                        &g_fontBodyBold, &g_fontSmall, &g_fontTiny}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
    g_fontTitle = MakeFont(21, FW_SEMIBOLD);
    g_fontSubtitle = MakeFont(9, FW_NORMAL);
    g_fontCardTitle = MakeFont(10, FW_SEMIBOLD);
    g_fontBody = MakeFont(9, FW_NORMAL);
    g_fontBodyBold = MakeFont(9, FW_SEMIBOLD);
    g_fontSmall = MakeFont(8, FW_NORMAL);
    g_fontTiny = MakeFont(8, FW_SEMIBOLD);
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
//  Window state
// ---------------------------------------------------------------------------

struct CardView {
    int  policy = -1;
    bool tagged = false;
    RECT rect = {};
    RECT seg[3] = {};
};

struct Layout {
    RECT header = {};
    RECT sidebar = {};
    RECT content = {};
    RECT footer = {};
    RECT copyright = {};
    RECT searchBox = {};
    RECT category[9] = {};
    int  categoryCount = 0;
    RECT help = {};
    RECT cardArea = {};
    std::vector<CardView> cards;
    RECT btnBackup = {};
    RECT btnDiscard = {};
    RECT btnApply = {};
    RECT btnElevate = {};
    bool hasElevate = false;
    RECT track = {};
    RECT thumb = {};
    bool hasScroll = false;
    int  scrollMax = 0;
};

struct App {
    std::vector<Tri>  state;
    std::vector<char> pending;       // staged, not yet written
    std::vector<Tri>  pendingState;
    std::vector<int>  visible;       // policy indices for the current filter
    std::wstring      search;
    int               category = 0;
    int               scroll = 0;
    int               hoverCard = -1;
    int               hoverSegment = -1;
    int               hoverCategory = -1;
    int               hotButton = -1;    // 0 apply, 1 discard, 2 backup, 3 elevate
    bool              searchFocused = false;
    bool              caretVisible = false;
    bool              statusError = false;
    std::wstring      status;
    std::wstring      statusHint;
    bool              elevated = false;
    bool              draggingThumb = false;
    int               dragOffset = 0;
    Layout            layout;
};

App* g_app = nullptr;

// Choices staged in an unelevated window travel to the elevated copy here, as
// "index:state;index:state". Empty when the program was started normally.
// The flag is --staged: --apply is the one-setting console mode.
std::wstring g_staged;

// ---------------------------------------------------------------------------
//  Drawing primitives
// ---------------------------------------------------------------------------

void FillRound(HDC dc, const RECT& rect, COLORREF colour, int radius) {
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

void StrokeRound(HDC dc, const RECT& rect, COLORREF colour, int radius, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, colour);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void TextIn(HDC dc, RECT rect, const std::wstring& text, HFONT font, COLORREF colour, UINT flags) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, colour);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

void DrawButton(HDC dc, const RECT& rect, const std::wstring& label, bool primary, bool hot,
                bool enabled) {
    const int radius = S(8);
    COLORREF fill = primary ? (hot && enabled ? RGB(104, 172, 255) : kAccent)
                            : (hot && enabled ? kCardHover : kCard);
    if (!enabled) fill = RGB(38, 40, 48);
    FillRound(dc, rect, fill, radius);
    if (!primary) StrokeRound(dc, rect, kStroke, radius);
    TextIn(dc, rect, label, g_fontBodyBold, enabled ? (primary ? kOnAccent : kText) : kTextFaint,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawMagnifier(HDC dc, int x, int y, int size, COLORREF colour) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2)), colour);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int diameter = size - S(5);
    Ellipse(dc, x, y, x + diameter, y + diameter);
    MoveToEx(dc, x + diameter - S(2), y + diameter - S(2), nullptr);
    LineTo(dc, x + diameter + S(3), y + diameter + S(3));
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// ---------------------------------------------------------------------------
//  Filtering and pending changes
// ---------------------------------------------------------------------------

std::wstring Lower(std::wstring text) {
    for (wchar_t& ch : text) ch = static_cast<wchar_t>(towlower(ch));
    return text;
}

bool Matches(const Policy& policy, const std::wstring& needle) {
    if (needle.empty()) return true;
    const std::wstring haystack = Lower(std::wstring(policy.title) + L" " + policy.summary + L" " +
                                        PathText(policy));
    return haystack.find(needle) != std::wstring::npos;
}

void RebuildVisible(App& app) {
    app.visible.clear();
    const std::wstring needle = Lower(app.search);
    for (int i = 0; i < static_cast<int>(Policies().size()); ++i) {
        const Policy& policy = Policies()[static_cast<size_t>(i)];
        if (app.category != 0 && policy.category != app.category) continue;
        if (!Matches(policy, needle)) continue;
        app.visible.push_back(i);
    }
    app.scroll = 0;
}

int PendingCount(const App& app) {
    int count = 0;
    for (size_t i = 0; i < app.pending.size(); ++i)
        if (app.pending[i] && app.pendingState[i] != app.state[i]) ++count;
    return count;
}

void RefreshStates(App& app) {
    app.state.assign(Policies().size(), Tri::NotConfigured);
    for (size_t i = 0; i < Policies().size(); ++i) app.state[i] = Read(Policies()[i]);
    app.pending.assign(Policies().size(), 0);
    app.pendingState.assign(Policies().size(), Tri::NotConfigured);
}

void DefaultStatus(App& app) {
    int active = 0;
    for (Tri state : app.state)
        if (state != Tri::NotConfigured) ++active;
    const int pending = PendingCount(app);
    app.statusError = false;
    if (pending > 0) {
        app.status = std::to_wstring(pending) + (pending == 1 ? L" change ready to apply" : L" changes ready to apply");
        app.statusHint = L"Press Apply changes to write them. Discard puts every row back as it was.";
    } else {
        app.status = std::to_wstring(active) + L" of " + std::to_wstring(Policies().size()) +
                     L" settings are set on this PC";
        app.statusHint = L"Nothing is written until you press Apply changes.";
    }
}

// Everything staged that differs from what the machine reads, in the form the
// elevated copy understands. Empty when nothing is staged.
std::wstring BuildApplySpec(const App& app) {
    std::vector<int> indices;
    std::vector<Tri> states;
    for (size_t i = 0; i < app.pending.size(); ++i) {
        if (!app.pending[i] || app.pendingState[i] == app.state[i]) continue;
        indices.push_back(static_cast<int>(i));
        states.push_back(app.pendingState[i]);
    }
    return EncodeStagedSpec(indices, states);
}

// ---------------------------------------------------------------------------
//  Layout
// ---------------------------------------------------------------------------

Layout BuildLayout(const App& app, int width, int height) {
    Layout layout;
    const int margin = S(24);
    const int headerHeight = S(100);
    const int footerHeight = S(108);
    const int sidebarWidth = S(228);

    layout.header = {0, 0, width, headerHeight};
    layout.footer = {0, height - footerHeight, width, height};
    layout.sidebar = {margin, headerHeight, margin + sidebarWidth, height - footerHeight};

    const int searchWidth = std::min(S(320), width / 3);
    layout.searchBox = {width - margin - searchWidth, S(34), width - margin, S(70)};

    // Category rows: one per entry of the catalog's category list.
    layout.categoryCount = static_cast<int>(Categories().size());
    for (int i = 0; i < layout.categoryCount; ++i) {
        const int top = layout.sidebar.top + S(10) + i * S(40);
        layout.category[i] = {layout.sidebar.left, top, layout.sidebar.right, top + S(38)};
    }

    layout.content = {layout.sidebar.right + S(28), headerHeight, width - margin, height - footerHeight};
    layout.help = {layout.content.left, layout.content.top + S(6), layout.content.right,
                   layout.content.top + S(34)};
    layout.cardArea = {layout.content.left, layout.help.bottom + S(6), layout.content.right,
                       layout.footer.top - S(12)};

    // Cards: one per visible policy, scrolled by whole pixels. A card with a
    // caveat is one line taller, so the tag never takes room from the path.
    const int gap = S(10);
    int top = layout.cardArea.top - app.scroll;
    for (int policy : app.visible) {
        const Policy& entry = Policies()[static_cast<size_t>(policy)];
        const bool tagged = entry.caveat != nullptr && *entry.caveat != L'\0';
        const int cardHeight = S(tagged ? 128 : 108);
        CardView view;
        view.policy = policy;
        view.tagged = tagged;
        view.rect = {layout.cardArea.left, top, layout.cardArea.right, top + cardHeight};
        const int segHeight = S(30);
        const int segTop = top + S(16);
        const int segWidths[3] = {S(94), S(70), S(74)};
        int x = view.rect.right - S(18);
        int lefts[3] = {};
        for (int i = 2; i >= 0; --i) {
            x -= segWidths[i];
            lefts[i] = x;
            x -= S(6);
        }
        for (int i = 0; i < 3; ++i)
            view.seg[i] = {lefts[i], segTop, lefts[i] + segWidths[i], segTop + segHeight};
        layout.cards.push_back(view);
        top += cardHeight + gap;
    }

    const int contentHeight = top + app.scroll - layout.cardArea.top;
    const int viewHeight = layout.cardArea.bottom - layout.cardArea.top;
    layout.scrollMax = std::max(0, contentHeight - viewHeight);

    // A slim thumb appears only when there is genuinely more list than window.
    if (layout.scrollMax > 0) {
        layout.hasScroll = true;
        layout.track = {layout.cardArea.right - S(4), layout.cardArea.top, layout.cardArea.right,
                        layout.cardArea.bottom};
        const int trackHeight = layout.track.bottom - layout.track.top;
        const int thumbHeight =
            std::max(S(28), MulDiv(trackHeight, viewHeight, contentHeight));
        const int travel = std::max(0, trackHeight - thumbHeight);
        const int offset = layout.scrollMax > 0 ? MulDiv(travel, app.scroll, layout.scrollMax) : 0;
        layout.thumb = {layout.track.left, layout.track.top + offset, layout.track.right,
                        layout.track.top + offset + thumbHeight};
    }

    // The bottom row of the footer holds the copyright, with the administrator
    // link on its right when there is one.
    layout.copyright = {margin, layout.footer.top + S(84),
                        width - margin - (app.elevated ? 0 : S(200)), layout.footer.top + S(102)};

    // Footer buttons, right to left.
    const int buttonHeight = S(38);
    const int buttonTop = layout.footer.top + S(24);
    int right = width - margin;
    layout.btnApply = {right - S(158), buttonTop, right, buttonTop + buttonHeight};
    right = layout.btnApply.left - S(10);
    layout.btnDiscard = {right - S(104), buttonTop, right, buttonTop + buttonHeight};
    right = layout.btnDiscard.left - S(10);
    layout.btnBackup = {right - S(126), buttonTop, right, buttonTop + buttonHeight};

    // Reading policies needs no rights, so the window runs as a normal user and
    // offers the administrator restart next to the buttons. The manifest is
    // asInvoker for the same reason.
    if (!app.elevated) {
        const int elevateTop = layout.footer.top + S(83);
        layout.btnElevate = {width - margin - S(190), elevateTop, width - margin,
                             elevateTop + S(18)};
        layout.hasElevate = true;
    }

    return layout;
}

// ---------------------------------------------------------------------------
//  Painting
// ---------------------------------------------------------------------------

void PaintSearch(HDC dc, const App& app, const Layout& layout) {
    const RECT& box = layout.searchBox;
    FillRound(dc, box, kCard, S(9));
    StrokeRound(dc, box, app.searchFocused ? kAccent : kStroke, S(9));
    DrawMagnifier(dc, box.left + S(14), box.top + (box.bottom - box.top - S(15)) / 2, S(15),
                  app.search.empty() ? kTextFaint : kTextDim);
    RECT text = {box.left + S(40), box.top, box.right - S(12), box.bottom};
    if (app.search.empty()) {
        TextIn(dc, text, L"Search settings", g_fontBody, kTextFaint,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        TextIn(dc, text, app.search, g_fontBody, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    if (app.searchFocused && app.caretVisible) {
        SIZE extent = {};
        HGDIOBJ old = SelectObject(dc, g_fontBody);
        GetTextExtentPoint32W(dc, app.search.c_str(), static_cast<int>(app.search.size()), &extent);
        SelectObject(dc, old);
        const int caretX = std::min(text.left + extent.cx + 1, text.right - 2);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), kAccent);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, caretX, box.top + S(10), nullptr);
        LineTo(dc, caretX, box.bottom - S(10));
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }
}

void PaintSidebar(HDC dc, const App& app, const Layout& layout) {
    for (int i = 0; i < layout.categoryCount; ++i) {
        int count = 0;
        for (const Policy& policy : Policies())
            if (i == 0 || policy.category == i) ++count;

        const RECT& row = layout.category[i];
        const bool selected = app.category == i;
        if (selected) {
            RECT body = row;
            body.left += S(6);
            FillRound(dc, body, kCardActive, S(7));
            RECT bar = {body.left + S(2), body.top + S(8), body.left + S(5), body.bottom - S(8)};
            FillRound(dc, bar, kAccent, S(2));
        } else if (app.hoverCategory == i) {
            RECT body = row;
            body.left += S(6);
            FillRound(dc, body, kCard, S(7));
        }

        RECT label = row;
        label.left += S(16);
        label.right -= S(52);
        TextIn(dc, label, Categories()[static_cast<size_t>(i)], g_fontBody,
               selected ? kText : kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT chip = row;
        chip.left = row.right - S(46);
        chip.right -= S(10);
        TextIn(dc, chip, std::to_wstring(count), g_fontSmall,
               selected ? kTextDim : kTextFaint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

void PaintCard(HDC dc, const App& app, const CardView& view, int index) {
    const Policy& policy = Policies()[static_cast<size_t>(view.policy)];
    const Tri state = app.state[static_cast<size_t>(view.policy)];
    const bool staged = app.pending[static_cast<size_t>(view.policy)] != 0;
    const Tri stagedState = app.pendingState[static_cast<size_t>(view.policy)];
    const bool dirty = staged && stagedState != state;
    const bool hot = app.hoverCard == index;

    FillRound(dc, view.rect, hot ? kCardHover : kCard, S(10));
    StrokeRound(dc, view.rect, dirty ? kAccentDeep : kStroke, S(10));

    const int textLeft = view.rect.left + S(18);
    const int textRight = view.seg[0].left - S(18);
    RECT title = {textLeft, view.rect.top + S(14), textRight, view.rect.top + S(36)};
    TextIn(dc, title, policy.title, g_fontCardTitle, kText,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (dirty) {
        const int dot = S(6);
        RECT marker = {view.rect.right - S(12), view.rect.top + S(10), view.rect.right - S(6),
                       view.rect.top + S(10) + dot};
        FillRound(dc, marker, kAccent, dot);
    }

    const int summaryBottom = view.tagged ? view.rect.bottom - S(48) : view.rect.bottom - S(26);
    RECT summary = {textLeft, title.bottom + S(2), textRight, summaryBottom};
    TextIn(dc, summary, policy.summary, g_fontBody, kTextDim,
           DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

    // The path gets the full width (nothing sits beside it on this line) and a
    // caveat sits on its own line underneath.
    RECT path = {textLeft, view.rect.bottom - S(view.tagged ? 46 : 24), view.rect.right - S(18),
                 view.rect.bottom - S(view.tagged ? 30 : 8)};
    TextIn(dc, path, PathText(policy), g_fontSmall, kTextFaint,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (view.tagged) {
        RECT note = {textLeft, view.rect.bottom - S(26), textRight, view.rect.bottom - S(10)};
        TextIn(dc, note, policy.caveat, g_fontSmall, kWarn,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    static const wchar_t* kLabels[3] = {L"Not configured", L"Enabled", L"Disabled"};
    for (int i = 0; i < 3; ++i) {
        const Tri value = i == 0 ? Tri::NotConfigured : (i == 1 ? Tri::Enabled : Tri::Disabled);
        const bool active = staged ? stagedState == value : state == value;
        const bool segmentHot = hot && app.hoverSegment == i;
        COLORREF fill = kCardActive;
        if (active) {
            fill = value == Tri::Enabled ? kAccent : (value == Tri::Disabled ? kDisabledFill : RGB(62, 66, 80));
        } else if (segmentHot) {
            fill = RGB(52, 55, 66);
        } else {
            fill = RGB(40, 42, 51);
        }
        FillRound(dc, view.seg[i], fill, S(7));
        if (!active) StrokeRound(dc, view.seg[i], kStroke, S(7));
        TextIn(dc, view.seg[i], kLabels[i], g_fontSmall,
               active ? kOnAccent : (segmentHot ? kText : kTextDim),
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    RECT effect = {view.seg[0].left, view.seg[0].bottom + S(4), view.seg[2].right,
                   view.seg[0].bottom + S(20)};
    TextIn(dc, effect, std::wstring(L"Applies: ") + policy.effect, g_fontSmall, kTextFaint,
           DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void PaintFooter(HDC dc, const App& app, const Layout& layout) {
    const int left = S(24);
    const int right = layout.btnBackup.left - S(16);
    RECT status = {left, layout.footer.top + S(20), right, layout.footer.top + S(42)};
    TextIn(dc, status, app.status, g_fontBodyBold, app.statusError ? kError : kText,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT hint = {left, layout.footer.top + S(45), right, layout.footer.top + S(66)};
    TextIn(dc, hint, app.statusHint, g_fontSmall, kTextDim,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    // A hairline rule separates the actions from the legal footer row.
    const int ruleTop = layout.footer.top + S(74);
    HBRUSH rule = CreateSolidBrush(kStroke);
    RECT line = {left, ruleTop, layout.footer.right - S(24), ruleTop + 1};
    FillRect(dc, &line, rule);
    DeleteObject(rule);
    TextIn(dc, layout.copyright, kCopyright, g_fontSmall, kTextFaint,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    DrawButton(dc, layout.btnBackup, L"Back up...", false, app.hotButton == 2, true);
    DrawButton(dc, layout.btnDiscard, L"Discard", false, app.hotButton == 1, PendingCount(app) > 0);
    DrawButton(dc, layout.btnApply, L"Apply changes", true, app.hotButton == 0, true);

    if (layout.hasElevate) {
        TextIn(dc, layout.btnElevate, L"Restart as administrator", g_fontSmall, kAccent,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

void PaintWindow(HWND window, HDC target) {
    if (!g_app) return;
    App& app = *g_app;
    RECT client = {};
    GetClientRect(window, &client);
    const int width = client.right;
    const int height = client.bottom;

    HDC memory = CreateCompatibleDC(target);
    HBITMAP back = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldBitmap = SelectObject(memory, back);

    HBRUSH background = CreateSolidBrush(kBackground);
    FillRect(memory, &client, background);
    DeleteObject(background);

    Layout& layout = app.layout;
    layout = BuildLayout(app, width, height);

    // Header
    RECT title = {S(24), S(24), layout.searchBox.left - S(24), S(58)};
    TextIn(memory, title, L"Home Policy", g_fontTitle, kText,
           DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT subtitle = {S(24), S(62), layout.searchBox.left - S(24), S(84)};
    TextIn(memory, subtitle, L"Group Policy Editor for Windows 10 Home and Windows 11 Home",
           g_fontSubtitle, kTextDim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    PaintSearch(memory, app, layout);
    PaintSidebar(memory, app, layout);

    // Content help line
    TextIn(memory, layout.help,
           L"Pick a category, then choose Enabled or Disabled for a setting. Not configured "
           L"leaves Windows in charge.",
           g_fontSmall, kTextFaint, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Cards, clipped to the scrolling area
    SaveDC(memory);
    IntersectClipRect(memory, layout.cardArea.left - S(2), layout.cardArea.top,
                      layout.cardArea.right + S(2), layout.cardArea.bottom);
    for (size_t i = 0; i < layout.cards.size(); ++i) {
        const CardView& view = layout.cards[i];
        if (view.rect.bottom < layout.cardArea.top || view.rect.top > layout.cardArea.bottom) continue;
        PaintCard(memory, app, view, static_cast<int>(i));
    }
    RestoreDC(memory, -1);

    if (app.visible.empty()) {
        RECT empty = layout.cardArea;
        empty.top += S(30);
        TextIn(memory, empty,
               app.search.empty() ? L"Nothing in this category." : L"No settings match that search.",
               g_fontBody, kTextDim, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }

    if (layout.hasScroll) {
        FillRound(memory, layout.track, RGB(40, 42, 50), S(2));
        FillRound(memory, layout.thumb, app.draggingThumb ? kAccent : RGB(78, 84, 100), S(2));
    }

    PaintFooter(memory, app, layout);

    BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap);
    DeleteObject(back);
    DeleteDC(memory);
}

// ---------------------------------------------------------------------------
//  Actions
// ---------------------------------------------------------------------------

void ApplyPending(HWND window, App& app) {
    int changed = 0;
    int failed = 0;
    std::wstring firstError;
    std::wstring failedTitle;
    for (size_t i = 0; i < app.pending.size(); ++i) {
        if (!app.pending[i] || app.pendingState[i] == app.state[i]) continue;
        const Policy& policy = Policies()[i];
        std::wstring error;
        if (Write(policy, app.pendingState[i], &error)) {
            app.state[i] = app.pendingState[i];
            app.pending[i] = 0;
            ++changed;
        } else {
            ++failed;
            if (firstError.empty()) {
                firstError = error;
                failedTitle = policy.title;
            }
        }
    }

    if (failed > 0) {
        app.statusError = true;
        app.status = L"Could not change " + std::to_wstring(failed) +
                     (failed == 1 ? L" setting" : L" settings") + L": " + failedTitle;
        app.statusHint = firstError + L" - the row is still staged, so nothing else was lost.";
    } else if (changed == 0) {
        DefaultStatus(app);
    } else {
        app.statusError = false;
        app.status = L"Applied " + std::to_wstring(changed) +
                     (changed == 1 ? L" change." : L" changes.") +
                     L" Sign out or restart where a setting says so.";
        app.statusHint = L"Settings read 'Applies: immediately' are already in force.";
    }
    InvalidateRect(window, nullptr, FALSE);
}

void DiscardPending(HWND window, App& app) {
    for (size_t i = 0; i < app.pending.size(); ++i) {
        app.pending[i] = 0;
        app.pendingState[i] = app.state[i];
    }
    DefaultStatus(app);
    InvalidateRect(window, nullptr, FALSE);
}

void Backup(HWND window, App& app) {
    wchar_t path[MAX_PATH] = L"HomePolicy-backup.reg";
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"Registry files (*.reg)\0*.reg\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"reg";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&dialog)) return;

    int values = 0;
    std::wstring error;
    if (WriteBackup(path, &values, &error)) {
        app.statusError = false;
        app.status = L"Backed up " + std::to_wstring(values) + L" policy values.";
        app.statusHint = std::wstring(L"Saved as ") + path + L" - open it to put them back.";
    } else {
        app.statusError = true;
        app.status = L"The backup was not written.";
        app.statusHint = error;
    }
    InvalidateRect(window, nullptr, FALSE);
}

// Starts this program elevated, with the staged choices on its command line so
// the new window writes them instead of reading a fresh, empty staging area.
bool LaunchElevated(const std::wstring& spec) {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    const std::wstring parameters = spec.empty() ? std::wstring() : L"--staged " + spec;
    SHELLEXECUTEINFOW run = {};
    run.cbSize = sizeof run;
    run.fMask = SEE_MASK_NOCLOSEPROCESS;
    run.lpVerb = L"runas";
    run.lpFile = path;
    run.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    run.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&run)) return false;
    if (run.hProcess) CloseHandle(run.hProcess);
    return true;
}

void SetSegment(App& app, int policyIndex, int segment, HWND window) {
    const Tri want = segment == 0 ? Tri::NotConfigured : (segment == 1 ? Tri::Enabled : Tri::Disabled);
    if (app.state[static_cast<size_t>(policyIndex)] == want) {
        app.pending[static_cast<size_t>(policyIndex)] = 0;
    } else {
        app.pending[static_cast<size_t>(policyIndex)] = 1;
        app.pendingState[static_cast<size_t>(policyIndex)] = want;
    }
    DefaultStatus(app);
    InvalidateRect(window, nullptr, FALSE);
}

void ScrollTo(App& app, int value) {
    const int maxScroll = app.layout.scrollMax;
    app.scroll = std::max(0, std::min(value, maxScroll));
}

// ---------------------------------------------------------------------------
//  Hit testing
// ---------------------------------------------------------------------------

bool Inside(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
           point.y < rect.bottom;
}

// ---------------------------------------------------------------------------
//  Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            g_dpi = QueryDpi(window);
            BuildFonts();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = BeginPaint(window, &paint);
            PaintWindow(window, dc);
            EndPaint(window, &paint);
            return 0;
        }

        case WM_DPICHANGED: {
            g_dpi = HIWORD(wParam);
            BuildFonts();
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = S(940);
            info->ptMinTrackSize.y = S(580);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == 1 && g_app && g_app->searchFocused) {
                g_app->caretVisible = !g_app->caretVisible;
                InvalidateRect(window, &g_app->layout.searchBox, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (!g_app) return 0;
            App& app = *g_app;
            const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            const bool focusBefore = app.searchFocused;
            app.searchFocused = Inside(app.layout.searchBox, point);
            if (app.searchFocused != focusBefore) {
                app.caretVisible = true;
                SetTimer(window, 1, 530, nullptr);
                if (!app.searchFocused) KillTimer(window, 1);
                InvalidateRect(window, nullptr, FALSE);
            }

            if (Inside(app.layout.btnApply, point)) {
                if (app.elevated) {
                    ApplyPending(window, app);
                } else {
                    // One UAC prompt beats an Access denied row. The staged
                    // choices go on the elevated copy's command line; this
                    // window then closes so the single-instance lock is free by
                    // the time the new one asks for it.
                    const std::wstring spec = BuildApplySpec(app);
                    if (spec.empty()) {
                        DefaultStatus(app);
                        InvalidateRect(window, nullptr, FALSE);
                        return 0;
                    }
                    app.statusError = false;
                    app.status = L"Asking for administrator rights...";
                    app.statusHint = L"The staged choices go with the elevated window and are written there.";
                    InvalidateRect(window, nullptr, FALSE);
                    UpdateWindow(window);
                    if (LaunchElevated(spec)) {
                        DestroyWindow(window);
                    } else {
                        app.statusError = true;
                        app.status = L"Administrator rights were not granted.";
                        app.statusHint = L"Nothing changed - your staged choices are still here.";
                        InvalidateRect(window, nullptr, FALSE);
                    }
                }
                SetFocus(window);
                return 0;
            }
            if (Inside(app.layout.btnDiscard, point) && PendingCount(app) > 0) {
                DiscardPending(window, app);
                SetFocus(window);
                return 0;
            }
            if (Inside(app.layout.btnBackup, point)) {
                Backup(window, app);
                return 0;
            }
            if (app.layout.hasElevate && Inside(app.layout.btnElevate, point)) {
                const std::wstring spec = BuildApplySpec(app);
                if (LaunchElevated(spec)) DestroyWindow(window);
                return 0;
            }

            if (Inside(app.layout.track, point)) {
                app.draggingThumb = true;
                app.dragOffset = point.y - app.layout.thumb.top;
                SetCapture(window);
                return 0;
            }

            for (int i = 0; i < app.layout.categoryCount; ++i) {
                if (!Inside(app.layout.category[i], point)) continue;
                if (app.category != i) {
                    if (PendingCount(app) > 0) {
                        app.statusError = false;
                        app.status = L"You changed category with staged changes.";
                        app.statusHint = L"They are still staged - press Apply changes or Discard.";
                    }
                    app.category = i;
                    RebuildVisible(app);
                    InvalidateRect(window, nullptr, FALSE);
                }
                SetFocus(window);
                return 0;
            }

            for (const CardView& view : app.layout.cards) {
                for (int i = 0; i < 3; ++i) {
                    if (!Inside(view.seg[i], point)) continue;
                    SetSegment(app, view.policy, i, window);
                    SetFocus(window);
                    return 0;
                }
            }
            SetFocus(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP: {
            if (g_app && g_app->draggingThumb) {
                g_app->draggingThumb = false;
                ReleaseCapture();
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!g_app) return 0;
            App& app = *g_app;
            const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            if (app.draggingThumb && app.layout.scrollMax > 0) {
                const int trackHeight = app.layout.track.bottom - app.layout.track.top;
                const int thumbHeight = app.layout.thumb.bottom - app.layout.thumb.top;
                const int travel = std::max(1, trackHeight - thumbHeight);
                const int offset = point.y - app.dragOffset - app.layout.track.top;
                ScrollTo(app, MulDiv(offset, app.layout.scrollMax, travel));
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            int hoverCard = -1;
            int hoverSegment = -1;
            for (size_t i = 0; i < app.layout.cards.size(); ++i) {
                const CardView& view = app.layout.cards[i];
                if (!Inside(view.rect, point)) continue;
                hoverCard = static_cast<int>(i);
                for (int segment = 0; segment < 3; ++segment)
                    if (Inside(view.seg[segment], point)) hoverSegment = segment;
                break;
            }
            int hoverCategory = -1;
            for (int i = 0; i < app.layout.categoryCount; ++i)
                if (Inside(app.layout.category[i], point)) hoverCategory = i;

            int hotButton = -1;
            if (Inside(app.layout.btnApply, point)) hotButton = 0;
            else if (Inside(app.layout.btnDiscard, point)) hotButton = 1;
            else if (Inside(app.layout.btnBackup, point)) hotButton = 2;
            else if (app.layout.hasElevate && Inside(app.layout.btnElevate, point)) hotButton = 3;

            if (hoverCard != app.hoverCard || hoverSegment != app.hoverSegment ||
                hoverCategory != app.hoverCategory || hotButton != app.hotButton) {
                app.hoverCard = hoverCard;
                app.hoverSegment = hoverSegment;
                app.hoverCategory = hoverCategory;
                app.hotButton = hotButton;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        case WM_SETCURSOR: {
            if (g_app) {
                POINT point = {};
                GetCursorPos(&point);
                ScreenToClient(window, &point);
                const bool clickable =
                    g_app->hoverCard >= 0 || g_app->hoverCategory >= 0 || g_app->hotButton >= 0 ||
                    Inside(g_app->layout.searchBox, point);
                SetCursor(LoadCursorW(nullptr, clickable ? IDC_HAND : IDC_ARROW));
                if (clickable) return TRUE;
            }
            break;
        }

        case WM_MOUSEWHEEL: {
            if (!g_app) return 0;
            POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            if (g_app->layout.scrollMax > 0 && point.y > g_app->layout.cardArea.top - S(40)) {
                g_app->scroll -= GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * S(72);
                ScrollTo(*g_app, g_app->scroll);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        case WM_CHAR: {
            if (!g_app || !g_app->searchFocused) break;
            const wchar_t ch = static_cast<wchar_t>(wParam);
            if (ch == L'\b') {
                if (!g_app->search.empty()) {
                    g_app->search.pop_back();
                    RebuildVisible(*g_app);
                    DefaultStatus(*g_app);
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            }
            if (ch < 32 || ch == 127) return 0;
            if (g_app->search.size() < 64) {
                g_app->search.push_back(ch);
                RebuildVisible(*g_app);
                DefaultStatus(*g_app);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        case WM_KEYDOWN: {
            if (!g_app) return 0;
            App& app = *g_app;
            if (wParam == VK_ESCAPE) {
                if (!app.search.empty()) {
                    app.search.clear();
                    RebuildVisible(app);
                    DefaultStatus(app);
                } else {
                    app.searchFocused = false;
                    KillTimer(window, 1);
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (wParam == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                app.searchFocused = true;
                app.caretVisible = true;
                SetTimer(window, 1, 530, nullptr);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) && app.searchFocused) {
                if (OpenClipboard(window)) {
                    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                        if (const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data))) {
                            for (const wchar_t* p = text; *p && app.search.size() < 64; ++p)
                                if (*p >= 32 && *p != 127) app.search.push_back(*p);
                            GlobalUnlock(data);
                        }
                    }
                    CloseClipboard();
                    RebuildVisible(app);
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            }
            if (wParam == VK_RETURN && PendingCount(app) > 0) {
                ApplyPending(window, app);
                return 0;
            }
            if (wParam == VK_DOWN || wParam == VK_UP) {
                ScrollTo(app, app.scroll + (wParam == VK_DOWN ? S(72) : -S(72)));
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;
        }

        case WM_DESTROY:
            KillTimer(window, 1);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  Command line
// ---------------------------------------------------------------------------

HANDLE OpenConsole() {
    // A redirected run (a pipe or a file) already owns a usable stdout; a
    // GUI-subsystem process started from a console has none until it attaches.
    HANDLE standard = GetStdHandle(STD_OUTPUT_HANDLE);
    if (standard && standard != INVALID_HANDLE_VALUE) return standard;
    AttachConsole(ATTACH_PARENT_PROCESS);
    standard = GetStdHandle(STD_OUTPUT_HANDLE);
    if (standard && standard != INVALID_HANDLE_VALUE) return standard;
    AllocConsole();
    return CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

void Say(HANDLE out, const std::wstring& text) {
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (GetFileType(out) == FILE_TYPE_CHAR &&
        WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr))
        return;
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return;
    std::string bytes(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), bytes.data(),
                        needed, nullptr, nullptr);
    WriteFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

void CloseConsole(HANDLE out) {
    if (out && out != INVALID_HANDLE_VALUE && out != GetStdHandle(STD_OUTPUT_HANDLE))
        CloseHandle(out);
}

std::wstring HexDword(DWORD value) {
    wchar_t text[16] = {};
    swprintf_s(text, L"0x%08lX", static_cast<unsigned long>(value));
    return text;
}

// One setting, written and read back, for an elevated shell or a support
// session: HomePolicy.exe --apply "Widgets board" disabled
int ApplyMode(const std::wstring& query, const std::wstring& stateText) {
    const HANDLE out = OpenConsole();
    Say(out, L"\r\nHome Policy - apply one setting\r\n\r\n");

    Tri want = Tri::Custom;
    if (!ParseTri(stateText, &want)) {
        Say(out, L"  \"" + stateText + L"\" is not a state. Use enabled, disabled or not-configured.\r\n");
        CloseConsole(out);
        return 2;
    }

    const std::vector<int> matches = FindPolicies(query);
    if (matches.empty()) {
        Say(out, L"  No setting matches \"" + query + L"\". Run --list to see every title.\r\n");
        CloseConsole(out);
        return 1;
    }
    if (matches.size() > 1) {
        Say(out, L"  \"" + query + L"\" matches " + std::to_wstring(matches.size()) +
                     L" settings - give more of the title:\r\n");
        for (size_t i = 0; i < matches.size() && i < 8; ++i)
            Say(out, L"    - " + std::wstring(Policies()[static_cast<size_t>(matches[i])].title) + L"\r\n");
        CloseConsole(out);
        return 2;
    }

    const Policy& policy = Policies()[static_cast<size_t>(matches[0])];
    const Tri before = Read(policy);
    std::wstring error;
    const bool written = Write(policy, want, &error);
    const Tri after = Read(policy);

    DWORD raw = 0;
    const bool present = ReadFirstValue(policy, &raw);

    Say(out, L"  setting : " + std::wstring(policy.title) + L"\r\n");
    Say(out, L"  path    : " + PathText(policy) + L"\r\n");
    Say(out, L"  before  : " + std::wstring(TriName(before)) + L"\r\n");
    Say(out, L"  asked   : " + std::wstring(TriName(want)) + L"\r\n");
    if (!written) Say(out, L"  write   : failed - " + error + L"\r\n");
    Say(out, L"  reads   : " + std::wstring(TriName(after)) +
                 (present ? L"  (value " + HexDword(raw) + L")\r\n" : L"  (value absent)\r\n"));
    Say(out, L"  applies : " + std::wstring(policy.effect) + L"\r\n");

    const bool ok = written && after == want;
    if (ok) {
        Say(out, L"  result  : OK\r\n");
    } else {
        Say(out, L"  result  : FAILED\r\n");
        if (!IsElevated())
            Say(out, L"            this policy key needs an elevated shell (run as administrator).\r\n");
        else if (error.find(L"Access is denied") != std::wstring::npos)
            Say(out, L"            Windows refused the key itself - some builds protect a policy key\r\n"
                     L"            against every writer, reg.exe included.\r\n");
    }
    CloseConsole(out);
    return ok ? 0 : 1;
}

int ListMode() {
    const HANDLE out = OpenConsole();
    Say(out, L"\r\nHome Policy - the catalog, as this machine reads it\r\n\r\n");
    int configured = 0;
    for (const Policy& policy : Policies()) {
        const Tri state = Read(policy);
        if (state != Tri::NotConfigured) ++configured;
        Say(out, std::wstring(L"  [") + TriName(state) + L"]  " + policy.title + L"\r\n      " +
                     PathText(policy) + L"\r\n");
    }
    Say(out, L"\r\n  " + std::to_wstring(configured) + L" of " +
                 std::to_wstring(Policies().size()) + L" settings set on this PC.\r\n");
    CloseConsole(out);
    return 0;
}

int SelfTestMode() {
    const HANDLE out = OpenConsole();
    Say(out, L"\r\nHome Policy self-test\r\n\r\n");
    int passed = 0;
    int failed = 0;
    for (const TestResult& result : RunSelfTest()) {
        if (result.passed) {
            ++passed;
            Say(out, std::wstring(L"  [PASS] ") + result.name + L"\r\n");
        } else {
            ++failed;
            Say(out, std::wstring(L"  [FAIL] ") + result.name +
                         (result.detail.empty() ? L"" : L" - " + result.detail) + L"\r\n");
        }
    }
    Say(out, L"\r\n  RESULT: " + std::to_wstring(passed) + L" passed, " +
                 std::to_wstring(failed) + L" failed.\r\n");
    CloseConsole(out);
    return failed == 0 ? 0 : 1;
}

int HelpMode() {
    const HANDLE out = OpenConsole();
    Say(out, L"\r\nHome Policy - a Group Policy Editor for Windows 10 and 11 Home.\r\n\r\n"
             L"  HomePolicy.exe                             open the window\r\n"
             L"  HomePolicy.exe --apply <title> <state>     write one setting, read it back\r\n"
             L"  HomePolicy.exe --list                      every setting and what this PC reads\r\n"
             L"  HomePolicy.exe --selftest                  prove the catalog and the engine\r\n"
             L"  HomePolicy.exe --help                      this text\r\n\r\n"
             L"  <state> is enabled, disabled or not-configured. <title> is any part of a\r\n"
             L"  setting's title that resolves to one row, e.g.\r\n"
             L"    HomePolicy.exe --apply \"Widgets board\" disabled\r\n"
             L"    HomePolicy.exe --apply \"Widgets board\" not-configured\r\n\r\n"
             L"  --staged \"i:s;i:s\" is internal: the unelevated window passes its\r\n"
             L"  staged choices to the elevated copy it launches.\r\n\r\n"
             L"  Each setting writes one of the policy values that the official editor\r\n"
             L"  would write, under HKLM\\SOFTWARE\\Policies. Nothing is changed until\r\n"
             L"  Apply changes is pressed, and 'Not configured' hands a value back to\r\n"
             L"  Windows by removing it.\r\n");
    CloseConsole(out);
    return 0;
}

// ---------------------------------------------------------------------------
//  Entry points
// ---------------------------------------------------------------------------

bool RegisterMainClass(HINSTANCE instance, HICON* big, HICON* small) {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof windowClass;
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kClassName;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(2));
    if (!windowClass.hIcon) windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(2), IMAGE_ICON, S(16), S(16), LR_DEFAULTCOLOR));
    if (!windowClass.hIconSm) windowClass.hIconSm = windowClass.hIcon;
    if (!RegisterClassExW(&windowClass)) return false;
    *big = windowClass.hIcon;
    *small = windowClass.hIconSm;
    return true;
}

void UseDarkTitleBar(HWND window) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof dark);   // DWMWA_USE_IMMERSIVE_DARK_MODE
    const COLORREF caption = kBackground;
    DwmSetWindowAttribute(window, 35, &caption, sizeof caption);  // DWMWA_CAPTION_COLOR
    const COLORREF border = RGB(48, 51, 62);
    DwmSetWindowAttribute(window, 34, &border, sizeof border);    // DWMWA_BORDER_COLOR
}

int RunGui(HINSTANCE instance, int showCommand) {
    const std::wstring handoff = g_staged;
    HANDLE instanceLock = nullptr;
    if (handoff.empty()) {
        instanceLock = CreateMutexW(nullptr, TRUE, L"Local\\HomePolicy.SingleInstance");
        if (instanceLock && GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND previous = FindWindowW(kClassName, nullptr)) {
                ShowWindow(previous, SW_RESTORE);
                SetForegroundWindow(previous);
            }
            CloseHandle(instanceLock);
            return 0;
        }
    } else {
        // Launched by the window that just staged these choices and is closing
        // now: its single-instance mutex is still held for a moment. Waiting for
        // the handover is what keeps this copy from exiting with the list, which
        // is exactly what used to happen.
        for (int attempt = 0; attempt < 100; ++attempt) {
            instanceLock = CreateMutexW(nullptr, TRUE, L"Local\\HomePolicy.SingleInstance");
            if (instanceLock && GetLastError() != ERROR_ALREADY_EXISTS) break;
            if (instanceLock) {
                CloseHandle(instanceLock);
                instanceLock = nullptr;
            }
            Sleep(100);
        }
        // After ten seconds the other window is not closing; carry on without
        // the lock rather than drop changes the user already staged.
    }

    HICON bigIcon = nullptr;
    HICON smallIcon = nullptr;
    if (!RegisterMainClass(instance, &bigIcon, &smallIcon)) return 1;

    g_app = new App();
    g_app->elevated = IsElevated();
    RefreshStates(*g_app);
    RebuildVisible(*g_app);
    if (!handoff.empty()) {
        std::wstring firstError;
        std::wstring failedTitle;
        const int changed = ApplyStagedSpec(Policies(), handoff, &firstError, &failedTitle);
        // Read the machine again rather than trust the write: the window shows
        // what landed, and a policy another component rewrote still reads true.
        RefreshStates(*g_app);
        RebuildVisible(*g_app);
        if (!firstError.empty()) {
            g_app->statusError = true;
            g_app->status = L"Could not change " + failedTitle + L".";
            g_app->statusHint = firstError;
        } else if (changed > 0) {
            g_app->status = L"Applied " + std::to_wstring(changed) +
                            (changed == 1 ? L" change" : L" changes") +
                            L" from the previous window.";
            g_app->statusHint = L"Sign out or restart where a setting says so.";
        } else {
            DefaultStatus(*g_app);
        }
    } else {
        DefaultStatus(*g_app);
    }

    HWND window = CreateWindowExW(0, kClassName, L"Home Policy",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(1160),
                                  S(780), nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    UseDarkTitleBar(window);
    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));

    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    RECT frame = {};
    GetWindowRect(window, &frame);
    const int width = std::min(static_cast<int>(frame.right - frame.left),
                               static_cast<int>(workArea.right - workArea.left));
    const int height = std::min(static_cast<int>(frame.bottom - frame.top),
                                static_cast<int>(workArea.bottom - workArea.top));
    SetWindowPos(window, nullptr,
                 workArea.left + (workArea.right - workArea.left - width) / 2,
                 workArea.top + (workArea.bottom - workArea.top - height) / 2, width, height,
                 SWP_NOZORDER);

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    delete g_app;
    g_app = nullptr;
    if (instanceLock) {
        ReleaseMutex(instanceLock);
        CloseHandle(instanceLock);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode;
    std::wstring first;
    std::wstring second;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--staged" && i + 1 < argc) {
            g_staged = argv[i + 1];
            ++i;
            continue;
        }
        if (arg == L"--apply" && i + 2 < argc) {
            mode = arg;
            first = argv[i + 1];
            second = argv[i + 2];
            i += 2;
            continue;
        }
        if (arg == L"--selftest" || arg == L"--list" || arg == L"--help" || arg == L"-h") {
            mode = arg;
            break;
        }
    }
    if (argv) LocalFree(argv);

    if (mode == L"--apply") return ApplyMode(first, second);
    if (mode == L"--selftest") return SelfTestMode();
    if (mode == L"--list") return ListMode();
    if (mode == L"--help" || mode == L"-h") return HelpMode();
    return RunGui(instance, showCommand);
}
