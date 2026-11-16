// widget.h — the main floating gauge card ("monitor & release memory").
#pragma once
#include "layerwin.h"
#include "app.h"
#include "tray.h"

class WidgetWindow : public LayerWindow {
public:
    bool Create(HINSTANCE hInst);
    void OpenMenu(POINT screenPt);        // card + tray context menu
    void TriggerClean();
    void HandleMenuCmd(int cmd, POINT menuPt);

protected:
    void OnDraw(ID2D1RenderTarget& rt, float scale) override;
    int  HitTestZone(POINT ptClient) const override;
    void OnClick(int zone, POINT ptClient) override;
    void OnHoverLeave() override;
    void OnAnimTick(float dt) override;
    bool NeedsAnim() const override;
    void OnStatsTick() override;
    void OnDragFinished() override;
    void OnDestroyWindow() override;
    void OnRenderTargetLost() override;
    void OnTimer(UINT_PTR id) override;
    void ArrangeInitialPlace() override;

private:
    enum { Z_CLEAN = 1, Z_GEAR = 2, Z_CLOSE = 3 };
    static const UINT_PTR kTrayClickTimer = 3;

    bool OnCustomMsg(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* out) override;
    void ToggleCard();
    void OpenSettingsCard();
    void DrawGauge(ID2D1RenderTarget& rt);
    void DrawHeader(ID2D1RenderTarget& rt);
    void DrawStatsRow(ID2D1RenderTarget& rt);
    void DrawButton(ID2D1RenderTarget& rt);
    void DrawCleanStatus(ID2D1RenderTarget& rt);
    D2D1_RECT_F ZoneRect(int zone, float s) const;
    void StartIntro();
    void RefreshTray();
    static POINT ClientToScreenPt(HWND hwnd, POINT pt);
    void SaveWindowPos();

    MemStats stats_;
    float gaugeShown_ = 0.f;        // animated usage % (also drives arc + number)
    float gaugeTarget_ = 0.f;
    float intro_ = 0.f;             // gauge pop-in
    float chromeAlpha_ = 0.f;       // hover chrome fade
    float btnScale_ = 1.f;          // button press feedback
    float spinnerAng_ = 0.f;
    bool  cleaning_ = false;

    ID2D1LinearGradientBrush* ringGrad_ = nullptr;
    int ringPctKey_ = INT_MIN;

    // Result of the last clean, shown as a line in the card (never a pop-up).
    bool    cleaned_ = false;
    bool    lastDeep_ = false;
    ULONG64 lastFreed_ = 0;

    ULONGLONG lastAutoClean_ = 0;
    int trayBucket_ = -1;
    int trayDrawnPct_ = INT_MIN;    // last percentage rendered to the tray icon
    ULONGLONG lastTrayStamp_ = 0;   // ~12 fps throttle for tray-icon frames
    bool menuOpen_ = false;
    bool trayClickPending_ = false;  // left-up waiting out the double-click time
    bool trayDoubleSeen_ = false;    // swallow the second left-up of a double-click
    TrayIcon tray_;

    friend int AppRun(HINSTANCE);
};
