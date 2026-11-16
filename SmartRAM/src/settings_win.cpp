// settings_win.cpp — the settings card.
#include "settings_win.h"
#include "gfx.h"
#include "widget.h"

#include <windowsx.h>
#include <shellapi.h>

static constexpr float kW = 356.f, kH = 544.f;
static constexpr float kTitleX = 24.f, kTitleY = 12.f, kTitleH = 26.f;
static constexpr float kCloseX = 296.f, kCloseS = 30.f, kCloseY = 10.f;
static constexpr float kRowsX = 24.f;        // toggle x
static constexpr float kTextX = 84.f;        // label x
static constexpr float kRowY[7] = { 52.f, 132.f, 190.f, 248.f, 306.f, 364.f, 422.f };
static constexpr float kRowH[7] = { 72.f, 50.f, 50.f, 50.f, 50.f, 50.f, 50.f };
static constexpr float kToggleW = 46.f, kToggleH = 26.f;
static constexpr float kSliderYOff = 56.f;   // row0 slider y offset from row top
static constexpr float kSliderX0 = 86.f, kSliderX1 = 262.f;
static constexpr float kFooterY = 508.f;

static const wchar_t* kRowLabel[7] = { L"Smart Clean", L"Clean When Idle",
                                       L"Clear Clipboard", L"Always on Top",
                                       L"Start with Windows", L"Close to Tray",
                                       L"Deep Memory Clean" };
static const wchar_t* kRowSub[7] = { L"Free memory automatically",
                                     L"After 5 minutes of inactivity",
                                     L"Purge clipboard on every clean",
                                     L"Keep the card above other windows",
                                     L"Launch the card at login",
                                     L"\u2715 hides the card instead of exiting",
                                     L"Purge caches and unused RAM" };

static bool InRectF2(float x, float y, float x0, float y0, float x1, float y1) {
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

bool SettingsWindow::EnsureCreated(HINSTANCE hInst) {
    if (Hwnd()) return true;
    return LayerWindow::Create(hInst, L"SmartRAM_Settings_v1", (int)kW, (int)kH, 30, false);
}

void SettingsWindow::Reveal() {
    if (!Hwnd()) return;
    PlaceNearWidget();
    ShowWindow(Hwnd(), SW_SHOWNOACTIVATE);
    SetForegroundWindow(Hwnd());
    Render();
}

// Put the settings card just left of the widget when there is room on the
// widget's monitor (vertically aligned with it); otherwise centre it on the
// monitor's work area.
void SettingsWindow::PlaceNearWidget() {
    const int winW = (int)((kW + 60) * Scale());
    const int winH = (int)((kH + 60) * Scale());

    RECT work = {};          // work area of the monitor the widget sits on
    RECT wr = {};
    HMONITOR mon = AppCtx().widget && GetWindowRect(AppCtx().widget->Hwnd(), &wr)
        ? MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST) : nullptr;
    MONITORINFO mi = { sizeof(mi) };
    if (mon && GetMonitorInfoW(mon, &mi))
        work = mi.rcWork;
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    int x, y;
    if (mon) {
        // try the gap left of the widget; fall back to horizontal centre
        x = wr.left - winW - 8;
        if (x < work.left + 4)
            x = work.left + ((work.right - work.left) - winW) / 2;
        y = wr.top + ((wr.bottom - wr.top) - winH) / 2;
    } else {
        x = work.left + ((work.right - work.left) - winW) / 2;
        y = work.top + ((work.bottom - work.top) - winH) / 2;
    }
    if (y < work.top + 4) y = work.top + 4;
    if (y + winH > work.bottom - 4) y = work.bottom - 4 - winH;
    if (x + winW > work.right - 4) x = work.right - 4 - winW;
    PlaceAt(x, y);
}

// ---------------------------------------------------------------- zones ----
int SettingsWindow::HitTestZone(POINT ptClient) const {
    const float s = Scale();
    const float x = (ptClient.x - Padding() * s) / s;
    const float y = (ptClient.y - Padding() * s) / s;

    if (InRectF2(x, y, kCloseX, kCloseY, kCloseX + kCloseS, kCloseY + kCloseS)) return Z_CLOSE;

    for (int i = 0; i < 7; ++i) {
        const float ty = kRowY[i] + (kRowH[i] - kToggleH) / 2;
        if (InRectF2(x, y, kRowsX, ty, kRowsX + kToggleW, ty + kToggleH))
            return i == 0 ? Z_ROW0_TOGGLE : (Z_ROW1 + i - 1);
    }
    if (InRectF2(x, y, kSliderX0 - 8.f, kRowY[0] + kSliderYOff - 10.f, kW - 24.f,
                 kRowY[0] + kSliderYOff + 18.f))
        return Z_ROW0_SLIDER;
    return 0;
}

void SettingsWindow::ApplyZoneToggle(int zone) {
    auto& s = AppCtx().settings;
    switch (zone) {
    case Z_ROW0_TOGGLE: s.smartCleanPct = s.smartCleanPct > 0 ? 0 : 85; break;
    case Z_ROW1:        s.cleanWhenIdle = !s.cleanWhenIdle; break;
    case Z_ROW2:        s.cleanClipboard = !s.cleanClipboard; break;
    case Z_ROW3:        s.alwaysTop = !s.alwaysTop; break;
    case Z_ROW4:        s.ApplyStartWithWindows(!s.startWithWin); break;
    case Z_ROW5:        s.closeToTray = !s.closeToTray; break;
    case Z_ROW6:        s.deepClean = !s.deepClean; break;
    default: return;
    }
    s.Save();
    AppCtx().BroadcastSettingsChanged();
}

void SettingsWindow::CloseCard() { ShowWindow(Hwnd(), SW_HIDE); }

void SettingsWindow::OnClick(int zone, POINT pt) {
    if (zone == Z_CLOSE) { CloseCard(); return; }

    if (zone == Z_ROW0_SLIDER) {
        auto& s = AppCtx().settings;
        const float x = (pt.x - Padding() * Scale()) / Scale();
        float t = Clamp01((x - kSliderX0) / (kSliderX1 - kSliderX0));
        int v = 50 + (int)(t * 45.f + 0.5f);
        v = v < 50 ? 50 : (v > 95 ? 95 : v);
        if (s.smartCleanPct > 0 && v != (int)s.smartCleanPct) {
            s.smartCleanPct = (UINT)v;
            s.Save();
            AppCtx().BroadcastSettingsChanged();
        }
        Render();
        return;
    }
    ApplyZoneToggle(zone);
    Render();
}

void SettingsWindow::OnAnimTick(float dt) { UNUSED(dt); }

bool SettingsWindow::NeedsAnim() const { return false; }

// AppContext::BroadcastSettingsChanged() posts this after a tray-menu toggle
// (or a toggle in another window), so an open settings card redraws with the
// new state instead of showing a stale switch.
bool SettingsWindow::OnCustomMsg(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* out) {
    UNUSED(wParam);
    UNUSED(lParam);
    if (msg == WM_APP + 2) {
        Repaint();
        *out = 0;
        return true;
    }
    return false;
}

void SettingsWindow::OnCaptureMove(int zone, POINT pt) {
    if (zone != Z_ROW0_SLIDER) return;
    auto& s = AppCtx().settings;
    if (s.smartCleanPct <= 0) return;
    const float x = (pt.x - Padding() * Scale()) / Scale();
    float t = Clamp01((x - kSliderX0) / (kSliderX1 - kSliderX0));
    int v = 50 + (int)(t * 45.f + 0.5f);
    v = v < 50 ? 50 : (v > 95 ? 95 : v);
    if (v != (int)s.smartCleanPct) {
        s.smartCleanPct = (UINT)v;
        s.Save();
        AppCtx().BroadcastSettingsChanged();
        Render();
    }
}

// --------------------------------------------------------------- drawing ---
void SettingsWindow::OnDraw(ID2D1RenderTarget& rt, float scale) {
    UNUSED(scale);
    DrawCardBase(rt);

    const float s = Scale();
    const D2D1_RECT_F card = CardRect();
    const float x = card.left, y = card.top;
    auto& set = AppCtx().settings;

    // header
    D2D1_RECT_F tr = { x + kTitleX * s, y + kTitleY * s, x + 240.f * s,
                       y + (kTitleY + kTitleH) * s };
    gfx::DrawTextLeft(rt, L"SETTINGS", *ui_.title, tr, *ui_.txtMed);

    D2D1_RECT_F cr = { x + kCloseX * s, y + kCloseY * s, x + (kCloseX + kCloseS) * s,
                       y + (kCloseY + kCloseS) * s };
    D2D1_ROUNDED_RECT crr = { cr, 9.f * s, 9.f * s };
    if (HoverZone() == Z_CLOSE) rt.FillRoundedRectangle(&crr, ui_.hover);
    const float cxx = (cr.left + cr.right) / 2, cxy = (cr.top + cr.bottom) / 2;
    const float arm = 5.f * s;
    rt.DrawLine(D2D1_POINT_2F{ cxx - arm, cxy - arm }, D2D1_POINT_2F{ cxx + arm, cxy + arm },
                ui_.white30, 1.6f * s);
    rt.DrawLine(D2D1_POINT_2F{ cxx - arm, cxy + arm }, D2D1_POINT_2F{ cxx + arm, cxy - arm },
                ui_.white30, 1.6f * s);

    // divider under header
    rt.DrawLine(D2D1_POINT_2F{ x + 24.f * s, y + 48.f * s },
                D2D1_POINT_2F{ x + (kW - 24.f) * s, y + 48.f * s }, ui_.white07, 1.f);

    // rows
    for (int i = 0; i < 7; ++i) {
        const bool on = i == 0 ? (set.smartCleanPct > 0)
                        : i == 1 ? set.cleanWhenIdle
                        : i == 2 ? set.cleanClipboard
                        : i == 3 ? set.alwaysTop
                        : i == 4 ? set.startWithWin
                        : i == 5 ? set.closeToTray
                                 : set.deepClean;
        const float rowTop = y + kRowY[i] * s;

        // toggle pill
        const float ty = rowTop + (kRowH[i] * s - kToggleH * s) / 2;
        D2D1_RECT_F pill = { x + kRowsX * s, ty, x + (kRowsX + kToggleW) * s,
                             ty + kToggleH * s };
        D2D1_ROUNDED_RECT pr = { pill, kToggleH * s / 2, kToggleH * s / 2 };

        if (on) {
            // local per-pill gradient across the pill's vertical extent
            com_ptr<ID2D1LinearGradientBrush> pgrad;
            D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES gp = {};
            gp.startPoint = D2D1_POINT_2F{ (pill.left + pill.right) / 2, pill.top };
            gp.endPoint   = D2D1_POINT_2F{ (pill.left + pill.right) / 2, pill.bottom };
            com_ptr<ID2D1GradientStopCollection> gs;
            D2D1_GRADIENT_STOP g[2] = { { 0.f, C(gfx::kAccentB) }, { 1.f, C(gfx::kAccentA) } };
            if (SUCCEEDED(rt.CreateGradientStopCollection(g, 2, D2D1_GAMMA_2_2,
                                                          D2D1_EXTEND_MODE_CLAMP, &gs)) &&
                SUCCEEDED(rt.CreateLinearGradientBrush(gp, gs, &pgrad)))
                rt.FillRoundedRectangle(&pr, pgrad);
        } else {
            com_ptr<ID2D1SolidColorBrush> offb;
            rt.CreateSolidColorBrush(WithAlpha(C(0xFFFFFF), 0.12f), &offb);
            rt.FillRoundedRectangle(&pr, offb);
        }

        const int hz = HoverZone();
        const bool rowHover =
            (i == 0 && hz == Z_ROW0_TOGGLE) ||
            (i > 0 && hz == Z_ROW1 + i - 1);
        if (rowHover) rt.DrawRoundedRectangle(&pr, ui_.white07, 1.2f * s);

        // knob
        const float knobR = (kToggleH - 6) / 2.f * s;
        const float kx = on ? pill.right - knobR - 3.f * s : pill.left + knobR + 3.f * s;
        const float ky = (pill.top + pill.bottom) / 2;
        com_ptr<ID2D1SolidColorBrush> knobb;
        rt.CreateSolidColorBrush(C(0xF2F6FC), &knobb);
        rt.FillEllipse(D2D1::Ellipse(D2D1_POINT_2F{ kx, ky }, knobR, knobR), knobb);

        // label + sub
        D2D1_RECT_F lr = { x + kTextX * s, rowTop + 2.f * s, x + (kW - 26.f) * s,
                           rowTop + 24.f * s };
        gfx::DrawTextLeft(rt, kRowLabel[i], *ui_.label, lr, *ui_.txtHi);
        D2D1_RECT_F sr = { x + kTextX * s, rowTop + 26.f * s, x + (kW - 26.f) * s,
                           rowTop + 44.f * s };
        gfx::DrawTextLeft(rt, kRowSub[i], *ui_.sub, sr, *ui_.txtDim);
    }

    // smart clean threshold slider
    {
        const float sy = y + (kRowY[0] + kSliderYOff) * s;
        const float x0 = x + kSliderX0 * s, x1 = x + kSliderX1 * s;
        const bool on = set.smartCleanPct > 0;
        const float t = on ? (float)(set.smartCleanPct - 50) / 45.f : 0.f;

        com_ptr<ID2D1SolidColorBrush> track;
        rt.CreateSolidColorBrush(WithAlpha(C(0xFFFFFF), on ? 0.10f : 0.06f), &track);
        D2D1_RECT_F tk = { x0, sy - 3.f * s, x1, sy + 3.f * s };
        D2D1_ROUNDED_RECT tkr = { tk, 3.f * s, 3.f * s };
        rt.FillRoundedRectangle(&tkr, track);

        if (on) {
            // filled segment
            com_ptr<ID2D1LinearGradientBrush> fillg;
            D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES gp = {};
            gp.startPoint = D2D1_POINT_2F{ x0, sy };
            gp.endPoint   = D2D1_POINT_2F{ x1, sy };
            com_ptr<ID2D1GradientStopCollection> gs;
            D2D1_GRADIENT_STOP g[2] = { { 0.f, C(gfx::kAccentB) }, { 1.f, C(gfx::kAccentA) } };
            if (SUCCEEDED(rt.CreateGradientStopCollection(g, 2, D2D1_GAMMA_2_2,
                                                          D2D1_EXTEND_MODE_CLAMP, &gs)) &&
                SUCCEEDED(rt.CreateLinearGradientBrush(gp, gs, &fillg))) {
                D2D1_RECT_F filled = { x0, sy - 3.f * s, x0 + t * (x1 - x0), sy + 3.f * s };
                D2D1_ROUNDED_RECT frr = { filled, 3.f * s, 3.f * s };
                rt.FillRoundedRectangle(&frr, fillg);
            }

            // thumb
            const float txv = x0 + t * (x1 - x0);
            com_ptr<ID2D1SolidColorBrush> thumb;
            rt.CreateSolidColorBrush(C(0xF2F6FC), &thumb);
            rt.FillEllipse(D2D1::Ellipse(D2D1_POINT_2F{ txv, sy }, 8.f * s, 8.f * s), thumb);

            // value label
            wchar_t lbl[24];
            swprintf_s(lbl, L"\u2265 %u%%", (unsigned)set.smartCleanPct);
            D2D1_RECT_F vr = { x1 + 8.f * s, sy - 12.f * s, x + (kW - 16.f) * s, sy + 12.f * s };
            gfx::DrawTextLeft(rt, lbl, *ui_.value, vr, *ui_.txtMed);
        } else {
            D2D1_RECT_F vr = { x1 + 8.f * s, sy - 12.f * s, x + (kW - 22.f) * s,
                               sy + 12.f * s };
            gfx::DrawTextLeft(rt, L"Off", *ui_.value, vr, *ui_.txtDim);
        }
    }

    // footer
    D2D1_RECT_F footer = { x + 24.f * s, y + kFooterY * s, x + (kW - 24.f) * s,
                           y + (kFooterY + 16.f) * s };
    gfx::DrawTextLeft(rt, L"Smart RAM for Windows  •  v1.0", *ui_.sub, footer, *ui_.txtDim);
}
