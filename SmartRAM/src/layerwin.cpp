// layerwin.cpp — frameless per-pixel-alpha window base implementation.
#include "layerwin.h"

#include <windowsx.h>
#include <shellapi.h>

static const int kDragThresholdPx = 3;
static const float kCardRadius_design = 22.f;
static const int   kCardPadding_design = 30;
static const int   kCardPadding_design_f = 30;   // kept int; padding passed at Create()

LayerWindow::~LayerWindow() {
    StopStatsTimer();
    if (hwnd_) { KillTimer(hwnd_, kAnimTimer); DestroyWindow(hwnd_); }
    if (bmp_)   DeleteObject(bmp_);
    if (memDC_) DeleteDC(memDC_);
}

bool LayerWindow::Create(HINSTANCE hInst, const wchar_t* className, int cardW, int cardH,
                         int padding, bool topmost) {
    inst_   = hInst;
    cardW_  = cardW;
    cardH_  = cardH;
    padding_ = padding;
    topmost_ = topmost;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &LayerWindow::WndProcThunk;
    wc.hInstance     = hInst;
    wc.lpszClassName = className;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    scale_         = dpi / 96.f;
    const int winW = (int)((cardW_ + 2 * padding_) * scale_);
    const int winH = (int)((cardH_ + 2 * padding_) * scale_);

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | (topmost_ ? WS_EX_TOPMOST : 0);
    hwnd_ = CreateWindowExW(exStyle, className, L"Smart RAM", WS_POPUP, 0, 0, winW, winH,
                            nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;

    InitRenderTargetIfMissing();
    return rt_ != nullptr;
}

void LayerWindow::Show() {
    ArrangeInitialPlace();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetTopmost(topmost_);
    Render();
}

void LayerWindow::SetTopmost(bool on) {
    topmost_ = on;
    if (hwnd_)
        SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void LayerWindow::PlaceAt(int x, int y) {
    if (hwnd_)
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// The card starts in the middle of the work area; dragging it somewhere else is
// remembered, so this is only where a machine that has never seen it puts it.
void LayerWindow::PlaceDefault() {
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int winW = (int)((cardW_ + 2 * padding_) * scale_);
    const int winH = (int)((cardH_ + 2 * padding_) * scale_);
    const int x = work.left + ((work.right - work.left) - winW) / 2;
    const int y = work.top + ((work.bottom - work.top) - winH) / 2;
    PlaceAt(x, y);
}

void LayerWindow::Repaint() { Render(); }

void LayerWindow::StartAnim() {
    if (hwnd_) SetTimer(hwnd_, kAnimTimer, 16, nullptr);
}
void LayerWindow::StartStatsTimer(UINT ms) {
    statsMs_ = ms;
    if (hwnd_) SetTimer(hwnd_, kStatsTimer, ms, nullptr);
}
void LayerWindow::StopStatsTimer() {
    statsMs_ = 0;
    if (hwnd_) KillTimer(hwnd_, kStatsTimer);
}

void LayerWindow::InitRenderTargetIfMissing() {
    if (rt_) return;
    if (!gfx::Factory() || !gfx::Factory().get()) return;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.f, 0.f, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
    gfx::Factory()->CreateDCRenderTarget(&props, &rt_);
}

bool LayerWindow::EnsureBitmap(const D2D1_SIZE_U& px) {
    if (bmp_ && memDC_ && bmpW_ == (int)px.width && bmpH_ == (int)px.height) return true;

    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    if (bmp_)   { DeleteObject(bmp_); bmp_ = nullptr; }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = (LONG)px.width;
    bi.bmiHeader.biHeight      = -(LONG)px.height;   // top-down for ULW
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp_) ZeroMemory(bits, (size_t)px.width * px.height * 4);
    memDC_ = bmp_ ? CreateCompatibleDC(screen) : nullptr;
    ReleaseDC(nullptr, screen);
    if (!bmp_ || !memDC_) return false;
    SelectObject(memDC_, bmp_);

    RECT rc = { 0, 0, (LONG)px.width, (LONG)px.height };
    if (FAILED(rt_->BindDC(memDC_, &rc))) return false;

    bmpW_ = (int)px.width;
    bmpH_ = (int)px.height;
    return true;
}

void LayerWindow::Render() {
    animClock_ = GetTickCount64();
    if (!hwnd_) return;
    InitRenderTargetIfMissing();
    if (!rt_) return;

    const D2D1_SIZE_U winPx = {
        (UINT)((cardW_ + 2 * padding_) * scale_),
        (UINT)((cardH_ + 2 * padding_) * scale_) };
    if (!EnsureBitmap(winPx)) return;

    ui_.Ensure(*rt_, scale_);

    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
    OnDraw(*rt_, scale_);
    HRESULT hr = rt_->EndDraw(nullptr, nullptr);
    if (hr == D2DERR_RECREATE_TARGET) {
        rt_.release();
        ui_.Invalidate();
        OnRenderTargetLost();
        return;
    }
    Composite();
}

void LayerWindow::Composite() {
    if (!hwnd_ || !memDC_ || !bmp_) return;
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT src = { 0, 0 };
    SIZE sz = { bmpW_, bmpH_ };
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd_, screen, nullptr, &sz, memDC_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void LayerWindow::OnDpiChanged(WPARAM wParam, LPARAM lParam) {
    scale_ = HIWORD(wParam) / 96.f;
    ui_.Invalidate();

    RECT* suggested = reinterpret_cast<RECT*>(lParam);
    if (suggested) {
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        const int winW = (int)((cardW_ + 2 * padding_) * scale_);
        const int winH = (int)((cardH_ + 2 * padding_) * scale_);
        SetWindowPos(hwnd_, nullptr, 0, 0, winW, winH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Repaint();
}

void LayerWindow::BeginDrag(const POINT& ptClient) {
    UNUSED(ptClient);
    dragging_ = true;
    dragMoved_ = false;
    pressedZone_ = 0;
    RECT wr = {};
    GetWindowRect(hwnd_, &wr);
    dragOrigin_ = POINT{ wr.left, wr.top };
    GetCursorPos(&cursorOrigin_);
    SetCapture(hwnd_);
}

void LayerWindow::HandleCaptureMove(const POINT& ptClient) {
    UNUSED(ptClient);
    POINT sp = {};
    GetCursorPos(&sp);
    const int dx = sp.x - cursorOrigin_.x;
    const int dy = sp.y - cursorOrigin_.y;
    if (!dragMoved_ && (abs(dx) + abs(dy)) < kDragThresholdPx) return;
    dragMoved_ = true;

    const int winW = (int)((cardW_ + 2 * padding_) * scale_);
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int minX = vx - winW + (int)(70 * scale_);
    const int maxX = vx + vw - (int)(70 * scale_);
    const int minY = vy;
    const int maxY = vy + vh - (int)(70 * scale_);

    int nx = dragOrigin_.x + dx;
    int ny = dragOrigin_.y + dy;
    nx = nx < minX ? minX : (nx > maxX ? maxX : nx);
    ny = ny < minY ? minY : (ny > maxY ? maxY : ny);
    PlaceAt(nx, ny);
}

D2D1_RECT_F LayerWindow::CardRect() const {
    const float p = padding_ * scale_;
    return D2D1_RECT_F{ p, p, p + cardW_ * scale_, p + cardH_ * scale_ };
}
D2D1_SIZE_F LayerWindow::CardSize() const {
    return D2D1_SIZE_F{ cardW_ * scale_, cardH_ * scale_ };
}

void LayerWindow::DrawCardBase(ID2D1RenderTarget& rt) {
    const float s = Scale();
    const D2D1_RECT_F card = CardRect();
    const float r = kCardRadius_design * s;

    // soft layered outer glow (stands in for the DWM shadow layered windows don't get)
    for (int i = 5; i >= 1; --i) {
        const float d = (float)i * 2.6f * s;
        D2D1_ROUNDED_RECT rr = { { card.left - d, card.top - d, card.right + d, card.bottom + d },
                                 r + d, r + d };
        rt.FillRoundedRectangle(&rr, ui_.black);
    }

    // card body
    D2D1_ROUNDED_RECT body = { card, r, r };
    rt.FillRoundedRectangle(&body, ui_.cardBg);

    // hairline border
    if (ui_.white07) {
        D2D1_ROUNDED_RECT in = {
            { card.left + 1.f * s, card.top + 1.f * s,
              card.right - 1.f * s, card.bottom - 1.f * s },
            r - 1.f * s, r - 1.f * s };
        rt.DrawRoundedRectangle(&in, ui_.white07, 1.f * s);
    }
}

// -------------------------------------------------------------- messages --
LRESULT CALLBACK LayerWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<LayerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (self) return self->WndProc(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT LayerWindow::WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        Repaint();
        return 0;
    }

    case WM_TIMER:
        if (wParam == kAnimTimer) {
            const ULONGLONG now = GetTickCount64();
            float dt = (now - animClock_) / 1000.f;
            animClock_ = now;
            if (dt > 0.25f) dt = 0.25f;
            OnAnimTick(dt);
            if (!NeedsAnim()) {
                KillTimer(hwnd_, kAnimTimer);
            } else {
                Render();
            }
            return 0;
        }
        if (wParam == kStatsTimer) { OnStatsTick(); return 0; }
        OnTimer(wParam);
        return 0;

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (dragging_) { HandleCaptureMove(pt); return 0; }
        if (pressedZone_ != 0 && GetCapture() == hwnd_) {
            OnCaptureMove(pressedZone_, pt);
            return 0;
        }
        if (!hovering_) {
            hovering_ = true;
            StartAnim();                       // fade the chrome in
            TRACKMOUSEEVENT te = { sizeof(te), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&te);
        }
        const int z = HitTestZone(pt);
        if (z != hoverZone_) { hoverZone_ = z; OnHoverZone(z); Repaint(); }
        return 0;
    }

    case WM_MOUSELEAVE:
        hovering_ = false;
        hoverZone_ = 0;
        pressedZone_ = 0;
        StartAnim();                           // fade the chrome out
        OnHoverLeave();
        Repaint();
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        pressedZone_ = HitTestZone(pt);
        if (pressedZone_ == 0)
            BeginDrag(pt);
        else
            SetCapture(hwnd_);
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
            OnDragFinished();
            Render();
            return 0;
        }
        if (pressedZone_ != 0) {
            const int z = HitTestZone(pt);
            if (z == pressedZone_) OnClick(z, pt);
            pressedZone_ = 0;
            ReleaseCapture();
            Repaint();
        }
        return 0;
    }

    case WM_RBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hwnd_, &pt);
        pressedZone_ = 0;
        OnRightClick(pt);
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt = {};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            const int z = HitTestZone(pt);
            SetCursor(LoadCursorW(nullptr, z != 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_DPICHANGED:
        OnDpiChanged(wParam, lParam);
        return 0;

    case WM_DISPLAYCHANGE:
        Repaint();
        return 0;

    case WM_ENDSESSION:
        if (wParam) OnDestroyWindow();
        return 0;

    case WM_DESTROY:
        OnDestroyWindow();
        return 0;
    }
    LRESULT routed = 0;
    if (OnCustomMsg(msg, wParam, lParam, &routed)) return routed;
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}
