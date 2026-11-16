// ============================================================================
//  ui.cpp — custom-drawn dashboard UI.
//
//  Everything except the two DNS input fields is owner-drawn on a dark,
//  double-buffered surface: a status hero card, a provider picker card,
//  action buttons and a live adapter list.
// ============================================================================

#include "ui.h"
#include "common.h"
#include "config.h"
#include "dns.h"
#include "log.h"
#include "theme.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <utility>

namespace dzp::ui {

namespace {

// Resource / control ids ----------------------------------------------------

constexpr UINT kAppIconResource = 2;      // resources.rc
constexpr int  kTimerId         = 1;
constexpr LONG kTimerPeriodMs   = 1500;

enum ControlIds {
    IDC_EDIT_PRIMARY = 3001,
    IDC_EDIT_SECOND  = 3002,
};

enum MenuIds {        // tray menu
    IDM_SHOW   = 4001,
    IDM_TOGGLE = 4002,
    IDM_EXIT   = 4003,
};

enum HitIds {         // owner-drawn click targets
    HK_NONE      = 0,
    HK_TOGGLE    = 1,
    HK_APPLY     = 2,
    HK_STOP      = 3,
    HK_RESTORE   = 4,
    HK_PROVIDERS = 100,   // + provider index (0..8)
};

// ---------------------------------------------------------------------------
// Layout — all geometry is authored at 96 dpi and scaled at use time.
// ---------------------------------------------------------------------------

constexpr int kClientW = 640;
constexpr int kClientH = 676;

struct Layout {
    RECT Client;
    RECT HeroCard, HeroLabel, HeroStatus, HeroSub, Toggle;
    RECT ProviderCard, Rows[9], Note, EditLabel[2], EditBox[2];
    RECT Buttons[3];
    RECT AdapterCard, AdapterRow[4];
    int  width = 0, height = 0;
};

namespace grid {

RECT At(int x, int y, int w, int h) {
    return RECT{ theme::Scale(x), theme::Scale(y),
                 theme::Scale(x + w), theme::Scale(y + h) };
}

}  // namespace grid

Layout Compose() {
    using namespace grid;
    Layout L{};
    L.width    = theme::Scale(kClientW);
    L.height   = theme::Scale(kClientH);
    L.Client   = { 0, 0, L.width, L.height };

    L.HeroCard   = At(20, 68, 600, 92);
    L.HeroLabel  = RECT{ L.HeroCard.left + theme::Scale(16),
                         L.HeroCard.top  + theme::Scale(12), 0, 0 };
    L.HeroStatus = RECT{ L.HeroCard.left + theme::Scale(16),
                         L.HeroCard.top  + theme::Scale(32), 0, 0 };
    L.HeroSub    = RECT{ L.HeroCard.left + theme::Scale(16),
                         L.HeroCard.top  + theme::Scale(64), 0, 0 };
    L.Toggle     = At(20 + 600 - 16 - 56, 68 + 32, 56, 28);

    L.ProviderCard = At(20, 172, 600, 272);
    const int rowW = theme::Scale(278);
    const int rowH = theme::Scale(34);
    const int colX[2] = { L.ProviderCard.left + theme::Scale(16),
                          L.ProviderCard.left + theme::Scale(16) + rowW + theme::Scale(12) };
    const int rowY0 = L.ProviderCard.top + theme::Scale(34);
    for (int i = 0; i < 9; ++i) {
        const int col   = i < 5 ? 0 : 1;
        const int index = i < 5 ? i : i - 5;
        L.Rows[i].left   = colX[col];
        L.Rows[i].top    = rowY0 + index * rowH;
        L.Rows[i].right  = colX[col] + rowW;
        L.Rows[i].bottom = rowY0 + (index + 1) * rowH - theme::Scale(4);
    }
    // Fifth slot of the right column carries an explanatory note.
    L.Note = RECT{ colX[1], rowY0 + 4 * rowH, colX[1] + rowW, rowY0 + 5 * rowH };

    L.EditLabel[0] = RECT{ colX[0], L.ProviderCard.top + theme::Scale(214), 0, 0 };
    L.EditLabel[1] = RECT{ colX[1], L.ProviderCard.top + theme::Scale(214), 0, 0 };
    L.EditBox[0]   = RECT{ colX[0], L.ProviderCard.top + theme::Scale(232),
                           colX[0] + rowW, L.ProviderCard.top + theme::Scale(256) };
    L.EditBox[1]   = RECT{ colX[1], L.EditBox[0].top, colX[1] + rowW, L.EditBox[0].bottom };

    L.Buttons[0] = At(36, 456, 150, 34);
    L.Buttons[1] = At(196, 456, 120, 34);
    L.Buttons[2] = At(326, 456, 170, 34);

    L.AdapterCard = At(20, 502, 600, 156);
    const int rowAH = theme::Scale(26);
    for (int i = 0; i < 4; ++i) {
        L.AdapterRow[i].left   = L.AdapterCard.left + theme::Scale(16);
        L.AdapterRow[i].top    = L.AdapterCard.top + theme::Scale(34) + i * rowAH;
        L.AdapterRow[i].right  = L.AdapterCard.right - theme::Scale(16);
        L.AdapterRow[i].bottom = L.AdapterRow[i].top + rowAH;
    }
    return L;
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------

Status  g_status;                    // engine snapshot
int     g_providerIdx     = 0;       // selected provider (index into kAll)
bool    g_providerTouched = false;   // user picked a provider this session
int     g_hover           = HK_NONE;
int     g_press           = HK_NONE;
bool    g_tracked         = false;
HWND    g_edit[2]         = {};
HICON   g_appIcon         = nullptr;
HICON   g_trayIcon        = nullptr;
bool    g_trayAdded       = false;
std::vector<std::pair<RECT, int>> g_hits;

int HitTest(POINT pt) {
    for (const auto& h : g_hits) {
        if (PtInRect(&h.first, pt)) return h.second;
    }
    return HK_NONE;
}

// ---------------------------------------------------------------------------
// GDI helpers
// ---------------------------------------------------------------------------

void FillRoundRect(HDC hdc, const RECT& r, COLORREF fill, COLORREF stroke, int diameter) {
    const HBRUSH brush = CreateSolidBrush(fill);
    const HPEN   pen   = stroke ? CreatePen(PS_SOLID, 1, stroke)
                                : static_cast<HPEN>(GetStockObject(NULL_PEN));
    const HGDIOBJ oldBrush = SelectObject(hdc, brush);
    const HGDIOBJ oldPen   = SelectObject(hdc, pen);
    RoundRect(hdc, r.left, r.top, r.right + 1, r.bottom + 1, diameter, diameter);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    if (stroke) DeleteObject(pen);
}

void FrameRectC(HDC hdc, const RECT& r, COLORREF color) {
    const HPEN pen = CreatePen(PS_SOLID, 1, color);
    const HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    const HGDIOBJ oldPen   = SelectObject(hdc, pen);
    Rectangle(hdc, r.left, r.top, r.right + 1, r.bottom + 1);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void FillCircle(HDC hdc, int cx, int cy, int r, COLORREF fill) {
    const HBRUSH brush = CreateSolidBrush(fill);
    const HGDIOBJ oldBrush = SelectObject(hdc, brush);
    const HGDIOBJ oldPen   = SelectObject(hdc, GetStockObject(NULL_PEN));
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
}

void RingCircle(HDC hdc, int cx, int cy, int r, COLORREF color) {
    const HPEN pen = CreatePen(PS_SOLID, 1, color);
    const HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    const HGDIOBJ oldPen   = SelectObject(hdc, pen);
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void DrawTextC(HDC hdc, const std::wstring& s, RECT r, HFONT font,
               COLORREF color, UINT flags) {
    const HGDIOBJ oldFont = SelectObject(hdc, font);
    const int     oldMode = SetBkMode(hdc, TRANSPARENT);
    const COLORREF oldTx  = SetTextColor(hdc, color);
    DrawTextW(hdc, s.c_str(), static_cast<int>(s.size()), &r, flags);
    SetTextColor(hdc, oldTx);
    SetBkMode(hdc, oldMode);
    SelectObject(hdc, oldFont);
}

SIZE TextExtent(HDC hdc, const std::wstring& s, HFONT font) {
    const HGDIOBJ oldFont = SelectObject(hdc, font);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, s.c_str(), static_cast<int>(s.size()), &sz);
    SelectObject(hdc, oldFont);
    return sz;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

struct ButtonSpec {
    const RECT*    rect;
    const wchar_t* label;
    bool           primary;
    bool           enabled;
};

void DrawButton(HDC hdc, const ButtonSpec& b, bool hover, bool press) {
    const auto& P = theme::Pal();
    if (b.primary) {
        const COLORREF fill = press ? P.AccentPress : hover ? P.AccentHover : P.Accent;
        FillRoundRect(hdc, *b.rect, fill, 0, theme::Scale(14));
        DrawTextC(hdc, b.label, *b.rect, theme::Font(theme::FontKind::Body), P.OnAccent,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    if (b.enabled) {
        FillRoundRect(hdc, *b.rect, press ? P.CardSelected : P.Card,
                      0, theme::Scale(14));
        DrawTextC(hdc, b.label, *b.rect, theme::Font(theme::FontKind::Body),
                  hover ? P.Text : P.TextDim,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        FillRoundRect(hdc, *b.rect, P.Card, 0, theme::Scale(14));
        DrawTextC(hdc, b.label, *b.rect, theme::Font(theme::FontKind::Body),
                  P.TextDisabled,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

void DrawToggle(HDC hdc, const RECT& r, bool on, bool hover) {
    const auto& P = theme::Pal();
    const int h = r.bottom - r.top;
    const int knobR = h / 2 - theme::Scale(3);
    const int cy = (r.top + r.bottom) / 2;
    const COLORREF track = on ? (hover ? P.AccentHover : P.Accent) : P.CardHover;
    FillRoundRect(hdc, r, track, on ? 0 : P.Stroke, h);
    const int kx = on ? r.right - knobR - theme::Scale(3)
                      : r.left + knobR + theme::Scale(3);
    FillCircle(hdc, kx, cy, knobR, on ? P.OnAccent : P.TextDisabled);
}

std::wstring HeroSubText(const Status& st, int trackedCount) {
    if (!st.Elevated) {
        return L"Administrator rights are required to enable protection.";
    }
    if (!st.Enabled) {
        return L"DNS changes are not being monitored. "
               L"Pick a provider below and press Apply & protect.";
    }
    std::wstring sub;
    if (st.Provider == provider::kSystem) {
        sub = L"Keeping your current DNS settings under watch";
    } else if (st.Provider == provider::kCustom) {
        sub = Fmt(L"Enforcing custom servers (%ls / %ls) across IP-enabled adapters",
                  st.CustomPrimary.c_str(), st.CustomSecondary.c_str());
    } else if (const Provider* p = provider::ById(st.Provider)) {
        sub = Fmt(L"Enforcing %ls on %d adapter(s)", p->Short, trackedCount);
    }
    if (st.HijacksReverted > 0) {
        sub += Fmt(L"   %c   %llu hijack attempt%ls reverted",
                   0x2022, st.HijacksReverted,
                   st.HijacksReverted == 1 ? L"" : L"s");
    }
    return sub;
}

void Paint(HDC hdc, const Layout& L) {
    const auto& P = theme::Pal();
    const Status& st = g_status;

    // background
    {
        const HBRUSH brush = CreateSolidBrush(P.Background);
        const HGDIOBJ old = SelectObject(hdc, brush);
        PatBlt(hdc, 0, 0, L.width, L.height, PATCOPY);
        SelectObject(hdc, old);
        DeleteObject(brush);
    }

    g_hits.clear();
    auto addHit = [](const RECT& r, int id) { g_hits.emplace_back(r, id); };

    // --- header -------------------------------------------------------------
    if (g_appIcon) {
        const int sz = theme::Scale(36);
        DrawIconEx(hdc, theme::Scale(20), theme::Scale(16), g_appIcon,
                   sz, sz, 0, nullptr, DI_NORMAL);
    }
    {
        const RECT title{ theme::Scale(68), theme::Scale(14),
                          theme::Scale(668), theme::Scale(44) };
        DrawTextC(hdc, kAppTitle, title, theme::Font(theme::FontKind::Title), P.Text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const RECT sub{ theme::Scale(70), theme::Scale(46),
                        theme::Scale(700), theme::Scale(64) };
        DrawTextC(hdc, L"Protects your DNS configuration from unauthorized changes.",
                  sub, theme::Font(theme::FontKind::Small), P.TextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // --- hero card ------------------------------------------------------------
    FillRoundRect(hdc, L.HeroCard, P.Card, P.Stroke, theme::Scale(16));

    DrawTextC(hdc, L"DNS PROTECTION",
              { L.HeroLabel.left, L.HeroLabel.top,
                L.HeroLabel.left + theme::Scale(300), L.HeroLabel.top + theme::Scale(16) },
              theme::Font(theme::FontKind::Small), P.TextDim, DT_LEFT | DT_SINGLELINE);

    int mismatches = 0, tracked = 0;
    for (const auto& a : st.Adapters) {
        if (a.State != AdapterState::Untracked) ++tracked;
        if (a.State == AdapterState::Mismatch)  ++mismatches;
    }

    std::wstring statusText;
    COLORREF statusColor;
    if (!st.Elevated || !st.Enabled) {
        statusColor = P.TextDim;
        statusText  = L"Unprotected";
    } else if (mismatches > 0) {
        statusColor = P.Warn;
        statusText  = L"Reverting changes";
    } else {
        statusColor = P.Ok;
        statusText  = L"Protected";
    }
    DrawTextC(hdc, statusText,
              { L.HeroStatus.left, L.HeroStatus.top,
                L.HeroStatus.left + theme::Scale(380), L.HeroStatus.top + theme::Scale(30) },
              theme::Font(theme::FontKind::Big), statusColor, DT_LEFT | DT_SINGLELINE);

    DrawTextC(hdc, HeroSubText(st, tracked),
              { L.HeroSub.left, L.HeroSub.top,
                L.HeroCard.right - theme::Scale(104), L.HeroSub.top + theme::Scale(18) },
              theme::Font(theme::FontKind::Small), P.TextDim,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    DrawToggle(hdc, L.Toggle, st.Enabled, g_hover == HK_TOGGLE);
    addHit(L.Toggle, HK_TOGGLE);

    // --- provider card ----------------------------------------------------------
    FillRoundRect(hdc, L.ProviderCard, P.Card, P.Stroke, theme::Scale(16));
    DrawTextC(hdc, L"DNS PROVIDER",
              { L.ProviderCard.left + theme::Scale(16), L.ProviderCard.top + theme::Scale(12),
                L.ProviderCard.left + theme::Scale(300), L.ProviderCard.top + theme::Scale(28) },
              theme::Font(theme::FontKind::Small), P.TextDim, DT_LEFT | DT_SINGLELINE);

    const int customIdx = provider::IndexOf(provider::kCustom);
    const bool editCustom = g_providerIdx == customIdx;
    for (int i = 0; i < 9; ++i) {
        const RECT& r = L.Rows[i];
        const bool selected = i == g_providerIdx;
        const bool hovered = g_hover == HK_PROVIDERS + i;
        const Provider& pv = provider::kAll[i];

        FillRoundRect(hdc, r,
                      selected ? P.CardSelected : hovered ? P.CardHover : P.Card,
                      selected ? P.Accent : 0, theme::Scale(12));
        const int cy = (r.top + r.bottom) / 2;
        const int cx = r.left + theme::Scale(14);
        if (selected) FillCircle(hdc, cx, cy, theme::Scale(4), P.Accent);
        else          RingCircle(hdc, cx, cy, theme::Scale(4), P.Stroke);
        DrawTextC(hdc, pv.Title,
                  { r.left + theme::Scale(28), r.top, r.right - theme::Scale(6), r.bottom },
                  theme::Font(theme::FontKind::Body),
                  selected || hovered ? P.Text : P.TextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        addHit(r, HK_PROVIDERS + i);
    }
    DrawTextC(hdc, L"Enforced on all IP-enabled IPv4 interfaces.",
              { L.Note.left, L.Note.top, L.Note.right, L.Note.bottom },
              theme::Font(theme::FontKind::Small), P.TextDisabled,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // custom inputs: labels, focus frames, validity dots
    const wchar_t* editLabels[2] = { L"Preferred DNS server", L"Alternate DNS server" };
    for (int i = 0; i < 2; ++i) {
        DrawTextC(hdc, editLabels[i],
                  { L.EditLabel[i].left, L.EditLabel[i].top,
                    L.EditLabel[i].left + theme::Scale(220), L.EditLabel[i].top + theme::Scale(16) },
                  theme::Font(theme::FontKind::Small),
                  editCustom ? P.TextDim : P.TextDisabled, DT_LEFT | DT_SINGLELINE);

        const bool focused = GetFocus() == g_edit[i];
        FrameRectC(hdc, L.EditBox[i],
                   focused ? P.Accent : editCustom ? P.Stroke : P.CardSelected);
        if (editCustom) {
            wchar_t buf[260] = {};
            GetWindowTextW(g_edit[i], buf, 260);
            const std::wstring v = Trim(buf);
            const int cx = L.EditBox[i].right - theme::Scale(12);
            const int cy = (L.EditBox[i].top + L.EditBox[i].bottom) / 2;
            if (v.empty())            FillCircle(hdc, cx, cy, theme::Scale(3), P.Stroke);
            else if (IsValidIpv4(v))  FillCircle(hdc, cx, cy, theme::Scale(3), P.Ok);
            else                      FillCircle(hdc, cx, cy, theme::Scale(3), P.Danger);
        }
    }

    // --- buttons -----------------------------------------------------------------
    {
        const ButtonSpec apply{ &L.Buttons[0],
                                st.Enabled ? L"Enforce provider" : L"Apply & protect",
                                true, true };
        const ButtonSpec stop{ &L.Buttons[1], L"Stop protection", false, st.Enabled };
        const ButtonSpec restore{ &L.Buttons[2], L"Restore original settings", false, true };
        DrawButton(hdc, apply,   g_hover == HK_APPLY,   g_press == HK_APPLY);
        DrawButton(hdc, stop,    g_hover == HK_STOP,    g_press == HK_STOP);
        DrawButton(hdc, restore, g_hover == HK_RESTORE, g_press == HK_RESTORE);
        addHit(L.Buttons[0], HK_APPLY);
        addHit(L.Buttons[1], HK_STOP);
        addHit(L.Buttons[2], HK_RESTORE);
    }

    // --- adapter card --------------------------------------------------------------
    FillRoundRect(hdc, L.AdapterCard, P.Card, P.Stroke, theme::Scale(16));
    DrawTextC(hdc, L"NETWORK ADAPTERS",
              { L.AdapterCard.left + theme::Scale(16), L.AdapterCard.top + theme::Scale(12),
                L.AdapterCard.left + theme::Scale(300), L.AdapterCard.top + theme::Scale(28) },
              theme::Font(theme::FontKind::Small), P.TextDim, DT_LEFT | DT_SINGLELINE);

    const int count = static_cast<int>(st.Adapters.size());
    if (count == 0) {
        DrawTextC(hdc, L"No IP-enabled adapters found.",
                  { L.AdapterRow[0].left, L.AdapterRow[0].top,
                    L.AdapterRow[0].right, L.AdapterRow[0].bottom },
                  theme::Font(theme::FontKind::Body), P.TextDisabled,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    for (int i = 0; i < 4; ++i) {
        const RECT r = L.AdapterRow[i];
        if (i == 3 && count > 4) {
            DrawTextC(hdc, Fmt(L"+%d more adapter(s)", count - 3),
                      { r.left + theme::Scale(18), r.top, r.right, r.bottom },
                      theme::Font(theme::FontKind::Small), P.TextDisabled,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            continue;
        }
        if (i >= count) break;
        const IfaceView& v = st.Adapters[i];

        COLORREF dot = P.Stroke;
        if (v.State == AdapterState::Mismatch) dot = P.Danger;
        else if (v.State == AdapterState::Ok)  dot = P.Ok;
        FillCircle(hdc, r.left + theme::Scale(4), (r.top + r.bottom) / 2,
                   theme::Scale(3), dot);

        const std::wstring tag = v.adapter.Dhcp ? L"DHCP" : L"static";
        const std::wstring dns = !v.adapter.NameServer.empty()
                                     ? v.adapter.NameServer
                                     : !v.adapter.DhcpNameServer.empty()
                                           ? v.adapter.DhcpNameServer : L"(automatic)";
        const COLORREF nameColor = v.adapter.Up
                                       ? (v.State == AdapterState::Untracked ? P.TextDim : P.Text)
                                       : P.TextDisabled;
        {
            RECT nameR{ r.left + theme::Scale(14), r.top,
                        r.right - theme::Scale(60), r.bottom };
            DrawTextC(hdc, v.adapter.Name, nameR, theme::Font(theme::FontKind::Body),
                      nameColor,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            const SIZE sz = TextExtent(hdc, v.adapter.Name, theme::Font(theme::FontKind::Body));
            RECT tagR{ nameR.left + sz.cx + theme::Scale(8), r.top, nameR.right, r.bottom };
            DrawTextC(hdc, tag, tagR, theme::Font(theme::FontKind::Small), P.TextDisabled,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        RECT dnsR{ r.right - theme::Scale(230), r.top, r.right, r.bottom };
        DrawTextC(hdc, v.adapter.Up ? dns : L"offline",
                  dnsR, theme::Font(theme::FontKind::Body),
                  v.adapter.Up ? P.TextDim : P.TextDisabled,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void RefreshSnapshot(HWND hwnd) {
    g_status = Engine::Instance().Snapshot();
    if (!g_providerTouched) g_providerIdx = provider::IndexOf(g_status.Provider);
    const bool custom = g_providerIdx == provider::IndexOf(provider::kCustom);
    for (HWND e : g_edit) EnableWindow(e, custom);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void TryEnable(HWND hwnd) {
    std::wstring id, p, s;
    if (g_providerIdx == provider::IndexOf(provider::kSystem)) {
        id = provider::kSystem;
    } else if (g_providerIdx == provider::IndexOf(provider::kCustom)) {
        id = provider::kCustom;
        wchar_t b1[260] = {}, b2[260] = {};
        GetWindowTextW(g_edit[0], b1, 260);
        GetWindowTextW(g_edit[1], b2, 260);
        p = Trim(b1);
        s = Trim(b2);
    } else {
        id = provider::kAll[g_providerIdx].Id;
    }
    const std::wstring err = Engine::Instance().Enable(id, p, s);
    if (!err.empty()) {
        MessageBoxW(hwnd, err.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
    }
}

void ToggleProtection(HWND hwnd) {
    if (g_status.Enabled) Engine::Instance().Disable();
    else                  TryEnable(hwnd);
    RefreshSnapshot(hwnd);
}

void PositionEdits(const Layout& L) {
    for (int i = 0; i < 2; ++i) {
        SetWindowPos(g_edit[i], nullptr,
                     L.EditBox[i].left, L.EditBox[i].top,
                     L.EditBox[i].right - L.EditBox[i].left,
                     L.EditBox[i].bottom - L.EditBox[i].top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(g_edit[i], WM_SETFONT,
                     (WPARAM)theme::Font(theme::FontKind::Body), TRUE);
    }
}

void SizeAndCenter(HWND hwnd) {
    Layout L = Compose();
    const DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    RECT r{ 0, 0, L.width, L.height };
    AdjustWindowRect(&r, style, FALSE);
    const int w = r.right - r.left, h = r.bottom - r.top;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void LoadIcons(HWND hwnd) {
    const HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    if (g_appIcon) { DestroyIcon(g_appIcon); g_appIcon = nullptr; }
    g_appIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(kAppIconResource),
                                  IMAGE_ICON, theme::Scale(36), theme::Scale(36), 0);
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = nullptr; }
    g_trayIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(kAppIconResource),
                                   IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
}

void AddRemoveTray(HWND hwnd, bool add) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_trayIcon;
    lstrcpynW(nid.szTip, kAppTitle, 128);
    Shell_NotifyIconW(add ? NIM_ADD : NIM_DELETE, &nid);
    g_trayAdded = add;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        theme::SetupFor(hwnd);

        // dark title bar + rounded corners (best effort across Windows builds)
        BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                                         &dark, sizeof(dark)))) {
            DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
        }
        INT corner = 2 /*DWMWCP_ROUND*/;
        DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                              &corner, sizeof(corner));

        const DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
        g_edit[0] = CreateWindowExW(0, L"Edit", L"", editStyle, 0, 0, 10, 10,
                                    hwnd, (HMENU)(INT_PTR)IDC_EDIT_PRIMARY,
                                    nullptr, nullptr);
        g_edit[1] = CreateWindowExW(0, L"Edit", L"", editStyle, 0, 0, 10, 10,
                                    hwnd, (HMENU)(INT_PTR)IDC_EDIT_SECOND,
                                    nullptr, nullptr);

        Layout L = Compose();
        PositionEdits(L);
        SizeAndCenter(hwnd);
        LoadIcons(hwnd);
        AddRemoveTray(hwnd, true);

        SetTimer(hwnd, kTimerId, kTimerPeriodMs, nullptr);
        Engine::Instance().SetNotify(hwnd);
        RefreshSnapshot(hwnd);

        // sync persisted custom values into the editors
        wchar_t b1[260] = {}, b2[260] = {};
        lstrcpynW(b1, g_status.CustomPrimary.c_str(), 260);
        lstrcpynW(b2, g_status.CustomSecondary.c_str(), 260);
        SetWindowTextW(g_edit[0], b1);
        SetWindowTextW(g_edit[1], b2);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        const Layout L = Compose();
        const int w = L.width, h = L.height;

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bm = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBm = SelectObject(mem, bm);
        Paint(mem, L);
        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBm);
        DeleteObject(bm);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        const auto& P = theme::Pal();
        const bool focused = GetFocus() == (HWND)lParam;
        SetTextColor(hdc, focused ? P.Text : P.TextDisabled);
        SetBkColor(hdc, P.Card);
        static HBRUSH cardBrush = CreateSolidBrush(P.Card);
        return (LRESULT)cardBrush;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDC_EDIT_PRIMARY || id == IDC_EDIT_SECOND) {
            // repaint the focus outline / validity dot around the edit box
            const Layout L = Compose();
            RECT rr = id == IDC_EDIT_PRIMARY ? L.EditBox[0] : L.EditBox[1];
            rr.left -= 2; rr.top -= 2; rr.right += 2; rr.bottom += 2;
            InvalidateRect(hwnd, &rr, FALSE);
            return 0;
        }
        switch (id) {
        case IDM_SHOW:
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return 0;
        case IDM_TOGGLE:
            ToggleProtection(hwnd);
            return 0;
        case IDM_EXIT:
            if (g_status.Enabled) {
                const int result = MessageBoxW(hwnd,
                    L"DNS protection is currently active.\n"
                    L"If DNS Protect exits, your settings will no longer be "
                    L"protected. Exit anyway?",
                    kAppTitle, MB_YESNO | MB_ICONQUESTION);
                if (result != IDYES) return 0;
                Engine::Instance().Disable();
            }
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_TIMER:
    case WM_APP_REFRESH:
        RefreshSnapshot(hwnd);
        return 0;

    case WM_MOUSEMOVE: {
        const POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int h = HitTest(pt);
        if (h != g_hover) {
            g_hover = h;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!g_tracked) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_tracked = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracked = false;
        if (g_hover != HK_NONE) {
            g_hover = HK_NONE;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        const POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        g_press = HitTest(pt);
        if (g_press != HK_NONE) SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        const int press = g_press;
        g_press = HK_NONE;
        if (GetCapture() == hwnd) ReleaseCapture();
        const POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int h = HitTest(pt);
        InvalidateRect(hwnd, nullptr, FALSE);
        if (h != press || h == HK_NONE) return 0;

        if (h == HK_TOGGLE) {
            ToggleProtection(hwnd);
        } else if (h == HK_APPLY) {
            TryEnable(hwnd);
            RefreshSnapshot(hwnd);
        } else if (h == HK_STOP) {
            Engine::Instance().Disable();
        } else if (h == HK_RESTORE) {
            const std::wstring msg = Engine::Instance().RestoreOriginal();
            if (!msg.empty()) {
                MessageBoxW(hwnd, msg.c_str(), kAppTitle, MB_OK | MB_ICONINFORMATION);
            }
        } else if (h >= HK_PROVIDERS && h < HK_PROVIDERS + provider::kCount) {
            g_providerIdx = h - HK_PROVIDERS;
            g_providerTouched = true;
            RefreshSnapshot(hwnd);
        }
        return 0;
    }

    case WM_APP_TRAY:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, IDM_SHOW, L"Show window");
            AppendMenuW(m, MF_STRING | (g_status.Enabled ? MF_CHECKED : MF_UNCHECKED),
                        IDM_TOGGLE,
                        g_status.Enabled ? L"Protection is on (turn off)"
                                         : L"Protection is off (turn on)");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
            const int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            if (cmd) SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
        }
        return 0;

    case WM_DPICHANGED: {
        theme::SetupFor(hwnd);
        const RECT* suggested = (const RECT*)lParam;
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout L = Compose();
        PositionEdits(L);
        LoadIcons(hwnd);
        if (g_trayAdded) { AddRemoveTray(hwnd, false); AddRemoveTray(hwnd, true); }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        if (g_trayAdded) AddRemoveTray(hwnd, false);
        if (g_appIcon)  DestroyIcon(g_appIcon);
        if (g_trayIcon) DestroyIcon(g_trayIcon);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int Run(HINSTANCE instance, int cmdShow) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    const HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW, kWindowClass, kAppTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        theme::Scale(kClientW), theme::Scale(kClientH),
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, cmdShow ? cmdShow : SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

}  // namespace dzp::ui
