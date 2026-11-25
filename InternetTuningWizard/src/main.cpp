// ============================================================================
//  main.cpp — the Internet Tuning Wizard window.
//
//  Three steps, one decision each: which connection to tune, which machine-wide
//  switches to flip, and a review of exactly what will be written. The window
//  opens as a normal user so the wizard can be read before anything is changed;
//  Apply relaunches it elevated with the plan on the command line, and the
//  elevated copy writes the values and reports back through a small text file.
//
//  Everything is drawn with GDI into one dark surface, scaled by the monitor's
//  DPI, like the rest of the suite.
// ============================================================================

#include "tuning.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace itw;

namespace {

// ---------------------------------------------------------------------------
//  Palette
// ---------------------------------------------------------------------------

constexpr COLORREF kBackground = RGB(24, 25, 31);
constexpr COLORREF kCard = RGB(34, 36, 44);
constexpr COLORREF kCardHover = RGB(40, 42, 52);
constexpr COLORREF kCardActive = RGB(44, 47, 58);
constexpr COLORREF kStroke = RGB(57, 60, 71);
constexpr COLORREF kText = RGB(236, 238, 244);
constexpr COLORREF kTextDim = RGB(146, 152, 164);
constexpr COLORREF kTextFaint = RGB(104, 110, 124);
constexpr COLORREF kAccent = RGB(84, 158, 255);
constexpr COLORREF kAccentDeep = RGB(52, 110, 196);
constexpr COLORREF kOnAccent = RGB(250, 252, 255);
constexpr COLORREF kOk = RGB(88, 205, 143);
constexpr COLORREF kWarn = RGB(242, 178, 84);
constexpr COLORREF kError = RGB(240, 120, 120);
constexpr COLORREF kSwitchOff = RGB(58, 62, 74);

const wchar_t* kClassName = L"InternetTuningWizardWindow";

UINT  g_dpi = 96;
HFONT g_fontTitle = nullptr;
HFONT g_fontSubtitle = nullptr;
HFONT g_fontHeading = nullptr;
HFONT g_fontBody = nullptr;
HFONT g_fontBodyBold = nullptr;
HFONT g_fontSmall = nullptr;
HFONT g_fontSmallBold = nullptr;

int S(int logical) { return MulDiv(logical, static_cast<int>(g_dpi), 96); }

HFONT MakeFont(int points, int weight) {
    return CreateFontW(-MulDiv(points, static_cast<int>(g_dpi), 72), 0, 0, 0, weight, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void BuildFonts() {
    for (HFONT* font : {&g_fontTitle, &g_fontSubtitle, &g_fontHeading, &g_fontBody,
                        &g_fontBodyBold, &g_fontSmall, &g_fontSmallBold}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
    g_fontTitle = MakeFont(21, FW_SEMIBOLD);
    g_fontSubtitle = MakeFont(9, FW_NORMAL);
    g_fontHeading = MakeFont(13, FW_SEMIBOLD);
    g_fontBody = MakeFont(9, FW_NORMAL);
    g_fontBodyBold = MakeFont(10, FW_SEMIBOLD);
    g_fontSmall = MakeFont(8, FW_NORMAL);
    g_fontSmallBold = MakeFont(8, FW_SEMIBOLD);
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
//  Drawing
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

bool Inside(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
           point.y < rect.bottom;
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

void DrawSwitch(HDC dc, const RECT& rect, bool on) {
    FillRound(dc, rect, on ? kAccent : kSwitchOff, (rect.bottom - rect.top) / 2);
    const int knob = rect.bottom - rect.top - S(6);
    const int left = on ? rect.right - S(3) - knob : rect.left + S(3);
    RECT dot = {left, rect.top + S(3), left + knob, rect.top + S(3) + knob};
    FillRound(dc, dot, on ? kOnAccent : RGB(196, 201, 212), knob / 2);
}

void DrawRadio(HDC dc, int cx, int cy, bool selected, bool hot) {
    const int radius = S(8);
    RECT ring = {cx - radius, cy - radius, cx + radius, cy + radius};
    StrokeRound(dc, ring, selected ? kAccent : (hot ? kTextDim : kStroke), radius, S(2));
    if (selected) {
        const int inner = S(4);
        RECT dot = {cx - inner, cy - inner, cx + inner, cy + inner};
        FillRound(dc, dot, kAccent, inner);
    }
}

// ---------------------------------------------------------------------------
//  Window state
// ---------------------------------------------------------------------------

struct Layout {
    RECT header = {};
    RECT content = {};
    RECT footer = {};
    RECT steps[3] = {};
    std::vector<RECT> rows;      // one per adapter, moved by the scroll offset
    RECT adapterArea = {};       // the viewport the rows are clipped to
    RECT adapterTrack = {};
    RECT adapterThumb = {};
    bool hasAdapterScroll = false;
    int  adapterScrollMax = 0;
    int  adapterStride = 0;
    int  adapterRowHeight = 0;
    RECT dnsPills[4] = {};
    RECT dnsNote = {};
    RECT toggles[4] = {};
    RECT nagleToggle = {};
    RECT page = {};
    RECT btnBack = {};
    RECT btnNext = {};
    RECT btnExtra = {};
    bool hasBack = false;
    bool hasExtra = false;
    RECT copyright = {};
    RECT version = {};
};

struct App {
    int step = 0;                 // 0 connection, 1 tuning, 2 review, 3 results
    std::vector<Adapter> adapters;
    std::wstring selectedGuid;
    int  adapterScroll = 0;       // pixels the adapter list is scrolled
    bool draggingThumb = false;   // the list's scrollbar thumb is being dragged
    int  dragOffset = 0;          // cursor distance from the thumb's top when grabbed
    int  dns = 0;
    bool nagle = false;
    bool throttle = false;
    bool ipv6 = false;
    bool scheduler = false;
    bool timeWait = false;

    int  hoverRow = -1;
    int  hoverDns = -1;
    int  hoverToggle = -1;
    int  hoverNav = -1;           // 0 back, 1 next, 2 extra
    bool restoreArmed = false;

    std::wstring status;
    std::wstring hint;
    bool statusError = false;

    Report report;
    Layout layout;
};

App* g_app = nullptr;

void SetStatus(App& app, const std::wstring& status, const std::wstring& hint, bool error = false) {
    app.status = status;
    app.hint = hint;
    app.statusError = error;
}

void DefaultStatus(App& app) {
    switch (app.step) {
        case 0:
            SetStatus(app, L"Step 1 of 3 - choose the connection to tune.",
                      L"Every connected adapter is listed; scroll when the list runs long.");
            break;
        case 1:
            SetStatus(app, L"Step 2 of 3 - pick the switches you want.",
                      L"All five are optional. Nothing is written until you press Apply.");
            break;
        case 2:
            SetStatus(app, L"Step 3 of 3 - review, then apply.",
                      L"Apply asks for administrator rights once.");
            break;
        default:
            break;
    }
}

Plan BuildPlan(const App& app) {
    Plan plan;
    if (!app.selectedGuid.empty()) {
        plan.adapter = app.selectedGuid;
        plan.dns = app.dns;
        plan.nagle = app.nagle;
    }
    plan.throttle = app.throttle;
    plan.ipv6 = app.ipv6;
    plan.scheduler = app.scheduler;
    plan.timeWait = app.timeWait;
    return plan;
}

const Adapter* Selected(const App& app) {
    return FindAdapter(app.adapters, app.selectedGuid);
}

void RefreshAdapters(App& app) {
    app.adapters = Adapters();
    if (!FindAdapter(app.adapters, app.selectedGuid)) {
        app.selectedGuid = app.adapters.empty() ? std::wstring() : app.adapters.front().guid;
    }
    if (app.selectedGuid.empty()) {
        app.nagle = false;
        app.dns = 0;
    }
    app.adapterScroll = 0;
}

// Scroll offsets are in pixels and always clamped to what the last layout
// computed, so a resize can never leave the list scrolled past its end.
void ScrollAdapters(App& app, int delta) {
    app.adapterScroll += delta;
    app.adapterScroll = std::max(0, std::min(app.adapterScroll, app.layout.adapterScrollMax));
}

void EnsureAdapterVisible(App& app, int index) {
    const Layout& layout = app.layout;
    if (layout.adapterStride <= 0) return;
    const int top = index * layout.adapterStride;
    const int height = layout.adapterArea.bottom - layout.adapterArea.top;
    if (top < app.adapterScroll) app.adapterScroll = top;
    if (top + layout.adapterRowHeight > app.adapterScroll + height)
        app.adapterScroll = top + layout.adapterRowHeight - height;
    app.adapterScroll = std::max(0, std::min(app.adapterScroll, layout.adapterScrollMax));
}

void SelectAdapter(App& app, int index) {
    if (index < 0 || index >= static_cast<int>(app.adapters.size())) return;
    app.selectedGuid = app.adapters[static_cast<size_t>(index)].guid;
    app.nagle = false;
    app.dns = 0;
    DefaultStatus(app);
    EnsureAdapterVisible(app, index);
}

// ---------------------------------------------------------------------------
//  Layout
// ---------------------------------------------------------------------------

Layout BuildLayout(App& app, int width, int height) {
    Layout layout;
    const int margin = S(28);
    const int headerHeight = S(96);
    const int footerHeight = S(118);

    layout.header = {0, 0, width, headerHeight};
    layout.footer = {0, height - footerHeight, width, height};
    layout.content = {margin, headerHeight, width - margin, height - footerHeight};
    layout.copyright = {margin, layout.footer.top + S(88), width - margin, layout.footer.top + S(108)};
    layout.version = {width - margin - S(140), layout.footer.top + S(88), width - margin,
                      layout.footer.top + S(108)};

    // Step chips: three equal pills across the content column.
    const int chipWidth =
        std::min(S(180), static_cast<int>((layout.content.right - layout.content.left - S(24)) / 3));
    for (int i = 0; i < 3; ++i) {
        const int left = layout.content.left + i * (chipWidth + S(12));
        layout.steps[i] = {left, layout.content.top + S(8), left + chipWidth,
                           layout.content.top + S(42)};
    }

    layout.page = {layout.content.left, layout.content.top + S(58), layout.content.right,
                   layout.footer.top - S(8)};

    if (app.step == 0) {
        const int rowHeight = S(58);
        const int gap = S(8);
        const int stride = rowHeight + gap;
        layout.adapterRowHeight = rowHeight;
        layout.adapterStride = stride;

        // Bottom-up: the note sits on the page floor, the DNS pills above it,
        // the label above those, and the adapter list takes whatever is left.
        // The list scrolls, so no adapter can hide below the fold.
        layout.dnsNote = {layout.page.left, layout.page.bottom - S(34), layout.page.right,
                          layout.page.bottom};
        const int pillTop = layout.dnsNote.top - S(10) - S(40);
        const int labelTop = pillTop - S(26);
        const int listTop = layout.page.top + S(62);
        const int listBottom = std::max(listTop + rowHeight,
                                        static_cast<int>(labelTop - S(10) - S(12)));
        layout.adapterArea = {layout.page.left, listTop, layout.page.right - S(12), listBottom};

        const int count = static_cast<int>(app.adapters.size());
        const int total = count > 0 ? count * stride - gap : 0;
        const int viewHeight = layout.adapterArea.bottom - layout.adapterArea.top;
        layout.adapterScrollMax = std::max(0, total - viewHeight);
        app.adapterScroll = std::max(0, std::min(app.adapterScroll, layout.adapterScrollMax));

        layout.rows.assign(static_cast<size_t>(count), RECT{});
        for (int i = 0; i < count; ++i) {
            const int top = layout.adapterArea.top + i * stride - app.adapterScroll;
            layout.rows[static_cast<size_t>(i)] = {layout.adapterArea.left, top,
                                                   layout.adapterArea.right, top + rowHeight};
        }

        layout.hasAdapterScroll = layout.adapterScrollMax > 0;
        if (layout.hasAdapterScroll) {
            layout.adapterTrack = {layout.page.right - S(6), layout.adapterArea.top,
                                   layout.page.right, layout.adapterArea.bottom};
            const int trackHeight = layout.adapterTrack.bottom - layout.adapterTrack.top;
            const int thumbHeight = std::max(S(30), MulDiv(trackHeight, viewHeight, total));
            const int travel = trackHeight - thumbHeight;
            const int offset = MulDiv(travel, app.adapterScroll, layout.adapterScrollMax);
            layout.adapterThumb = {layout.adapterTrack.left, layout.adapterTrack.top + offset,
                                   layout.adapterTrack.right,
                                   layout.adapterTrack.top + offset + thumbHeight};
        }

        const int totalWidth = layout.page.right - layout.page.left;
        const int pillWidth = (totalWidth - S(3) * S(10)) / 4;
        for (int i = 0; i < 4; ++i) {
            const int left = layout.page.left + i * (pillWidth + S(10));
            layout.dnsPills[i] = {left, pillTop, left + pillWidth, pillTop + S(40)};
        }
    } else if (app.step == 1) {
        // Four machine-wide switches, two columns, so they fit the smallest window.
        const int gap = S(12);
        const int column = (layout.page.right - layout.page.left - gap) / 2;
        for (int i = 0; i < 4; ++i) {
            const int left = layout.page.left + (i % 2) * (column + gap);
            const int top = layout.page.top + S(64) + (i / 2) * (S(96) + gap);
            layout.toggles[i] = {left, top, left + column, top + S(96)};
        }
        // The adapter-specific Nagle switch sits with the machine-wide ones.
        layout.nagleToggle = {layout.page.left, layout.toggles[3].bottom + S(10),
                              layout.page.right, layout.toggles[3].bottom + S(70)};
    }

    // Footer buttons, right to left.
    const int top = layout.footer.top + S(12);
    const int height38 = S(38);
    const int nextWidth = app.step == 2 ? S(158) : (app.step == 3 ? S(96) : S(96));
    layout.btnNext = {layout.content.right - nextWidth, top, layout.content.right, top + height38};
    int right = layout.btnNext.left - S(10);
    if (app.step == 2 || app.step == 3) {
        const int extraWidth = S(206);
        layout.btnExtra = {right - extraWidth, top, right, top + height38};
        layout.hasExtra = true;
        right = layout.btnExtra.left - S(10);
    }
    if (app.step > 0) {
        layout.btnBack = {right - S(86), top, right, top + height38};
        layout.hasBack = true;
    }

    return layout;
}

// ---------------------------------------------------------------------------
//  Painting
// ---------------------------------------------------------------------------

void PaintSteps(HDC dc, const App& app, const Layout& layout) {
    static const wchar_t* kNames[3] = {L"1   Connection", L"2   Tuning", L"3   Review"};
    for (int i = 0; i < 3; ++i) {
        const bool active = app.step == i;
        const bool done = app.step > i;
        FillRound(dc, layout.steps[i], active ? kAccentDeep : kCard, S(8));
        if (!active) StrokeRound(dc, layout.steps[i], done ? kAccentDeep : kStroke, S(8));
        TextIn(dc, layout.steps[i], kNames[i], active ? g_fontSmallBold : g_fontSmall,
               active ? kOnAccent : (done ? kAccent : kTextDim),
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void PaintConnectionPage(HDC dc, const App& app, const Layout& layout) {
    RECT heading = {layout.page.left, layout.page.top, layout.page.right, layout.page.top + S(26)};
    TextIn(dc, heading, L"Which connection should I tune?", g_fontHeading, kText,
           DT_LEFT | DT_TOP | DT_SINGLELINE);

    if (app.adapters.empty()) {
        RECT empty = {layout.page.left, layout.page.top + S(64), layout.page.right,
                      layout.page.top + S(120)};
        TextIn(dc, empty,
               L"No connected network adapter was found. Plug one in, or connect to Wi-Fi or a "
               L"VPN, then reopen the wizard.",
               g_fontBody, kTextDim, DT_LEFT | DT_TOP | DT_WORDBREAK);
        return;
    }

    // Rows are clipped to the viewport so a scrolled list never paints over the
    // DNS block underneath it.
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, layout.adapterArea.left, layout.adapterArea.top,
                      layout.adapterArea.right, layout.adapterArea.bottom);
    for (size_t i = 0; i < layout.rows.size(); ++i) {
        const Adapter& adapter = app.adapters[i];
        const bool selected = adapter.guid == app.selectedGuid;
        const bool hot = app.hoverRow == static_cast<int>(i);
        FillRound(dc, layout.rows[i], selected ? kCardActive : (hot ? kCardHover : kCard), S(10));
        StrokeRound(dc, layout.rows[i], selected ? kAccentDeep : kStroke, S(10));

        DrawRadio(dc, layout.rows[i].left + S(24), (layout.rows[i].top + layout.rows[i].bottom) / 2,
                  selected, hot);

        RECT name = {layout.rows[i].left + S(48), layout.rows[i].top + S(10),
                     layout.rows[i].right - S(200), layout.rows[i].top + S(30)};
        TextIn(dc, name, adapter.name, g_fontBodyBold, kText,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        // The friendly name is often just "Ethernet"; the driver description says
        // which adapter it really is.
        std::wstring detail = adapter.model.empty() ? adapter.kind : adapter.model;
        if (!adapter.address.empty()) detail += L"  -  " + adapter.address;
        RECT sub = {layout.rows[i].left + S(48), layout.rows[i].top + S(32),
                    layout.rows[i].right - S(200), layout.rows[i].top + S(52)};
        TextIn(dc, sub, detail, g_fontSmall, kTextDim,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        std::wstring servers;
        for (const std::wstring& server : adapter.dns) {
            if (!servers.empty()) servers += L", ";
            servers += server;
        }
        if (servers.empty()) servers = L"DNS automatic";
        RECT dns = {layout.rows[i].right - S(210), layout.rows[i].top + S(18),
                    layout.rows[i].right - S(16), layout.rows[i].bottom - S(14)};
        TextIn(dc, dns, servers, g_fontSmall, kTextFaint,
               DT_RIGHT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    }
    RestoreDC(dc, saved);

    if (layout.hasAdapterScroll) {
        FillRound(dc, layout.adapterTrack, kCard, S(3));
        FillRound(dc, layout.adapterThumb, kTextFaint, S(3));
    }

    const int labelTop = layout.dnsPills[0].top - S(26);
    RECT label = {layout.page.left, labelTop, layout.page.right, labelTop + S(20)};
    TextIn(dc, label, L"DNS resolver for this connection", g_fontSmallBold, kTextDim,
           DT_LEFT | DT_TOP | DT_SINGLELINE);
    for (int i = 0; i < 4; ++i) {
        const bool selected = app.dns == i;
        FillRound(dc, layout.dnsPills[i], selected ? kAccentDeep : kCard, S(8));
        if (!selected) StrokeRound(dc, layout.dnsPills[i], kStroke, S(8));
        TextIn(dc, layout.dnsPills[i], DnsProviders()[static_cast<size_t>(i)].label,
               g_fontSmallBold, selected ? kOnAccent : kText,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    const std::wstring detail =
        std::wstring(L"DNS applies to this adapter only. ") +
        (app.dns == 0 ? L"Automatic keeps whatever the network hands out."
                      : std::wstring(L"Servers: ") +
                            DnsProviders()[static_cast<size_t>(app.dns)].detail + L".");
    TextIn(dc, layout.dnsNote, detail, g_fontSmall, kTextFaint,
           DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
}

void PaintTuningPage(HDC dc, const App& app, const Layout& layout) {
    RECT heading = {layout.page.left, layout.page.top, layout.page.right, layout.page.top + S(26)};
    TextIn(dc, heading, L"Tuning switches", g_fontHeading, kText,
           DT_LEFT | DT_TOP | DT_SINGLELINE);

    struct ToggleText {
        const wchar_t* title;
        const wchar_t* body;
    };
    static const ToggleText kTexts[4] = {
        {L"Turn off network throttling",
         L"While media is playing, Windows caps network traffic at about 10 packets per "
         L"millisecond. Off removes that cap for the whole PC."},
        {L"Turn off IPv6",
         L"Fixes stalls on networks whose IPv6 is broken. Needs a restart, and some VPNs or "
         L"Teredo tunnels need IPv6 - leave this off if yours does."},
        {L"Release the multimedia CPU reserve",
         L"Windows keeps a slice of the CPU for audio and video. Off gives that slice back "
         L"to games and foreground work; audio can stutter on a maximally busy PC."},
        {L"Release closed connections sooner",
         L"Caps TIME_WAIT at 30 seconds instead of the Windows default (two minutes on many "
         L"builds), so a busy client runs out of ports later. It does not change speed."},
    };
    const bool values[4] = {app.throttle, app.ipv6, app.scheduler, app.timeWait};

    for (int i = 0; i < 4; ++i) {
        const bool hot = app.hoverToggle == i;
        FillRound(dc, layout.toggles[i], hot ? kCardHover : kCard, S(10));
        StrokeRound(dc, layout.toggles[i], values[i] ? kAccentDeep : kStroke, S(10));

        RECT title = {layout.toggles[i].left + S(18), layout.toggles[i].top + S(14),
                      layout.toggles[i].right - S(90), layout.toggles[i].top + S(34)};
        TextIn(dc, title, kTexts[i].title, g_fontBodyBold, kText,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT body = {layout.toggles[i].left + S(18), layout.toggles[i].top + S(38),
                     layout.toggles[i].right - S(90), layout.toggles[i].bottom - S(12)};
        TextIn(dc, body, kTexts[i].body, g_fontBody, kTextDim,
               DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

        RECT sw = {layout.toggles[i].right - S(62), layout.toggles[i].top + S(14),
                   layout.toggles[i].right - S(18), layout.toggles[i].top + S(38)};
        DrawSwitch(dc, sw, values[i]);
    }

    // Nagle belongs to the chosen adapter, so it gets its own full-width switch
    // under the four machine-wide ones.
    {
        const RECT& card = layout.nagleToggle;
        const bool hot = app.hoverToggle == 4;
        FillRound(dc, card, hot ? kCardHover : kCard, S(10));
        StrokeRound(dc, card, app.nagle ? kAccentDeep : kStroke, S(10));
        RECT title = {card.left + S(16), card.top + S(8), card.right - S(78), card.top + S(28)};
        TextIn(dc, title, L"Nagle and delayed ACK off on the chosen connection", g_fontBodyBold,
               kText, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        const Adapter* adapter = Selected(app);
        std::wstring who = L"this adapter";
        if (adapter) who = adapter->name;
        RECT body = {card.left + S(16), card.top + S(30), card.right - S(78), card.bottom - S(6)};
        TextIn(dc, body,
               L"Small writes leave immediately instead of waiting for a full packet. " + who +
                   L" only. Off by default.",
               g_fontSmall, kTextDim,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT sw = {card.right - S(62), card.top + S(16), card.right - S(18), card.top + S(40)};
        DrawSwitch(dc, sw, app.nagle);
    }

    const LONG noteTop = layout.nagleToggle.bottom + S(14);
    if (noteTop < layout.page.bottom - S(24)) {
        RECT note = {layout.page.left, noteTop, layout.page.right, noteTop + S(64)};
        TextIn(dc, note,
               L"A restart is only needed for IPv6. Throttling, the multimedia reserve and Nagle "
               L"are read when the owning component next starts; TIME_WAIT applies to new "
               L"connections and the DNS change lands the next time the adapter connects.",
               g_fontSmall, kTextFaint, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
}

void PaintReviewPage(HDC dc, const App& app, const Layout& layout) {
    RECT heading = {layout.page.left, layout.page.top, layout.page.right, layout.page.top + S(26)};
    TextIn(dc, heading, L"Review your changes", g_fontHeading, kText,
           DT_LEFT | DT_TOP | DT_SINGLELINE);

    std::vector<std::wstring> lines;
    if (const Adapter* adapter = Selected(app)) {
        lines.push_back(L"Connection: " + adapter->name +
                        (adapter->model.empty() ? std::wstring() : L" - " + adapter->model));
    }
    for (const std::wstring& line : Describe(BuildPlan(app))) lines.push_back(line);

    // Seven lines is the full plan; tighten the rows so they still fit a small window.
    const int rowHeight = lines.size() > 6 ? S(32) : (lines.size() > 4 ? S(36) : S(42));
    const int rowGap = lines.size() > 6 ? S(4) : (lines.size() > 4 ? S(6) : S(8));
    int top = layout.page.top + S(lines.size() > 4 ? 54 : 64);
    if (lines.empty()) {
        RECT none = {layout.page.left, top, layout.page.right, top + S(28)};
        TextIn(dc, none, L"Nothing is selected, so Apply would write nothing.",
               g_fontBody, kTextDim, DT_LEFT | DT_TOP | DT_SINGLELINE);
        top += S(40);
    }
    for (const std::wstring& line : lines) {
        RECT row = {layout.page.left, top, layout.page.right, top + rowHeight};
        FillRound(dc, row, kCard, S(9));
        StrokeRound(dc, row, kStroke, S(9));
        const int dot = S(4);
        RECT marker = {row.left + S(18), (row.top + row.bottom) / 2 - dot,
                       row.left + S(18) + 2 * dot, (row.top + row.bottom) / 2 + dot};
        FillRound(dc, marker, kAccent, dot);
        RECT text = {row.left + S(36), row.top, row.right - S(16), row.bottom};
        TextIn(dc, text, line, lines.size() > 4 ? g_fontSmall : g_fontBody, kText,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        top += rowHeight + rowGap;
    }

    const LONG noteTop = std::max<LONG>(static_cast<LONG>(top + S(10)), layout.page.bottom - S(70));
    if (noteTop < layout.page.bottom - S(16)) {
        RECT note = {layout.page.left, noteTop, layout.page.right, layout.page.bottom};
        TextIn(dc, note,
               L"Apply writes and removes exactly what this page lists; a switch left off is "
               L"not touched. Restore Windows defaults puts every value back: the saved DNS "
               L"pair returns and the rest is removed.",
               g_fontSmall, kTextFaint, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
}

void PaintResultPage(HDC dc, const App& app, const Layout& layout) {
    RECT heading = {layout.page.left, layout.page.top, layout.page.right, layout.page.top + S(26)};
    const bool clean = app.report.failed == 0;
    TextIn(dc, heading, clean ? L"Done" : L"Finished with problems", g_fontHeading,
           clean ? kText : kError, DT_LEFT | DT_TOP | DT_SINGLELINE);

    int top = layout.page.top + S(60);
    for (const Line& line : app.report.lines) {
        RECT row = {layout.page.left, top, layout.page.right, top + S(36)};
        FillRound(dc, row, kCard, S(9));
        const int dot = S(4);
        RECT marker = {row.left + S(16), (row.top + row.bottom) / 2 - dot,
                       row.left + S(16) + 2 * dot, (row.top + row.bottom) / 2 + dot};
        FillRound(dc, marker, line.ok ? kOk : kError, dot);
        RECT text = {row.left + S(34), row.top, row.right - S(14), row.bottom};
        TextIn(dc, text, line.text, g_fontBody, line.ok ? kText : kError,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        top += S(36) + S(6);
    }

    const LONG noteTop = std::max<LONG>(static_cast<LONG>(top + S(12)), layout.page.bottom - S(70));
    RECT note = {layout.page.left, noteTop, layout.page.right, layout.page.bottom};
    TextIn(dc, note,
           app.report.failed == 0
               ? L"A restart finishes IPv6. DNS and Nagle apply the next time the adapter "
                 L"connects; sign out if a change seems slow to appear."
               : L"The changes above that are marked red were refused. Nothing else was rolled "
                 L"back - press Back to review, or Restore Windows defaults.",
           g_fontSmall, app.report.failed == 0 ? kTextFaint : kWarn,
           DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void PaintFooter(HDC dc, const App& app, const Layout& layout) {
    const int left = layout.content.left;
    const int right = layout.btnNext.left - S(16);
    RECT status = {left, layout.footer.top + S(14), right, layout.footer.top + S(38)};
    TextIn(dc, status, app.status, g_fontBodyBold, app.statusError ? kError : kText,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT hint = {left, layout.footer.top + S(40), right, layout.footer.top + S(60)};
    TextIn(dc, hint, app.hint, g_fontSmall, kTextDim,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (layout.hasBack) DrawButton(dc, layout.btnBack, L"Back", false, app.hoverNav == 0, true);

    if (app.step == 2) {
        DrawButton(dc, layout.btnNext, L"Apply changes", true, app.hoverNav == 1, true);
        DrawButton(dc, layout.btnExtra,
                   app.restoreArmed ? L"Click again to confirm" : L"Restore Windows defaults",
                   false, app.hoverNav == 2, true);
    } else if (app.step == 3) {
        DrawButton(dc, layout.btnNext, L"Close", true, app.hoverNav == 1, true);
        DrawButton(dc, layout.btnExtra,
                   app.restoreArmed ? L"Click again to confirm" : L"Restore Windows defaults",
                   false, app.hoverNav == 2, true);
    } else {
        DrawButton(dc, layout.btnNext, L"Next", true, app.hoverNav == 1, true);
    }

    HBRUSH rule = CreateSolidBrush(kStroke);
    RECT line = {left, layout.footer.top + S(78), layout.content.right,
                 layout.footer.top + S(78) + 1};
    FillRect(dc, &line, rule);
    DeleteObject(rule);

    TextIn(dc, layout.copyright, CopyrightText(), g_fontSmall, kTextFaint,
           DT_LEFT | DT_TOP | DT_SINGLELINE);
    TextIn(dc, layout.version, std::wstring(L"Version ") + VersionText(), g_fontSmall, kTextFaint,
           DT_RIGHT | DT_TOP | DT_SINGLELINE);
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

    app.layout = BuildLayout(app, width, height);

    RECT title = {S(28), S(22), width - S(28), S(54)};
    TextIn(memory, title, L"Internet Tuning Wizard", g_fontTitle, kText,
           DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT subtitle = {S(28), S(58), width - S(28), S(78)};
    TextIn(memory, subtitle, L"Six well-documented network switches, written and undone in one place",
           g_fontSubtitle, kTextDim, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    PaintSteps(memory, app, app.layout);
    switch (app.step) {
        case 0: PaintConnectionPage(memory, app, app.layout); break;
        case 1: PaintTuningPage(memory, app, app.layout); break;
        case 2: PaintReviewPage(memory, app, app.layout); break;
        default: PaintResultPage(memory, app, app.layout); break;
    }
    PaintFooter(memory, app, app.layout);

    BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap);
    DeleteObject(back);
    DeleteDC(memory);
}

// ---------------------------------------------------------------------------
//  Applying: relaunch elevated, wait, read the report
// ---------------------------------------------------------------------------

std::wstring SelfPath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

bool RelaunchElevated(const std::wstring& arguments, DWORD* exitCode) {
    SHELLEXECUTEINFOW run = {};
    run.cbSize = sizeof run;
    run.fMask = SEE_MASK_NOCLOSEPROCESS;
    run.lpVerb = L"runas";
    const std::wstring path = SelfPath();
    run.lpFile = path.c_str();
    run.lpParameters = arguments.c_str();
    run.nShow = SW_HIDE;
    if (!ShellExecuteExW(&run)) return false;
    if (run.hProcess) {
        WaitForSingleObject(run.hProcess, 120000);
        if (exitCode) GetExitCodeProcess(run.hProcess, exitCode);
        CloseHandle(run.hProcess);
    }
    return true;
}

void RunPlanAndShow(HWND window, App& app, const std::wstring& arguments, bool restoring) {
    const std::wstring resultPath = DefaultResultPath();
    DeleteFileW(resultPath.c_str());
    const std::wstring full = arguments + L" --result \"" + resultPath + L"\"";

    SetStatus(app, L"Writing the changes...", L"Windows is asking for administrator rights.");
    InvalidateRect(window, nullptr, FALSE);
    UpdateWindow(window);

    DWORD exitCode = 1;
    const bool launched = RelaunchElevated(full, &exitCode);
    app.report = Report();
    if (!launched) {
        app.report.lines.push_back({false, L"Administrator rights were not granted."});
        app.report.failed = 1;
    } else if (!ReadResultFile(resultPath, &app.report)) {
        app.report.lines.push_back({false, L"The elevated copy did not report back."});
        app.report.failed = 1;
    }
    DeleteFileW(resultPath.c_str());

    app.step = 3;
    if (app.report.failed == 0 && restoring) {
        SetStatus(app, L"Restored.",
                  L"Every value the wizard changed is back to how it was before.");
    } else if (app.report.failed == 0) {
        SetStatus(app, L"Applied " + std::to_wstring(app.report.changed) +
                           (app.report.changed == 1 ? L" change." : L" changes."),
                  L"Everything listed below is in effect, or starts with the next restart.");
    } else {
        SetStatus(app, L"Finished with " + std::to_wstring(app.report.failed) +
                           (app.report.failed == 1 ? L" problem." : L" problems."),
                  L"Read the red rows below for what Windows refused.", true);
    }
    RefreshAdapters(app);
    InvalidateRect(window, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  Input
// ---------------------------------------------------------------------------

void GoToStep(App& app, int step) {
    app.step = std::max(0, std::min(step, 2));
    app.restoreArmed = false;
    DefaultStatus(app);
}

void ClickNav(HWND window, App& app, int which) {
    if (which == 0) {
        GoToStep(app, app.step - 1);
    } else if (which == 1) {
        if (app.step == 2) {
            RunPlanAndShow(window, app, L"--apply \"" + EncodePlan(BuildPlan(app)) + L"\"",
                           false);
            return;
        }
        if (app.step == 3) {
            DestroyWindow(window);
            return;
        }
        GoToStep(app, app.step + 1);
    } else if (which == 2) {
        if (app.step == 2) {
            if (!app.restoreArmed) {
                app.restoreArmed = true;
                SetStatus(app, L"Restore Windows defaults?",
                          L"Press the same button again to remove every value the wizard manages.");
                SetTimer(window, 2, 4000, nullptr);
            } else {
                KillTimer(window, 2);
                app.restoreArmed = false;
                RunPlanAndShow(window, app, L"--restore \"" + app.selectedGuid + L"\"", true);
                return;
            }
        } else if (app.step == 3) {
            // Restore is reachable straight from the result page, so a run that
            // applied something can be undone without walking the wizard again.
            if (!app.restoreArmed) {
                app.restoreArmed = true;
                SetStatus(app, L"Restore Windows defaults?",
                          L"Press the same button again to remove every value the wizard manages.");
                SetTimer(window, 2, 4000, nullptr);
            } else {
                KillTimer(window, 2);
                app.restoreArmed = false;
                RunPlanAndShow(window, app, L"--restore \"" + app.selectedGuid + L"\"", true);
                return;
            }
        }
    }
    InvalidateRect(window, nullptr, FALSE);
}

void ClickPage(HWND window, App& app, POINT point) {
    if (app.step == 0) {
        // The thumb is grabbed on press and followed until the button is released.
        if (app.layout.hasAdapterScroll && Inside(app.layout.adapterThumb, point)) {
            app.draggingThumb = true;
            app.dragOffset = point.y - app.layout.adapterThumb.top;
            SetCapture(window);
            return;
        }
        if (Inside(app.layout.adapterArea, point)) {
            for (size_t i = 0; i < app.layout.rows.size(); ++i) {
                if (!Inside(app.layout.rows[i], point)) continue;
                SelectAdapter(app, static_cast<int>(i));
                InvalidateRect(window, nullptr, FALSE);
                return;
            }
            // Clicking the track pages the list; the thumb itself is left alone.
            if (app.layout.hasAdapterScroll && Inside(app.layout.adapterTrack, point) &&
                !Inside(app.layout.adapterThumb, point)) {
                const int view = app.layout.adapterArea.bottom - app.layout.adapterArea.top;
                ScrollAdapters(app, point.y < app.layout.adapterThumb.top ? -view : view);
                InvalidateRect(window, nullptr, FALSE);
                return;
            }
        }
        for (int i = 0; i < 4; ++i) {
            if (!Inside(app.layout.dnsPills[i], point)) continue;
            app.dns = i;
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
    } else if (app.step == 1) {
        for (int i = 0; i < 4; ++i) {
            if (!Inside(app.layout.toggles[i], point)) continue;
            if (i == 0) app.throttle = !app.throttle;
            else if (i == 1) app.ipv6 = !app.ipv6;
            else if (i == 2) app.scheduler = !app.scheduler;
            else app.timeWait = !app.timeWait;
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
        if (Inside(app.layout.nagleToggle, point)) {
            app.nagle = !app.nagle;
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
    }
}

void UpdateHover(App& app, POINT point) {
    app.hoverRow = -1;
    app.hoverDns = -1;
    app.hoverToggle = -1;
    app.hoverNav = -1;
    if (Inside(app.layout.btnNext, point)) app.hoverNav = 1;
    else if (app.layout.hasExtra && Inside(app.layout.btnExtra, point)) app.hoverNav = 2;
    else if (app.layout.hasBack && Inside(app.layout.btnBack, point)) app.hoverNav = 0;

    if (app.step == 0) {
        if (Inside(app.layout.adapterArea, point)) {
            for (size_t i = 0; i < app.layout.rows.size(); ++i)
                if (Inside(app.layout.rows[i], point)) app.hoverRow = static_cast<int>(i);
        }
        for (int i = 0; i < 4; ++i)
            if (Inside(app.layout.dnsPills[i], point)) app.hoverDns = i;
    } else if (app.step == 1) {
        for (int i = 0; i < 4; ++i)
            if (Inside(app.layout.toggles[i], point)) app.hoverToggle = i;
        if (Inside(app.layout.nagleToggle, point)) app.hoverToggle = 4;
    }
}

// ---------------------------------------------------------------------------
//  Window class
// ---------------------------------------------------------------------------

void UseDarkTitleBar(HWND window) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof dark);
    DwmSetWindowAttribute(window, 19, &dark, sizeof dark);
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g_dpi = QueryDpi(window);
            BuildFonts();
            return 0;

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
            info->ptMinTrackSize.x = S(900);
            info->ptMinTrackSize.y = S(660);
            return 0;
        }

        case WM_TIMER:
            if (wParam == 2 && g_app) {
                KillTimer(window, 2);
                g_app->restoreArmed = false;
                DefaultStatus(*g_app);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEMOVE: {
            if (!g_app) return 0;
            App& app = *g_app;
            const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (app.draggingThumb) {
                const RECT& track = app.layout.adapterTrack;
                const int travel = std::max(
                    1, static_cast<int>((track.bottom - track.top) -
                                        (app.layout.adapterThumb.bottom -
                                         app.layout.adapterThumb.top)));
                const int offset =
                    std::max(0, std::min(static_cast<int>(point.y - app.dragOffset - track.top),
                                         travel));
                app.adapterScroll = MulDiv(offset, app.layout.adapterScrollMax, travel);
                app.hoverRow = -1;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            UpdateHover(app, point);
            TRACKMOUSEEVENT track = {sizeof track, TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_app) {
                g_app->hoverRow = -1;
                g_app->hoverDns = -1;
                g_app->hoverToggle = -1;
                g_app->hoverNav = -1;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (g_app && g_app->step == 0 && g_app->layout.hasAdapterScroll) {
                const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
                ScrollAdapters(*g_app, -notches * g_app->layout.adapterStride * 2);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            if (!g_app) return 0;
            App& app = *g_app;
            const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (Inside(app.layout.btnNext, point)) ClickNav(window, app, 1);
            else if (app.layout.hasExtra && Inside(app.layout.btnExtra, point)) ClickNav(window, app, 2);
            else if (app.layout.hasBack && Inside(app.layout.btnBack, point)) ClickNav(window, app, 0);
            else ClickPage(window, app, point);
            return 0;
        }

        case WM_LBUTTONUP:
            if (g_app && g_app->draggingThumb) {
                App& app = *g_app;
                const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                app.draggingThumb = false;
                ReleaseCapture();
                // Lay the rows out for the new scroll before the next press is
                // hit-tested, so a click after a drag selects what it touches.
                UpdateWindow(window);
                UpdateHover(app, point);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (g_app && g_app->draggingThumb) {
                g_app->draggingThumb = false;
                UpdateWindow(window);
            }
            return 0;

        case WM_KEYDOWN:
            if (!g_app) return 0;
            if (wParam == VK_ESCAPE) {
                DestroyWindow(window);
                return 0;
            }
            if (wParam == VK_RETURN) {
                if (g_app->step == 0 || g_app->step == 1) {
                    ClickNav(window, *g_app, 1);
                } else if (g_app->step == 2) {
                    ClickNav(window, *g_app, 1);
                }
                return 0;
            }
            if ((wParam == VK_DOWN || wParam == VK_UP) && g_app->step == 0 &&
                !g_app->adapters.empty()) {
                int index = 0;
                for (size_t i = 0; i < g_app->adapters.size(); ++i)
                    if (g_app->adapters[i].guid == g_app->selectedGuid)
                        index = static_cast<int>(i);
                index += wParam == VK_DOWN ? 1 : -1;
                index = std::max(0, std::min(index,
                                            static_cast<int>(g_app->adapters.size()) - 1));
                SelectAdapter(*g_app, index);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_LEFT) {
                if (g_app->step == 0) {
                    /* adapter selection is mouse-only for clarity */
                } else {
                    ClickNav(window, *g_app, 0);
                }
                return 0;
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  Console output
// ---------------------------------------------------------------------------

void Say(const std::wstring& text) {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out && out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        DWORD wrote = 0;
        WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()), &wrote, nullptr);
        return;
    }
    if (!out || out == INVALID_HANDLE_VALUE) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes,
                        nullptr, nullptr);
    DWORD wrote = 0;
    WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote, nullptr);
}

int SelfTestMode() {
    Say(L"\r\nInternet Tuning Wizard self-test\r\n\r\n");
    int passed = 0;
    int failed = 0;
    for (const TestResult& result : RunSelfTest()) {
        Say(std::wstring(L"  [") + (result.passed ? L"PASS" : L"FAIL") + L"] " + result.name + L"\r\n");
        if (result.passed) {
            ++passed;
        } else {
            ++failed;
            if (!result.detail.empty()) Say(L"         " + result.detail + L"\r\n");
        }
    }
    Say(L"\r\n  RESULT: " + std::to_wstring(passed) + L" passed, " + std::to_wstring(failed) +
        L" failed.\r\n\r\n");
    return failed == 0 ? 0 : 1;
}

int ListMode() {
    const std::vector<Adapter> adapters = Adapters();
    Say(L"\r\nInternet Tuning Wizard - this PC\r\n\r\n");
    if (adapters.empty()) {
        Say(L"  No connected network adapter was found.\r\n\r\n");
        return 1;
    }
    for (const Adapter& adapter : adapters) {
        Say(L"  " + adapter.name + L"  (" + adapter.kind + L")  " + adapter.guid + L"\r\n");
        std::wstring row = L"      ";
        if (!adapter.address.empty()) row += adapter.address + L"   ";
        row += L"DNS: ";
        if (adapter.dns.empty()) row += L"automatic";
        for (size_t i = 0; i < adapter.dns.size(); ++i) row += (i ? L", " : L"") + adapter.dns[i];
        Say(row + L"\r\n");

        const Current state = ReadState(adapter.guid);
        Say(std::wstring(L"      Nagle: ") +
            (state.nagle ? L"off (tuned)" : L"on (default)") + L"   resolver: " +
            (state.dns == 0 ? L"automatic" : DnsProviders()[static_cast<size_t>(state.dns)].label) +
            L"\r\n");
    }
    const Current machine = ReadState(std::wstring());
    Say(std::wstring(L"\r\n  Network throttling: ") +
        (machine.throttle ? L"off (tuned)" : L"on (Windows default)") + L"\r\n");
    Say(std::wstring(L"  IPv6: ") + (machine.ipv6 ? L"off (tuned)" : L"on (Windows default)") +
        L"\r\n");
    Say(std::wstring(L"  Multimedia reserve: ") +
        (machine.scheduler ? L"off (tuned)" : L"on (Windows default)") + L"\r\n");
    Say(std::wstring(L"  TIME_WAIT: ") +
        (machine.timeWait ? L"30 s (tuned)" : L"Windows default") + L"\r\n\r\n");
    return 0;
}

int HelpMode() {
    Say(L"\r\nInternet Tuning Wizard - a three-step network tuning wizard\r\n\r\n");
    Say(L"  InternetTuningWizard.exe                        open the wizard\r\n");
    Say(L"  InternetTuningWizard.exe --list                 every adapter and what this PC reads\r\n");
    Say(L"  InternetTuningWizard.exe --selftest             prove the model and the registry engine\r\n");
    Say(L"  InternetTuningWizard.exe --help                 this text\r\n\r\n");
    Say(L"  The wizard writes six documented settings: network throttling, the multimedia\r\n");
    Say(L"  CPU reserve, TIME_WAIT, IPv6, Nagle and the DNS resolver of one adapter.\r\n");
    Say(L"  Apply asks for administrator rights;\r\n");
    Say(L"  Restore Windows defaults removes everything the wizard manages.\r\n\r\n");
    return 0;
}

// ---------------------------------------------------------------------------

bool RegisterMainClass(HINSTANCE instance, HICON* bigIcon, HICON* smallIcon) {
    *bigIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(2), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), 0));
    *smallIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(2), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof cls;
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = WndProc;
    cls.hInstance = instance;
    cls.hIcon = *bigIcon;
    cls.hIconSm = *smallIcon;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = nullptr;
    cls.lpszClassName = kClassName;
    return RegisterClassExW(&cls) != 0;
}

int RunWindow(HINSTANCE instance, int showCommand) {
    HICON bigIcon = nullptr;
    HICON smallIcon = nullptr;
    if (!RegisterMainClass(instance, &bigIcon, &smallIcon)) return 1;

    g_app = new App();
    RefreshAdapters(*g_app);
    DefaultStatus(*g_app);

    HWND window = CreateWindowExW(0, kClassName, L"Internet Tuning Wizard", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, S(1080), S(760), nullptr, nullptr,
                                  instance, nullptr);
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
    return static_cast<int>(message.wParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode;
    std::wstring planText;
    std::wstring adapterGuid;
    std::wstring resultPath;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--selftest" || arg == L"--list" || arg == L"--help" || arg == L"-h") {
            mode = arg;
        } else if (arg == L"--apply" && i + 1 < argc) {
            mode = arg;
            planText = argv[++i];
        } else if (arg == L"--restore" && i + 1 < argc) {
            mode = arg;
            adapterGuid = argv[++i];
        } else if (arg == L"--result" && i + 1 < argc) {
            resultPath = argv[++i];
        }
    }
    if (argv) LocalFree(argv);

    if (mode == L"--selftest") return SelfTestMode();
    if (mode == L"--list") return ListMode();
    if (mode == L"--help" || mode == L"-h") return HelpMode();

    if (mode == L"--apply" || mode == L"--restore") {
        Report report;
        if (mode == L"--apply") {
            Plan plan;
            if (!DecodePlan(planText, &plan)) return 2;
            report = Apply(plan);
        } else {
            report = Restore(adapterGuid);
        }
        if (!resultPath.empty()) WriteResultFile(resultPath, report);
        return report.failed == 0 ? 0 : 1;
    }

    return RunWindow(instance, showCommand);
}
