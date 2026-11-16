// layerwin.h — frameless per-pixel-alpha window base (Direct2D + UpdateLayeredWindow).
//
// Every card in the app derives from this. It owns: window creation, an
// alpha-capable D2D render target bound to a top-down DIB, per-pixel-alpha
// composition, DPI handling, a 60 Hz animation timer, and idle/stat timers.
#pragma once
#include "gfx.h"
#include "util.h"

class LayerWindow {
public:
    virtual ~LayerWindow();

    // padding is drawn space around the card (soft shadow room).
    bool Create(HINSTANCE hInst, const wchar_t* className, int cardW, int cardH, int padding,
                bool topmost);
    void Show();
    void Repaint();
    HWND Hwnd() const { return hwnd_; }
    bool Topmost() const { return topmost_; }
    void SetTopmost(bool on);
    void PlaceAt(int x, int y);          // physical screen coords
    void PlaceDefault();                 // bottom-right of work area (virtual)

protected:
    LayerWindow() = default;

    // ---- hooks for card classes ----
    virtual void OnDraw(ID2D1RenderTarget& rt, float scale) = 0;
    virtual int  HitTestZone(POINT ptClient) const = 0;   // 0 = card background
    virtual void OnClick(int zone, POINT ptClient) { UNUSED(ptClient); UNUSED(zone); }
    virtual void OnHoverZone(int zone) { UNUSED(zone); }
    virtual void OnHoverLeave() {}
    virtual void OnRightClick(POINT ptScreen) { UNUSED(ptScreen); }
    virtual void OnAnimTick(float dt) { UNUSED(dt); }
    virtual bool NeedsAnim() const { return false; }
    virtual void OnStatsTick() {}
    virtual void OnDragFinished() {}
    virtual void OnCaptureMove(int zone, POINT ptClient) {
        UNUSED(zone); UNUSED(ptClient);
    }
    virtual void OnDestroyWindow() {}
    virtual void OnRenderTargetLost() {}
    virtual bool OnCustomMsg(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* out) {
        UNUSED(msg); UNUSED(wParam); UNUSED(lParam); UNUSED(out);
        return false;
    }
    virtual void OnTimer(UINT_PTR id) { UNUSED(id); }
    virtual void ArrangeInitialPlace() { PlaceDefault(); }

    int HoverZone() const { return hoverZone_; };

    // ---- helpers for derived classes ----
    float Scale() const { return scale_; }
    // Card rect in device-dependent pixels (device px) in client space.
    int   Padding() const { return padding_; }
    int   CardW() const { return cardW_; }
    int   CardH() const { return cardH_; }
    D2D1_RECT_F CardRect() const;
    D2D1_SIZE_F CardSize() const;
    void DrawCardBase(ID2D1RenderTarget& rt);            // shadow + card background + border
    float Now() const { return (float)animClock_; }      // seconds, monotonic

    gfx::UiKit& UI() { return ui_; }

    // timers
    void StartAnim();
    void StartStatsTimer(UINT ms);
    void StopStatsTimer();

    com_ptr<ID2D1DCRenderTarget> rt_;

protected:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    void InitRenderTargetIfMissing();
    bool EnsureBitmap(const D2D1_SIZE_U& px);
    void Render();
    void Composite();                                    // UpdateLayeredWindow
    void OnDpiChanged(WPARAM wParam, LPARAM lParam);
    void BeginDrag(const POINT& ptClient);
    void HandleCaptureMove(const POINT& ptClient);

    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    int cardW_ = 0, cardH_ = 0, padding_ = 0;
    float scale_ = 1.f;
    bool topmost_ = false;
    bool hovering_ = false;
    bool dragging_ = false;
    bool dragMoved_ = false;
    int pressedZone_ = 0;
    int hoverZone_ = 0;
    POINT dragOrigin_ = {};                              // window pos at drag start (phys)
    POINT cursorOrigin_ = {};                            // screen pt at drag start

    // alpha DIB backing the layered surface
    HBITMAP bmp_ = nullptr;
    HDC memDC_ = nullptr;
    int bmpW_ = 0, bmpH_ = 0;

    gfx::UiKit ui_;
    ULONGLONG animClock_ = GetTickCount64();
    UINT statsMs_ = 0;

    static const UINT_PTR kAnimTimer = 1;
    static const UINT_PTR kStatsTimer = 2;
};
