// widget.cpp — the main floating gauge card implementation.
#include "widget.h"
#include "gfx.h"
#include "cleaner.h"

#include <windowsx.h>
#include <shellapi.h>
#include <thread>

// ------------------------------------------------------------ layout -------
static constexpr float kTitleX   = 24.f, kTitleY = 12.f, kTitleH = 26.f;
static constexpr float kChromeY  = 10.f, kChromeS = 30.f;
static constexpr float kGearX    = 232.f, kCloseX = 266.f;
static constexpr float kGaugeCx  = 158.f, kGaugeCy = 192.f, kGaugeR = 84.f;
static constexpr float kRingW    = 12.f;
static constexpr float kBigY0    = 148.f, kBigH = 64.f;
static constexpr float kCapY     = 226.f, kCapH = 16.f;
static constexpr float kBytesY   = 244.f, kBytesH = 22.f;
static constexpr float kStatCapY = 292.f, kStatCapH = 15.f;
static constexpr float kStatValY = 307.f, kStatValH = 25.f;
static constexpr float kStatX[3] = { 28.f, 122.f, 208.f };
static constexpr float kBtnX0    = 24.f, kBtnX1 = 292.f;
static constexpr float kBtnY0    = 344.f, kBtnY1 = 394.f;
static constexpr float kStatusY  = 404.f, kStatusH = 16.f;

static bool InRectF(float x, float y, float x0, float y0, float x1, float y1) {
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

// Arc color pair by usage %: cool cyan/emerald when calm, amber/red when hot.
static void RingColors(float pct, D2D1_COLOR_F* a, D2D1_COLOR_F* b) {
    const float h = Clamp01((pct - 55.f) / 30.f);
    *a = InterpColor(C(gfx::kAccentA), C(gfx::kWarnB), h);
    *b = InterpColor(C(gfx::kAccentB), C(gfx::kHot), h);
}

int WidgetWindow::HitTestZone(POINT ptClient) const {
    const float s = Scale();
    const float x = (ptClient.x - Padding() * s) / s;
    const float y = (ptClient.y - Padding() * s) / s;

    if (InRectF(x, y, kGearX, kChromeY, kGearX + kChromeS, kChromeY + kChromeS))
        return Z_GEAR;
    if (InRectF(x, y, kCloseX, kChromeY, kCloseX + kChromeS, kChromeY + kChromeS))
        return Z_CLOSE;
    if (InRectF(x, y, kBtnX0, kBtnY0, kBtnX1, kBtnY1))
        return Z_CLEAN;
    return 0;
}

bool WidgetWindow::Create(HINSTANCE hInst) {
    const auto& s = AppCtx().settings;
    // 436 tall, not 412: the extra band under CLEAN NOW holds the last clean's
    // result, so the number stays on the card instead of in a notification.
    if (!LayerWindow::Create(hInst, L"SmartRAM_Widget_v1", 316, 436, 30, s.alwaysTop))
        return false;

    // restore remembered position (falls back to default when stale, e.g. a
    // monitor is gone)
    if (s.posRemembered) {
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        if (s.posX >= vx - 200 && s.posX <= vx + vw + 200 && s.posY >= vy - 100 &&
            s.posY <= vy + vh + 100)
            PlaceAt((int)s.posX, (int)s.posY);
    }

    stats_ = AppCtx().monitor.Tick();   // establishes CPU baseline
    tray_.Add(hwnd_, WM_APP + 1, gfx::MakeIcon(32, [](ID2D1RenderTarget&) {}), L"Smart RAM");
    RefreshTray();

    StartIntro();
    StartStatsTimer(500);
    return true;
}

void WidgetWindow::ArrangeInitialPlace() {
    if (!AppCtx().settings.posRemembered) PlaceDefault();
}

void WidgetWindow::StartIntro() {
    intro_ = 0.f;
    StartAnim();
}

void WidgetWindow::OnClick(int zone, POINT pt) {
    UNUSED(pt);
    switch (zone) {
    case Z_CLEAN: TriggerClean(); break;
    case Z_GEAR:  OpenSettingsCard(); break;
    case Z_CLOSE:
        if (AppCtx().settings.closeToTray) {
            ShowWindow(hwnd_, SW_HIDE);
        } else {
            DestroyWindow(hwnd_);
        }
        break;
    }
}

void WidgetWindow::OnHoverLeave() {}   // chrome fade handled by the base class

// ------------------------------------------------------------- actions -----
void WidgetWindow::TriggerClean() {
    if (cleaning_) return;
    cleaning_ = true;
    btnScale_ = 1.f;
    StartAnim();
    Render();

    const bool clip = AppCtx().settings.cleanClipboard;
    const bool deep = AppCtx().settings.deepClean;
    HWND hwnd = hwnd_;
    std::thread([hwnd, clip, deep] {
        CleanResult r = CleanAll(clip, deep);
        PostMessageW(hwnd, WM_APP + 3, (WPARAM)(ULONG_PTR)r.releasedBytes,
                     deep ? 1 : 0);
    }).detach();
}

void WidgetWindow::OnStatsTick() {
    auto& s = AppCtx().settings;
    stats_ = AppCtx().monitor.Tick();

    if (std::abs((int)stats_.usagePct - (int)gaugeTarget_) >= 1) {
        gaugeTarget_ = (float)stats_.usagePct;
        StartAnim();
    }
    RefreshTray();

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG kCool = 5 * 60 * 1000;
    const ULONGLONG kIdleCool = 15 * 60 * 1000;
    if (!cleaning_ && now - lastAutoClean_ > kCool && s.smartCleanPct > 0 &&
        (int)stats_.usagePct >= (int)s.smartCleanPct) {
        lastAutoClean_ = now;
        TriggerClean();
    } else if (!cleaning_ && s.cleanWhenIdle && now - lastAutoClean_ > kIdleCool) {
        LASTINPUTINFO li = {};
        li.cbSize = sizeof(li);
        if (GetLastInputInfo(&li) && GetTickCount64() - li.dwTime > kCool) {
            lastAutoClean_ = now;
            TriggerClean();
        }
    }
}

void WidgetWindow::RefreshTray() {
    wchar_t tip[80];
    swprintf_s(tip, L"RAM %lu%%  •  CPU %lu%%", stats_.usagePct, stats_.cpuPct);

    // Draw frames off a ~12 fps throttle: tray icons rebuild an HICON per
    // frame, so keep the cost low but still visibly animated.
    const ULONGLONG now = GetTickCount64();
    const int shownPct = (int)floorf(gaugeShown_ + 0.5f);
    const int bucket = (int)stats_.usagePct / 10;

    const bool animating = cleaning_ || fabsf(gaugeTarget_ - gaugeShown_) > 0.05f;
    const bool needsFrame =
        animating || shownPct != trayDrawnPct_ || bucket != trayBucket_;
    if (!needsFrame || now - lastTrayStamp_ < 85) {
        tray_.UpdateTip(tip);
        return;
    }
    lastTrayStamp_ = now;
    trayDrawnPct_ = shownPct;
    trayBucket_ = bucket;

    // Match the card: colour flows green -> amber -> red as the sweep moves.
    D2D1_COLOR_F ca, cb;
    RingColors(gaugeShown_, &ca, &cb);
    const double sweep = (double)gaugeShown_ * 3.6;
    const float spin = spinnerAng_;

    HICON ic = gfx::MakeIcon(32, [&](ID2D1RenderTarget& rt) {
        gfx::UiKit k;
        k.Ensure(rt, 1.f);
        D2D1_ROUNDED_RECT bg = { { 2.f, 2.f, 30.f, 30.f }, 7.f, 7.f };
        rt.FillRoundedRectangle(&bg, k.black);

        com_ptr<ID2D1SolidColorBrush> accent;
        rt.CreateSolidColorBrush(ca, &accent);
        com_ptr<ID2D1SolidColorBrush> track;
        rt.CreateSolidColorBrush(WithAlpha(C(0xFFFFFF), 0.12f), &track);
        rt.DrawEllipse(D2D1::Ellipse(D2D1_POINT_2F{ 16.f, 16.f }, 10.f, 10.f), track, 3.6f);

        if (cleaning_) {
            // spinning "working" arc while the cleaner runs
            gfx::StrokeArc(rt, 16.f, 16.f, 10.f, 3.6f, spin, 130.f, *accent, k.roundCaps);
        } else {
            gfx::StrokeArc(rt, 16.f, 16.f, 10.f, 3.6f, -90.f, (float)sweep, *accent,
                           k.roundCaps);
        }

        std::wstring txt = std::to_wstring(shownPct);
        gfx::DrawTextCentered(rt, txt.c_str(), *k.mini,
                              D2D1_RECT_F{ 5.f, 5.f, 27.f, 27.f }, *k.txtHi);
    });
    tray_.ReplaceIcon(ic);
    tray_.UpdateTip(tip);
}

void WidgetWindow::OnRenderTargetLost() { ringPctKey_ = INT_MIN; }

// ---------------------------------------------------------------- drawing --
void WidgetWindow::OnDraw(ID2D1RenderTarget& rt, float scale) {
    UNUSED(scale);
    DrawCardBase(rt);
    DrawHeader(rt);
    DrawGauge(rt);
    DrawStatsRow(rt);
    DrawButton(rt);
    DrawCleanStatus(rt);
}

void WidgetWindow::DrawHeader(ID2D1RenderTarget& rt) {
    const float s = Scale();
    const D2D1_RECT_F card = CardRect();
    const float x = card.left, y = card.top;

    D2D1_RECT_F tr = { x + kTitleX * s, y + kTitleY * s, x + 220.f * s,
                       y + (kTitleY + kTitleH) * s };
    gfx::DrawTextLeft(rt, L"SMART RAM", *ui_.title, tr, *ui_.txtMed);

    if (chromeAlpha_ <= 0.02f) return;

    // hover highlight behind the chrome buttons
    const int hz = HoverZone();
    float bx = kGearX;
    if (hz == Z_CLOSE) bx = kCloseX;
    if (hz == Z_GEAR || hz == Z_CLOSE) {
        D2D1_RECT_F zr = { x + bx * s, y + kChromeY * s, x + (bx + kChromeS) * s,
                           y + (kChromeY + kChromeS) * s };
        D2D1_ROUNDED_RECT rr = { zr, 9.f * s, 9.f * s };
        rt.FillRoundedRectangle(&rr, ui_.hover);
    }

    // gear glyph
    const float gx = x + (kGearX + kChromeS / 2) * s;
    const float gy = y + (kChromeY + kChromeS / 2) * s;
    const float r1 = 5.2f * s, r2 = 8.4f * s;
    rt.DrawEllipse(D2D1::Ellipse(D2D1_POINT_2F{ gx, gy }, r1, r1), ui_.white30, 1.5f * s);
    for (int i = 0; i < 6; ++i) {
        const float a = DegToRad(i * 60.f + 15.f);
        rt.DrawLine(D2D1_POINT_2F{ gx + r1 * cosf(a), gy + r1 * sinf(a) },
                    D2D1_POINT_2F{ gx + r2 * cosf(a), gy + r2 * sinf(a) }, ui_.white30,
                    1.6f * s);
    }

    // close glyph
    const float cxx = x + (kCloseX + kChromeS / 2) * s;
    const float cxy = y + (kChromeY + kChromeS / 2) * s;
    const float arm = 5.f * s;
    rt.DrawLine(D2D1_POINT_2F{ cxx - arm, cxy - arm }, D2D1_POINT_2F{ cxx + arm, cxy + arm },
                ui_.white30, 1.7f * s);
    rt.DrawLine(D2D1_POINT_2F{ cxx - arm, cxy + arm }, D2D1_POINT_2F{ cxx + arm, cxy - arm },
                ui_.white30, 1.7f * s);
}

void WidgetWindow::DrawGauge(ID2D1RenderTarget& rt) {
    const float s = Scale();
    const D2D1_RECT_F card = CardRect();
    const float cx = card.left + kGaugeCx * s;
    const float cy = card.top + kGaugeCy * s;

    const float t = Clamp01(intro_);
    const float e = EaseOutBack(t);      // allows slight overshoot (springy pop)
    const float ringR = kGaugeR * s * (0.55f + 0.45f * e);
    const float ringW = kRingW * s;
    const float pct = gaugeShown_;

    // track
    if (ui_.white09)
        rt.DrawEllipse(D2D1::Ellipse(D2D1_POINT_2F{ cx, cy }, ringR, ringR), ui_.white09, ringW);

    // progress arc with soft glow
    if (pct > 0.5f) {
        const float a = pct * 3.6f;
        D2D1_COLOR_F ca, cb;
        RingColors(pct, &ca, &cb);

        const int key = (int)floorf(pct + 0.5f);
        if (key != ringPctKey_) {
            if (ringGrad_) ringGrad_->Release();
            ringGrad_ = ui_.MakeRingGrad(rt, ca, cb, cx, cy, ringR);
            ringPctKey_ = key;
        }
        if (a < 359.4f) {
            if (ringGrad_) {
                // glow pass (wider, faint)
                gfx::StrokeArc(rt, cx, cy, ringR, ringW * 1.9f, -90.f, a, *ringGrad_,
                               ui_.roundCaps);
                // main arc
                gfx::StrokeArc(rt, cx, cy, ringR, ringW, -90.f, a, *ringGrad_, ui_.roundCaps);
                // leading dot
                const float endDeg = -90.f + a;
                rt.FillEllipse(
                    D2D1::Ellipse(
                        D2D1_POINT_2F{ cx + ringR * cosf(DegToRad(endDeg)),
                                       cy + ringR * sinf(DegToRad(endDeg)) },
                        (ringW * 0.52f), (ringW * 0.52f)),
                    ui_.white30);
            }
        } else if (ringGrad_) {
            rt.DrawEllipse(D2D1::Ellipse(D2D1_POINT_2F{ cx, cy }, ringR, ringR), ringGrad_,
                           ringW);
        }
    }

    // percentage (large number + small unit), centered
    const int disp = (int)floorf(pct + 0.5f);
    std::wstring num = std::to_wstring(disp);
    com_ptr<IDWriteTextLayout> l1, l2;
    gfx::DWrite()->CreateTextLayout(num.c_str(), (UINT32)num.size(), ui_.big,
                                    220.f * s, kBigH * s + 20.f, &l1);
    gfx::DWrite()->CreateTextLayout(L"%", 1, ui_.unit, 60.f * s, kBigH * s, &l2);
    if (l1 && l2) {
        DWRITE_TEXT_METRICS m1 {}, m2 {};
        l1->GetMetrics(&m1);
        l2->GetMetrics(&m2);
        const float total = m1.widthIncludingTrailingWhitespace + 4.f * s +
                            m2.widthIncludingTrailingWhitespace;
        const float x0 = card.left + kGaugeCx * s - total / 2;
        const float y0 = card.top + kBigY0 * s;
        rt.DrawTextLayout(D2D1_POINT_2F{ x0, y0 + (kBigH * s - m1.height) / 2 }, l1, ui_.txtHi,
                          D2D1_DRAW_TEXT_OPTIONS_NONE);
        rt.DrawTextLayout(
            D2D1_POINT_2F{ x0 + m1.widthIncludingTrailingWhitespace + 4.f * s,
                           y0 + (kBigH * s - m2.height) / 2 + 8.f * s },
            l2, ui_.txtDim, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // captions
    D2D1_RECT_F cap = { cx - 110.f * s, card.top + kCapY * s, cx + 110.f * s,
                        card.top + (kCapY + kCapH) * s };
    gfx::DrawTextCentered(rt, L"RAM USED", *ui_.legend, cap, *ui_.txtDim);
    D2D1_RECT_F bytes = { cx - 110.f * s, card.top + kBytesY * s, cx + 110.f * s,
                          card.top + (kBytesY + kBytesH) * s };
    std::wstring bs = FormatBytes(stats_.totalPhys - stats_.availPhys, 1) + L" / " +
                      FormatBytes(stats_.totalPhys, 1);
    gfx::DrawTextCentered(rt, bs.c_str(), *ui_.value, bytes, *ui_.txtMed);
}

void WidgetWindow::DrawStatsRow(ID2D1RenderTarget& rt) {
    const float s = Scale();
    const D2D1_RECT_F card = CardRect();
    const float x = card.left, y = card.top;

    const wchar_t* caps[3] = { L"FREE MEMORY", L"CPU LOAD", L"APPS" };
    std::wstring vals[3] = { FormatBytes(stats_.availPhys, 1),
                             std::to_wstring(stats_.cpuPct) + L"%",
                             std::to_wstring(stats_.procCount) };

    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F c = { x + kStatX[i] * s, y + kStatCapY * s, x + kStatX[i] * s + 96.f * s,
                          y + (kStatCapY + kStatCapH) * s };
        gfx::DrawTextLeft(rt, caps[i], *ui_.legend, c, *ui_.txtDim);
        D2D1_RECT_F v = { x + kStatX[i] * s, y + kStatValY * s, x + kStatX[i] * s + 96.f * s,
                          y + (kStatValY + kStatValH) * s };
        if (i == 1) {   // CPU value tinted by load
            const D2D1_COLOR_F ccc = stats_.cpuPct < 50 ? C(gfx::kAccentB)
                                     : (stats_.cpuPct < 80 ? C(gfx::kWarnA) : C(gfx::kHot));
            com_ptr<ID2D1SolidColorBrush> tint;
            rt.CreateSolidColorBrush(ccc, &tint);
            gfx::DrawTextLeft(rt, vals[i].c_str(), *ui_.value, v, *tint);
            continue;
        }
        gfx::DrawTextLeft(rt, vals[i].c_str(), *ui_.value, v, *ui_.txtHi);
    }
}

void WidgetWindow::DrawButton(ID2D1RenderTarget& rt) {
    const float s = Scale();
    const D2D1_RECT_F card = CardRect();
    const float x = card.left, y = card.top;

    const float cx = x + (kBtnX0 + kBtnX1) / 2 * s;
    const float cy = y + (kBtnY0 + kBtnY1) / 2 * s;
    const float halfW = (kBtnX1 - kBtnX0) / 2 * s * btnScale_;
    const float halfH = (kBtnY1 - kBtnY0) / 2 * s * btnScale_;
    const float rad = (kBtnY1 - kBtnY0) / 2 * s * btnScale_;
    D2D1_RECT_F r = { cx - halfW, cy - halfH, cx + halfW, cy + halfH };
    D2D1_ROUNDED_RECT rr = { r, rad, rad };

    if (cleaning_) {
        com_ptr<ID2D1SolidColorBrush> dim;
        rt.CreateSolidColorBrush(WithAlpha(C(gfx::kBtnA), 0.28f), &dim);
        rt.FillRoundedRectangle(&rr, dim);
    } else if (ui_.buttonGrad) {
        rt.FillRoundedRectangle(&rr, ui_.buttonGrad);
    }
    if (HoverZone() == Z_CLEAN && !cleaning_) {
        com_ptr<ID2D1SolidColorBrush> gloss;
        rt.CreateSolidColorBrush(WithAlpha(C(0xFFFFFF), 0.09f), &gloss);
        rt.FillRoundedRectangle(&rr, gloss);
    }

    const float midY = (kBtnY0 + kBtnY1) / 2 * s + y;
    if (cleaning_) {
        if (ui_.white30) {
            gfx::StrokeArc(rt, cx - 30.f * s, cy, 6.f * s, 2.f * s, spinnerAng_, 120.f,
                           *ui_.white30, ui_.roundCaps);
            D2D1_RECT_F text = { cx - 12.f * s, midY - 16.f * s, cx + 110.f * s,
                                 midY + 16.f * s };
            gfx::DrawTextLeft(rt, L"CLEANING…", *ui_.btn, text, *ui_.white30);
        }
        return;
    }

    // bolt glyph drawn left of label
    const float bx = cx - 44.f * s, by = cy;
    const D2D1_POINT_2F pts[6] = {
        { bx + 2.2f * s, by - 8.4f * s }, { bx - 4.2f * s, by + 1.8f * s },
        { bx - 0.8f * s, by + 1.8f * s }, { bx - 2.6f * s, by + 8.4f * s },
        { bx + 4.2f * s, by - 1.8f * s }, { bx + 0.8f * s, by - 1.8f * s },
    };
    com_ptr<ID2D1PathGeometry> bolt;
    if (SUCCEEDED(gfx::Factory()->CreatePathGeometry(&bolt))) {
        com_ptr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(bolt->Open(&sink))) {
            sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLines(&pts[1], 5);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            rt.FillGeometry(bolt, ui_.white30);
        }
    }

    D2D1_RECT_F text = { cx - 36.f * s, midY - 16.f * s, cx + 110.f * s, midY + 16.f * s };
    gfx::DrawTextLeft(rt, L"CLEAN NOW", *ui_.btn, text, *ui_.txtHi);
}

// The card is the notice: the result of the last clean sits under the button
// until the next one replaces it. No toast, no tray balloon, nothing that
// appears over other windows.
void WidgetWindow::DrawCleanStatus(ID2D1RenderTarget& rt) {
    if (!cleaned_ || cleaning_) return;

    const float s = Scale();
    const D2D1_RECT_F card = CardRect();
    const float cx = card.left + kGaugeCx * s;
    D2D1_RECT_F box = { cx - 120.f * s, card.top + kStatusY * s,
                        cx + 120.f * s, card.top + (kStatusY + kStatusH) * s };

    const std::wstring result = lastFreed_ >= (1ull << 20)
        ? FormatBytes(lastFreed_, 2) + L" FREED"
        : L"NOTHING TO FREE";
    const std::wstring text = (lastDeep_ ? L"DEEP \u2022 " : L"") + result;

    // Green only when it was worth the click; a token release stays quiet.
    ID2D1Brush* brush = lastFreed_ >= (96ull << 20) ? (ID2D1Brush*)ui_.okGreen.get()
                                                    : (ID2D1Brush*)ui_.txtDim.get();
    if (brush) gfx::DrawTextCentered(rt, text.c_str(), *ui_.legend, box, *brush);
}

// ------------------------------------------------------------- animation ---
void WidgetWindow::OnAnimTick(float dt) {
    gaugeShown_ += (gaugeTarget_ - gaugeShown_) * (dt * 7.f < 1.f ? dt * 7.f : 1.f);
    if (fabsf(gaugeTarget_ - gaugeShown_) < 0.06f) gaugeShown_ = gaugeTarget_;

    if (intro_ < 1.f) intro_ += dt / 0.6f, intro_ = intro_ > 1.f ? 1.f : intro_;

    const float chromeTarget = hovering_ ? 1.f : 0.f;
    chromeAlpha_ += (chromeTarget - chromeAlpha_) * (dt * 9.f < 1.f ? dt * 9.f : 1.f);
    if (fabsf(chromeTarget - chromeAlpha_) < 0.02f) chromeAlpha_ = chromeTarget;

    const float btnTarget = pressedZone_ == Z_CLEAN ? 0.965f : 1.f;
    btnScale_ += (btnTarget - btnScale_) * (dt * 14.f < 1.f ? dt * 14.f : 1.f);
    if (fabsf(btnTarget - btnScale_) < 0.004f) btnScale_ = btnTarget;

    if (cleaning_) spinnerAng_ += dt * 420.f;

    // keep the tray icon animation in lock-step with the gauge / spinner
    // (RefreshTray throttles itself, so safe to call every frame here)
    if (GetTickCount64() - lastTrayStamp_ >= 85 &&
        (cleaning_ || fabsf(gaugeTarget_ - gaugeShown_) > 0.05f))
        RefreshTray();
}

bool WidgetWindow::NeedsAnim() const {
    if (cleaning_) return true;
    if (intro_ < 1.f) return true;
    if (fabsf(gaugeTarget_ - gaugeShown_) > 0.05f) return true;
    const float chromeTarget = hovering_ ? 1.f : 0.f;
    if (fabsf(chromeTarget - chromeAlpha_) > 0.01f) return true;
    const float btnTarget = pressedZone_ == Z_CLEAN ? 0.965f : 1.f;
    if (fabsf(btnTarget - btnScale_) > 0.003f) return true;
    return false;
}

// ------------------------------------------------------------- messages ----
void WidgetWindow::ToggleCard() {
    if (IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    StartIntro();
    Render();
}

void WidgetWindow::OnTimer(UINT_PTR id) {
    if (id != kTrayClickTimer) return;
    KillTimer(hwnd_, kTrayClickTimer);
    if (!trayClickPending_) return;
    trayClickPending_ = false;
    ToggleCard();
}

void WidgetWindow::OpenSettingsCard() { AppCtx().OpenSettings(); }

void WidgetWindow::OpenMenu(POINT screenPt) {
    if (menuOpen_) return;
    menuOpen_ = true;
    auto& s = AppCtx().settings;

    HMENU m = CreatePopupMenu();
    auto add = [&](int id, const std::wstring& text, bool checked) {
        AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0), id, text.c_str());
    };
    AppendMenuW(m, MF_STRING, kMenuShow,
                IsWindowVisible(hwnd_) ? L"Hide card" : L"Show card");
    AppendMenuW(m, MF_STRING, kMenuClean, cleaning_ ? L"Cleaning…" : L"Clean now");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(kMenuTop, L"Always on top", s.alwaysTop);
    add(kMenuSmart, L"Smart clean (" + std::to_wstring(s.smartCleanPct) + L"%+)",
        s.smartCleanPct > 0);
    add(kMenuIdle, L"Clean when idle", s.cleanWhenIdle);
    add(kMenuClip, L"Clear clipboard", s.cleanClipboard);
    add(kMenuDeepClean, L"Deep memory clean", s.deepClean);
    add(kMenuStartup, L"Start with Windows", s.startWithWin);
    add(kMenuCloseToTray, L"Close to Tray", s.closeToTray);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuSettings, L"Settings…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuExit, L"Exit");

    SetForegroundWindow(hwnd_);
    const int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN |
                                             TPM_RIGHTBUTTON,
                                     screenPt.x, screenPt.y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(m);
    menuOpen_ = false;
    if (cmd) HandleMenuCmd(cmd, screenPt);
}

void WidgetWindow::HandleMenuCmd(int cmd, POINT menuPt) {
    UNUSED(menuPt);
    auto& s = AppCtx().settings;
    bool settingsChanged = false;
    switch (cmd) {
    case kMenuShow: ToggleCard(); break;
    case kMenuClean: TriggerClean(); break;
    case kMenuTop:
        s.alwaysTop = !s.alwaysTop;
        SetTopmost(s.alwaysTop);
        s.Save();
        settingsChanged = true;
        break;
    case kMenuSmart:
        s.smartCleanPct = s.smartCleanPct > 0 ? 0 : 85;
        s.Save();
        settingsChanged = true;
        break;
    case kMenuIdle:
        s.cleanWhenIdle = !s.cleanWhenIdle;
        s.Save();
        settingsChanged = true;
        break;
    case kMenuClip:
        s.cleanClipboard = !s.cleanClipboard;
        s.Save();
        settingsChanged = true;
        break;
    case kMenuDeepClean:
        s.deepClean = !s.deepClean;
        s.Save();
        settingsChanged = true;
        break;
    case kMenuStartup:
        s.ApplyStartWithWindows(!s.startWithWin);
        s.Save();
        settingsChanged = true;
        break;
    case kMenuCloseToTray:
        s.closeToTray = !s.closeToTray;
        s.Save();
        settingsChanged = true;
        break;
    case kMenuSettings: OpenSettingsCard(); break;
    case kMenuExit:     DestroyWindow(hwnd_); break;
    default: break;
    }
    if (settingsChanged) AppCtx().BroadcastSettingsChanged();
}

void WidgetWindow::SaveWindowPos() {
    RECT wr = {};
    if (GetWindowRect(hwnd_, &wr)) {
        auto& s = AppCtx().settings;
        s.posX = wr.left;
        s.posY = wr.top;
        s.posRemembered = true;
        s.Save();
    }
}

void WidgetWindow::OnDragFinished() {
    if (dragMoved_) SaveWindowPos();
}

void WidgetWindow::OnDestroyWindow() {
    SaveWindowPos();
    tray_.Remove();
    PostQuitMessage(0);
}

bool WidgetWindow::OnCustomMsg(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* out) {
    if (msg == WM_APP + 1) {   // tray callback
        switch (lParam) {
        case WM_LBUTTONUP:
            // Wait out the double-click interval so a double-click can open
            // Settings without also toggling the card.
            if (trayDoubleSeen_) {
                trayDoubleSeen_ = false;
                break;
            }
            if (!trayClickPending_) {
                trayClickPending_ = true;
                SetTimer(hwnd_, kTrayClickTimer, GetDoubleClickTime(), nullptr);
            }
            break;
        case WM_LBUTTONDBLCLK:
            if (trayClickPending_) {
                KillTimer(hwnd_, kTrayClickTimer);
                trayClickPending_ = false;
            }
            trayDoubleSeen_ = true;
            AppCtx().ToggleSettings();
            break;
        case WM_RBUTTONUP: {
            if (trayClickPending_) {
                KillTimer(hwnd_, kTrayClickTimer);
                trayClickPending_ = false;
            }
            trayDoubleSeen_ = false;
            POINT p = {};
            GetCursorPos(&p);
            OpenMenu(p);
            break;
        }
        default:
            break;
        }
        *out = 0;
        return true;
    }
    if (msg == WM_APP + 2) {   // settings changed elsewhere
        const auto& s = AppCtx().settings;
        SetTopmost(s.alwaysTop);
        RefreshTray();
        Render();
        *out = 0;
        return true;
    }
    if (msg == WM_APP + 3) {   // clean finished, wParam = bytes released, lParam = deep
        cleaning_ = false;
        cleaned_ = true;
        lastDeep_ = lParam != 0;
        lastFreed_ = (ULONG64)wParam;
        stats_ = AppCtx().monitor.Tick();
        gaugeTarget_ = (float)stats_.usagePct;
        StartAnim();
        Render();
        *out = 0;
        return true;
    }
    if (msg == WM_APP + 4) {   // second instance asks us to surface
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        StartIntro();
        Render();
        *out = 0;
        return true;
    }
    if (msg == AppTaskbarCreatedMsg()) {   // explorer restarted
        tray_ = TrayIcon{};                // drop stale registration state
        tray_.Add(hwnd_, WM_APP + 1, gfx::MakeIcon(32, [](ID2D1RenderTarget&) {}),
                  L"Smart RAM");
        trayBucket_ = -1;
        trayDrawnPct_ = INT_MIN;
        lastTrayStamp_ = 0;
        RefreshTray();
        *out = 0;
        return true;
    }
    if (msg == WM_CLOSE) {
        DestroyWindow(hwnd_);
        *out = 0;
        return true;
    }
    UNUSED(wParam);
    UNUSED(lParam);
    return false;
}
