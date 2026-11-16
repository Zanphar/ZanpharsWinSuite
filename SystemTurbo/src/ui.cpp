// ============================================================================
//  ui.cpp — custom-drawn dashboard UI.
//
//  Everything is owner-drawn on a dark, double-buffered surface: a turbo
//  hero card, an option picker card, action buttons and a live memory & CPU
//  instrumentation card with a top-process list.
// ============================================================================

#include "ui.h"
#include "common.h"
#include "config.h"
#include "theme.h"
#include "turbo.h"
#include "log.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <utility>

namespace stc::ui {

namespace {

// Resource / control ids ----------------------------------------------------

constexpr UINT kAppIconResource = 2;      // resources.rc
constexpr int  kTimerId         = 1;
constexpr LONG kTimerPeriodMs   = 1500;

enum MenuIds {        // tray menu
    IDM_SHOW   = 4001,
    IDM_TOGGLE = 4002,
    IDM_FREE   = 4003,
    IDM_EXIT   = 4004,
};

enum HitIds {         // owner-drawn click targets
    HK_NONE      = 0,
    HK_TOGGLE    = 1,
    HK_BOOST     = 2,
    HK_OFF       = 3,
    HK_FREE      = 4,
    HK_AUTOTOG   = 5,
    HK_OPTIONS   = 100,   // + option index (0..3)
};

// ---------------------------------------------------------------------------
// Layout — all geometry is authored at 96 dpi and scaled at use time.
// ---------------------------------------------------------------------------

constexpr int kClientW   = 660;
constexpr int kClientH   = 752;
constexpr int kOptCount  = 4;
constexpr int kProcCount = 3;

struct Layout {
    RECT Client;
    RECT HeroCard, HeroLabel, HeroStatus, HeroSub, Toggle;
    RECT OptionCard, Rows[kOptCount];
    RECT Buttons[3];
    RECT RamCard, RamTrack, CpuTrack, AutoRow, AutoToggle;
    RECT ProcRow[kProcCount];
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

    L.HeroCard   = At(20, 64, 620, 92);
    L.HeroLabel  = RECT{ L.HeroCard.left + theme::Scale(16),
                         L.HeroCard.top  + theme::Scale(12), 0, 0 };
    L.HeroStatus = RECT{ L.HeroCard.left + theme::Scale(16),
                         L.HeroCard.top  + theme::Scale(32), 0, 0 };
    L.HeroSub    = RECT{ L.HeroCard.left + theme::Scale(16),
                         L.HeroCard.top  + theme::Scale(62), 0, 0 };
    L.Toggle     = At(20 + 620 - 16 - 56, 64 + 32, 56, 28);

    L.OptionCard = At(20, 168, 620, 224);
    for (int i = 0; i < kOptCount; ++i) {
        const int top = L.OptionCard.top + theme::Scale(38 + 44 * i);
        L.Rows[i] = RECT{ L.OptionCard.left + theme::Scale(12), top,
                          L.OptionCard.right - theme::Scale(12),
                          top + theme::Scale(40) };
    }

    L.Buttons[0] = At(36, 404, 150, 34);
    L.Buttons[1] = At(196, 404, 170, 34);
    L.Buttons[2] = At(376, 404, 170, 34);

    L.RamCard  = At(20, 450, 620, 286);
    const int rx = L.RamCard.left + theme::Scale(18);
    const int rw = L.RamCard.right - L.RamCard.left - theme::Scale(36);
    const int ry = L.RamCard.top;
    L.RamTrack = RECT{ rx, ry + theme::Scale(54), rx + rw, ry + theme::Scale(68) };
    L.CpuTrack = RECT{ rx, ry + theme::Scale(96), rx + rw, ry + theme::Scale(110) };
    L.AutoRow    = RECT{ rx, ry + theme::Scale(138), rx + rw, ry + theme::Scale(164) };
    L.AutoToggle = RECT{ L.AutoRow.right - theme::Scale(44), L.AutoRow.top + theme::Scale(2),
                         L.AutoRow.right, L.AutoRow.top + theme::Scale(24) };
    for (int i = 0; i < kProcCount; ++i) {
        const int top = ry + theme::Scale(200 + 26 * i);
        L.ProcRow[i] = RECT{ rx, top, rx + rw, top + theme::Scale(26) };
    }
    return L;
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------

Status  g_status;
int     g_hover   = HK_NONE;
int     g_press   = HK_NONE;
bool    g_tracked = false;
HICON   g_appIcon  = nullptr;
HICON   g_trayIcon = nullptr;
bool    g_trayAdded = false;
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

void FillRectC(HDC hdc, const RECT& r, COLORREF fill) {
    const HBRUSH brush = CreateSolidBrush(fill);
    const HGDIOBJ old = SelectObject(hdc, brush);
    PatBlt(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top, PATCOPY);
    SelectObject(hdc, old);
    DeleteObject(brush);
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

struct OptInfo {
    const wchar_t* Title;
    const wchar_t* Desc;
};

OptInfo OptionAt(int i) {
    static constexpr OptInfo kOptions[kOptCount] = {
        { L"High performance power plan",
          L"Switches the active scheme to High Performance while boosting." },
        { L"Pause background services",
          L"Windows Search, SysMain, Spooler, Bluetooth, Diagnostics, Fax, WMP Sharing." },
        { L"Disable Game DVR recording",
          L"Stops background gameplay recording and its scheduling bits." },
        { L"Free RAM when boosting",
          L"Runs a full memory release pass while the boost kicks in." },
    };
    return kOptions[i];
}

std::wstring DetailText(const Status& st) {
    if (!st.Elevated)
        return L"Administrator rights are required to apply turbo tweaks.";
    if (!st.Turbo)
        return L"Boost is off. Pick options below and press Turbo now.";

    std::wstring sub;
    auto join = [&](const std::wstring& piece) {
        if (piece.empty()) return;
        if (!sub.empty()) sub += L"   \x2022   ";
        sub += piece;
    };
    if (st.TweakMask & MaskPowerPlan) join(L"High performance plan");
    if (st.TweakMask & MaskServices)
        join(st.ParkedCount > 0
                 ? Fmt(L"%d service(s) parked", st.ParkedCount)
                 : L"services parked");
    if (st.TweakMask & MaskGameDVR) join(L"Game DVR off");
    if (st.TweakMask & MaskRamOnBoost) join(L"RAM trimmed");
    if (st.LastFreedBytes > 0)
        join(Fmt(L"%ls freed", FormatBytes(st.LastFreedBytes).c_str()));
    if (sub.empty()) sub = L"Boost active";
    return sub;
}

std::wstring HeroStatusText(const Status& st) {
    if (!st.Elevated) return L"Restricted";
    return st.Turbo ? L"Boosted" : L"Standing by";
}

void Paint(HDC hdc, const Layout& L) {
    const auto& P = theme::Pal();
    const Status& st = g_status;

    // background
    FillRectC(hdc, L.Client, P.Background);

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
                          theme::Scale(680), theme::Scale(44) };
        DrawTextC(hdc, kAppTitle, title, theme::Font(theme::FontKind::Title), P.Text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const RECT sub{ theme::Scale(70), theme::Scale(46),
                        theme::Scale(720), theme::Scale(64) };
        DrawTextC(hdc, L"Unties RAM and power so your apps get the machine.",
                  sub, theme::Font(theme::FontKind::Small), P.TextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // --- hero card ------------------------------------------------------------
    FillRoundRect(hdc, L.HeroCard, P.Card, P.Stroke, theme::Scale(16));

    DrawTextC(hdc, L"TURBO BOOST",
              { L.HeroLabel.left, L.HeroLabel.top,
                L.HeroLabel.left + theme::Scale(300), L.HeroLabel.top + theme::Scale(16) },
              theme::Font(theme::FontKind::Small), P.TextDim, DT_LEFT | DT_SINGLELINE);

    const bool boosted = st.Turbo;
    DrawTextC(hdc, HeroStatusText(st),
              { L.HeroStatus.left, L.HeroStatus.top,
                L.HeroStatus.left + theme::Scale(380), L.HeroStatus.top + theme::Scale(30) },
              theme::Font(theme::FontKind::Big), boosted ? P.Ok : P.TextDim,
              DT_LEFT | DT_SINGLELINE);

    DrawTextC(hdc, DetailText(st),
              { L.HeroSub.left, L.HeroSub.top,
                L.HeroCard.right - theme::Scale(104), L.HeroSub.top + theme::Scale(18) },
              theme::Font(theme::FontKind::Small), P.TextDim,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    DrawToggle(hdc, L.Toggle, boosted, g_hover == HK_TOGGLE);
    addHit(L.Toggle, HK_TOGGLE);

    // --- option card ------------------------------------------------------------------
    FillRoundRect(hdc, L.OptionCard, P.Card, P.Stroke, theme::Scale(16));
    DrawTextC(hdc, L"BOOST OPTIONS",
              { L.OptionCard.left + theme::Scale(16), L.OptionCard.top + theme::Scale(12),
                L.OptionCard.left + theme::Scale(320), L.OptionCard.top + theme::Scale(28) },
              theme::Font(theme::FontKind::Small), P.TextDim, DT_LEFT | DT_SINGLELINE);
    DrawTextC(hdc, st.Turbo ? L"Unticking a group undoes it on the next power-off."
                            : L"Tick the groups a boost should apply.",
              { L.OptionCard.left, L.OptionCard.top + theme::Scale(12),
                L.OptionCard.right - theme::Scale(16), L.OptionCard.top + theme::Scale(28) },
              theme::Font(theme::FontKind::Small), P.TextDisabled,
              DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);

    for (int i = 0; i < kOptCount; ++i) {
        const RECT r = L.Rows[i];
        const bool on = (st.TweakMask & (1u << i)) != 0;
        const bool hovered = g_hover == HK_OPTIONS + i;

        // row background
        FillRoundRect(hdc, r, hovered ? P.CardHover : P.Card, 0, theme::Scale(12));

        const OptInfo info = OptionAt(i);
        DrawTextC(hdc, info.Title,
                  { r.left + theme::Scale(16), r.top + theme::Scale(2),
                    r.right - theme::Scale(70), r.top + theme::Scale(20) },
                  theme::Font(theme::FontKind::Body), on ? P.Text : P.TextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        DrawTextC(hdc, info.Desc,
                  { r.left + theme::Scale(16), r.top + theme::Scale(20),
                    r.right - theme::Scale(70), r.top + theme::Scale(38) },
                  theme::Font(theme::FontKind::Small), P.TextDisabled,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        const RECT tgl{ r.right - theme::Scale(56), (r.top + r.bottom) / 2 - theme::Scale(11),
                        r.right - theme::Scale(12), (r.top + r.bottom) / 2 + theme::Scale(11) };
        DrawToggle(hdc, tgl, on, hovered);
        addHit(r, HK_OPTIONS + i);
    }

    // --- buttons -----------------------------------------------------------------
    {
        const ButtonSpec boost{ &L.Buttons[0], L"Turbo now", true, !boosted };
        const ButtonSpec off{ &L.Buttons[1], L"Turn off & restore", false, boosted };
        const ButtonSpec freeBtn{ &L.Buttons[2], L"Free memory now", false, true };
        DrawButton(hdc, boost,   g_hover == HK_BOOST, g_press == HK_BOOST);
        DrawButton(hdc, off,     g_hover == HK_OFF,   g_press == HK_OFF);
        DrawButton(hdc, freeBtn, g_hover == HK_FREE,  g_press == HK_FREE);
        addHit(L.Buttons[0], HK_BOOST);
        addHit(L.Buttons[1], HK_OFF);
        addHit(L.Buttons[2], HK_FREE);
    }

    // --- memory & CPU card ---------------------------------------------------------
    FillRoundRect(hdc, L.RamCard, P.Card, P.Stroke, theme::Scale(16));
    DrawTextC(hdc, L"MEMORY & CPU",
              { L.RamCard.left + theme::Scale(16), L.RamCard.top + theme::Scale(12),
                L.RamCard.left + theme::Scale(320), L.RamCard.top + theme::Scale(28) },
              theme::Font(theme::FontKind::Small), P.TextDim, DT_LEFT | DT_SINGLELINE);
    DrawTextC(hdc, FormatBytes(st.Mem.Avail) + L" free of " + FormatBytes(st.Mem.Total),
              { L.RamCard.left, L.RamCard.top + theme::Scale(12),
                L.RamCard.right - theme::Scale(16), L.RamCard.top + theme::Scale(28) },
              theme::Font(theme::FontKind::Small), P.TextDisabled,
              DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // RAM bar
    {
        DrawTextC(hdc, L"RAM load",
                  { L.RamTrack.left, L.RamTrack.top - theme::Scale(18),
                    L.RamTrack.left + theme::Scale(260), L.RamTrack.top - theme::Scale(2) },
                  theme::Font(theme::FontKind::Small), P.TextDim, DT_LEFT | DT_SINGLELINE);
        DrawTextC(hdc, Fmt(L"%.0f%%", st.Mem.LoadPercent),
                  { L.RamTrack.right - theme::Scale(80), L.RamTrack.top - theme::Scale(18),
                    L.RamTrack.right, L.RamTrack.top - theme::Scale(2) },
                  theme::Font(theme::FontKind::SmallBold), P.Text,
                  DT_RIGHT | DT_SINGLELINE);
        // track + fill
        FillRoundRect(hdc, L.RamTrack, P.CardHover, 0, theme::Scale(10));
        const int w = (L.RamTrack.right - L.RamTrack.left) * st.Mem.LoadPercent / 100;
        if (w > 0) {
            const RECT fill = RECT{ L.RamTrack.left, L.RamTrack.top,
                                    L.RamTrack.left + w, L.RamTrack.bottom };
            const COLORREF col = st.Mem.LoadPercent >= (int)kAutoReleaseLoad
                                     ? P.Warn : P.Ok;
            FillRoundRect(hdc, fill, col, 0, theme::Scale(10));
        }
    }

    // CPU bar
    {
        const int cpuPct = (int)(st.Mem.CpuPercent + 0.5);
        DrawTextC(hdc, L"CPU load",
                  { L.CpuTrack.left, L.CpuTrack.top - theme::Scale(18),
                    L.CpuTrack.left + theme::Scale(260), L.CpuTrack.top - theme::Scale(2) },
                  theme::Font(theme::FontKind::Small), P.TextDim, DT_LEFT | DT_SINGLELINE);
        DrawTextC(hdc, Fmt(L"%d%%", cpuPct),
                  { L.CpuTrack.right - theme::Scale(80), L.CpuTrack.top - theme::Scale(18),
                    L.CpuTrack.right, L.CpuTrack.top - theme::Scale(2) },
                  theme::Font(theme::FontKind::SmallBold), P.Text,
                  DT_RIGHT | DT_SINGLELINE);
        FillRoundRect(hdc, L.CpuTrack, P.CardHover, 0, theme::Scale(10));
        const int w = (L.CpuTrack.right - L.CpuTrack.left) * cpuPct / 100;
        if (w > 0) {
            const RECT fill = RECT{ L.CpuTrack.left, L.CpuTrack.top,
                                    L.CpuTrack.left + w, L.CpuTrack.bottom };
            FillRoundRect(hdc, fill, P.TextDim, 0, theme::Scale(10));
        }
    }

    // last freed line
    {
        std::wstring line = st.LastFreedBytes > 0
                                ? L"Released since stand-by: " +
                                      FormatBytes(st.LastFreedBytes)
                                : L"No memory released yet.";
        DrawTextC(hdc, line,
                  { L.AutoRow.left, L.AutoRow.top - theme::Scale(18),
                    L.AutoRow.right, L.AutoRow.top - theme::Scale(2) },
                  theme::Font(theme::FontKind::Small), P.TextDisabled,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // auto-release row
    {
        FillRoundRect(hdc, L.AutoRow, g_hover == HK_AUTOTOG ? P.CardHover : P.Card,
                      0, theme::Scale(12));
        DrawTextC(hdc, Fmt(L"Automatically free memory at %d%% load or higher",
                           (int)kAutoReleaseLoad),
                  { L.AutoRow.left + theme::Scale(14), L.AutoRow.top,
                    L.AutoToggle.left - theme::Scale(8), L.AutoRow.bottom },
                  theme::Font(theme::FontKind::Body),
                  st.AutoRelease ? P.Text : P.TextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        // tiny indicator dot to make the whole row read as a switch target
        const int cy = (L.AutoRow.top + L.AutoRow.bottom) / 2;
        FillCircle(hdc, L.AutoRow.left + theme::Scale(6), cy, theme::Scale(3),
                   st.AutoRelease ? P.Ok : P.Stroke);
        DrawToggle(hdc, L.AutoToggle, st.AutoRelease, g_hover == HK_AUTOTOG);
        addHit(L.AutoRow, HK_AUTOTOG);
    }

    // process list
    DrawTextC(hdc, L"TOP RAM USERS",
              { L.RamCard.left + theme::Scale(16), L.RamCard.top + theme::Scale(174),
                L.RamCard.left + theme::Scale(320), L.RamCard.top + theme::Scale(190) },
              theme::Font(theme::FontKind::Small), P.TextDim, DT_LEFT | DT_SINGLELINE);

    const int count = static_cast<int>(st.Procs.size());
    if (count == 0) {
        DrawTextC(hdc, L"No openable processes found.",
                  { L.ProcRow[0].left, L.ProcRow[0].top,
                    L.ProcRow[0].right, L.ProcRow[0].bottom },
                  theme::Font(theme::FontKind::Body), P.TextDisabled,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    for (int i = 0; i < kProcCount; ++i) {
        const RECT r = L.ProcRow[i];
        if (i >= count) break;
        const ProcItem& p = st.Procs[i];

        // intensity dot: bigger consumers look "hotter"
        COLORREF dot = P.Stroke;
        const double share = st.Mem.Total
                                 ? double(p.WorkingSet) / double(st.Mem.Total) * 100.0
                                 : 0.0;
        if (share >= 10.0)        dot = P.Danger;
        else if (share >= 5.0)    dot = P.Warn;
        else if (share >= 1.0)    dot = P.Accent;
        FillCircle(hdc, r.left + theme::Scale(4), (r.top + r.bottom) / 2,
                   theme::Scale(3), dot);

        DrawTextC(hdc, p.Name,
                  { r.left + theme::Scale(14), r.top,
                    r.right - theme::Scale(120), r.bottom },
                  theme::Font(theme::FontKind::Body), P.Text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        DrawTextC(hdc, FormatBytes(p.WorkingSet),
                  { r.right - theme::Scale(140), r.top, r.right - theme::Scale(4), r.bottom },
                  theme::Font(theme::FontKind::Body), P.TextDim,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (count > kProcCount) {
        const RECT more = L.ProcRow[kProcCount - 1];
        DrawTextC(hdc, Fmt(L"+%d more image(s)", count - kProcCount),
                  { more.left + theme::Scale(14), more.top,
                    more.right, more.bottom },
                  theme::Font(theme::FontKind::Small), P.TextDisabled,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void RefreshSnapshot(HWND hwnd) {
    g_status = Engine::Instance().Snapshot();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void TryBoost(HWND hwnd) {
    const std::wstring err = Engine::Instance().Boost();
    if (!err.empty()) {
        MessageBoxW(hwnd, err.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
    }
    RefreshSnapshot(hwnd);
}

void ToggleBoost(HWND hwnd) {
    if (g_status.Turbo) {
        const std::wstring err = Engine::Instance().TurboOff();
        if (!err.empty())
            MessageBoxW(hwnd, err.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
    } else {
        TryBoost(hwnd);
    }
    RefreshSnapshot(hwnd);
}

void ToggleAuto(HWND hwnd) {
    Engine::Instance().SetOptions(g_status.TweakMask, !g_status.AutoRelease);
    RefreshSnapshot(hwnd);
}

void TryFree(HWND hwnd) {
    const std::wstring err = Engine::Instance().FreeMemory();
    if (!err.empty())
        MessageBoxW(hwnd, err.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
    RefreshSnapshot(hwnd);
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

// Re-applies the whole boost in place (used when a group is ticked while the
// boost is running); failures are logged instead of blocking the user.
void TryBoostNoError() {
    const std::wstring err = Engine::Instance().Boost();
    if (!err.empty()) log::Warn(Fmt(L"Boost re-apply: %ls", err.c_str()));
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

        SizeAndCenter(hwnd);
        LoadIcons(hwnd);
        AddRemoveTray(hwnd, true);

        SetTimer(hwnd, kTimerId, kTimerPeriodMs, nullptr);
        Engine::Instance().SetNotify(hwnd);
        RefreshSnapshot(hwnd);
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

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        switch (id) {
        case IDM_SHOW:
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return 0;
        case IDM_TOGGLE:
            ToggleBoost(hwnd);
            return 0;
        case IDM_FREE:
            TryFree(hwnd);
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_TIMER:
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
            ToggleBoost(hwnd);
        } else if (h == HK_BOOST) {
            TryBoost(hwnd);
        } else if (h == HK_OFF) {
            const std::wstring err = Engine::Instance().TurboOff();
            if (!err.empty())
                MessageBoxW(hwnd, err.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
            RefreshSnapshot(hwnd);
        } else if (h == HK_FREE) {
            TryFree(hwnd);
        } else if (h == HK_AUTOTOG) {
            ToggleAuto(hwnd);
        } else if (h >= HK_OPTIONS && h < HK_OPTIONS + kOptCount) {
            const unsigned bit = 1u << (h - HK_OPTIONS);
            const unsigned mask = g_status.TweakMask ^ bit;
            Engine::Instance().SetOptions(mask, g_status.AutoRelease);
            // Applying a freshly enabled group in place keeps the boost live.
            if (g_status.Turbo && (mask & bit)) TryBoostNoError();
            RefreshSnapshot(hwnd);
        }
        return 0;
    }

    case WM_APP_REFRESH:
        RefreshSnapshot(hwnd);
        return 0;

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
            AppendMenuW(m, MF_STRING | (g_status.Turbo ? MF_CHECKED : MF_UNCHECKED),
                        IDM_TOGGLE,
                        g_status.Turbo ? L"Turbo is on (restore)"
                                       : L"Turbo is off (boost)");
            AppendMenuW(m, MF_STRING, IDM_FREE, L"Free memory now");
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

}  // namespace stc::ui
