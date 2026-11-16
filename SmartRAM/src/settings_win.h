// settings_win.h — the settings card.
#pragma once
#include "layerwin.h"
#include "app.h"

class SettingsWindow : public LayerWindow {
public:
    bool EnsureCreated(HINSTANCE hInst);
    void Reveal();      // show + activate (existing window is reused)

protected:
    void OnDraw(ID2D1RenderTarget& rt, float scale) override;
    int  HitTestZone(POINT ptClient) const override;
    void OnClick(int zone, POINT ptClient) override;
    void OnAnimTick(float dt) override;
    bool NeedsAnim() const override;
    bool OnCustomMsg(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* out) override;

private:
    enum {
        Z_ROW0_TOGGLE = 1,   // smart clean toggle
        Z_ROW0_SLIDER,       // threshold slider
        Z_ROW1, Z_ROW2, Z_ROW3, Z_ROW4, Z_ROW5, Z_ROW6,   // idle / clip / top / startup / tray rows
        Z_CLOSE
    };

    void ApplyZoneToggle(int zone);
    void OnCaptureMove(int zone, POINT pt) override;
    void PlaceNearWidget();
    void CloseCard();

    float knobAnim_ = 0.f;   // shared toggle knob progress (drives anim channel)
    float sliderAnim_ = 0.f;
    bool  sliderDragging_ = false;
    ULONG64 lastAnimStamp_ = 0;
};
