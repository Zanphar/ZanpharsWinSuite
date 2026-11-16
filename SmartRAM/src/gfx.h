// gfx.h — Direct2D/DirectWrite plumbing: factories, resource kit, drawing helpers.
#pragma once
#include "util.h"
#include "comptr.h"
#include <dwrite.h>
#include <functional>

namespace gfx {

bool InitFactories();                               // D2D + DWrite, once per process
com_ptr<ID2D1Factory>&   Factory();
com_ptr<IDWriteFactory>& DWrite();

HICON MakeIcon(int px, const std::function<void(ID2D1RenderTarget&)>& draw);

// ------------------------------------------------------------ UiKit -------
// Fonts + brushes owned by a window. Rebuild with Ensure() after RT or DPI change
// (each window has its own instance because brushes are bound to one render target).
class UiKit {
public:
    void Ensure(ID2D1RenderTarget& rt, float scale);
    void Invalidate() { ready_ = false; }
    bool IsReady() const { return ready_; }
    void ReleaseAll();

    // fonts
    com_ptr<IDWriteTextFormat> title, sub, big, unit, legend, label, value, btn, mini;
    com_ptr<ID2D1StrokeStyle>  roundCaps;

    // brushes
    com_ptr<ID2D1SolidColorBrush>    hover, white07, white09, white14, white30;
    com_ptr<ID2D1SolidColorBrush>    txtHi, txtMed, txtDim, black, okGreen;
    com_ptr<ID2D1LinearGradientBrush> cardBg;        // vertical, sized to render target
    com_ptr<ID2D1LinearGradientBrush> buttonGrad;    // horizontal accent
    com_ptr<ID2D1LinearGradientBrush> toggleGrad;    // accent for "on" state

    ID2D1LinearGradientBrush* MakeRingGrad(ID2D1RenderTarget& rt, const D2D1_COLOR_F& a,
                                           const D2D1_COLOR_F& b, float cx, float cy, float r);

private:
    bool ready_ = false;
};

// --------------------------------------------------------- primitives -----
struct ArcGeom { com_ptr<ID2D1PathGeometry> geo; };
ArcGeom ArcPath(float cx, float cy, float radius, float startDeg, float sweepDeg);
void    StrokeArc(ID2D1RenderTarget& rt, float cx, float cy, float r, float width,
                  float startDeg, float sweepDeg, ID2D1Brush& brush, ID2D1StrokeStyle* style);
void    DrawTextCentered(ID2D1RenderTarget& rt, const wchar_t* text, IDWriteTextFormat& fmt,
                         const D2D1_RECT_F& rect, ID2D1Brush& brush);
void    DrawTextLeft(ID2D1RenderTarget& rt, const wchar_t* text, IDWriteTextFormat& fmt,
                     const D2D1_RECT_F& rect, ID2D1Brush& brush);
void    DrawTextRight(ID2D1RenderTarget& rt, const wchar_t* text, IDWriteTextFormat& fmt,
                      const D2D1_RECT_F& rect, ID2D1Brush& brush);
void    OutlineRoundRect(ID2D1RenderTarget& rt, const D2D1_ROUNDED_RECT& rr, ID2D1Brush& brush,
                         float w);

// ------------------------------------------------------------ palette -----
constexpr uint32_t kAccentA = 0x22D3EE;   // cyan
constexpr uint32_t kAccentB = 0x34D399;   // emerald
constexpr uint32_t kWarnA   = 0xFBBF24;   // amber
constexpr uint32_t kWarnB   = 0xF97316;   // orange
constexpr uint32_t kHot     = 0xF87171;   // red
constexpr uint32_t kBtnA    = 0x10B981;
constexpr uint32_t kBtnB    = 0x0EA5E9;

}  // namespace gfx
