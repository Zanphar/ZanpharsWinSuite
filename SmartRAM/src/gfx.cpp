// gfx.cpp — Direct2D/DirectWrite plumbing: factories, resource kit, drawing helpers.
#include "gfx.h"

namespace gfx {

static com_ptr<ID2D1Factory>   g_d2d;
static com_ptr<IDWriteFactory> g_dw;

bool InitFactories() {
    if (!g_d2d) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
        if (FAILED(hr)) return false;
    }
    if (!g_dw) {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(&g_dw));
        if (FAILED(hr)) return false;
    }
    return true;
}
com_ptr<ID2D1Factory>&   Factory() { return g_d2d; }
com_ptr<IDWriteFactory>& DWrite()  { return g_dw; }

// ------------------------------------------------------------ UiKit --------
static void MakeFormat(com_ptr<IDWriteTextFormat>& f, const wchar_t* family, float px,
                       DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    if (FAILED(DWrite()->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, px, L"en-us", &f)))
        return;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void UiKit::ReleaseAll() {
    title.release(); sub.release(); big.release(); unit.release(); legend.release();
    label.release(); value.release(); btn.release(); mini.release();
    roundCaps.release();
    hover.release(); white07.release(); white09.release(); white14.release(); white30.release();
    txtHi.release(); txtMed.release(); txtDim.release(); black.release(); okGreen.release();
    cardBg.release(); buttonGrad.release(); toggleGrad.release();
    ready_ = false;
}

void UiKit::Ensure(ID2D1RenderTarget& rt, float s) {
    if (ready_) return;
    ready_ = true;

    static const wchar_t* kFamily = L"Segoe UI";
    auto mk = [&](com_ptr<IDWriteTextFormat>& f, float px, DWRITE_FONT_WEIGHT w,
                  DWRITE_TEXT_ALIGNMENT align) {
        MakeFormat(f, kFamily, px * s, w, align);
    };

    mk(title,  12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(sub,    11.f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(big,    52.f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(unit,   20.f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(legend, 11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    mk(label,  15.f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(value,  16.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(btn,    15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    mk(mini,   13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);

    D2D1_STROKE_STYLE_PROPERTIES props = {};
    props.startCap = D2D1_CAP_STYLE_ROUND;
    props.endCap   = D2D1_CAP_STYLE_ROUND;
    props.dashCap  = D2D1_CAP_STYLE_ROUND;
    props.lineJoin = D2D1_LINE_JOIN_ROUND;
    props.miterLimit = 10.f;
    Factory()->CreateStrokeStyle(&props, nullptr, 0, &roundCaps);

    auto solid = [&](com_ptr<ID2D1SolidColorBrush>& b, const D2D1_COLOR_F& c) {
        rt.CreateSolidColorBrush(c, &b);
    };

    solid(hover,    C(0xFFFFFF, 0.06f));
    solid(white07,  C(0xFFFFFF, 0.07f));
    solid(white09,  C(0xFFFFFF, 0.09f));
    solid(white14,  C(0xFFFFFF, 0.14f));
    solid(white30,  C(0xFFFFFF, 0.30f));
    solid(txtHi,    C(0xF2F6FC));
    solid(txtMed,   C(0x8A96AC));
    solid(txtDim,   C(0x55617A));
    solid(black,    C(0x000000, 0.55f));
    solid(okGreen,  C(0x34D399));

    const D2D1_SIZE_F size = rt.GetSize();
    com_ptr<ID2D1GradientStopCollection> stops;
    D2D1_GRADIENT_STOP gs[2] = {
        { 0.f, C(0x232833) },
        { 1.f, C(0x0B0E14) },
    };
    if (SUCCEEDED(rt.CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2,
                                                  D2D1_EXTEND_MODE_CLAMP, &stops))) {
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES gp = {};
        gp.startPoint = D2D1_POINT_2F{ 0.f, 0.f };
        gp.endPoint   = D2D1_POINT_2F{ 0.f, size.height };
        rt.CreateLinearGradientBrush(gp, stops, &cardBg);

        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES bp = {};
        bp.startPoint = D2D1_POINT_2F{ 0.f, 0.f };
        bp.endPoint   = D2D1_POINT_2F{ size.width, 0.f };
        D2D1_GRADIENT_STOP bs[2] = { { 0.f, C(kBtnA) }, { 1.f, C(kBtnB) } };
        com_ptr<ID2D1GradientStopCollection> stops2;
        if (SUCCEEDED(rt.CreateGradientStopCollection(bs, 2, D2D1_GAMMA_2_2,
                                                      D2D1_EXTEND_MODE_CLAMP, &stops2)))
            rt.CreateLinearGradientBrush(bp, stops2, &buttonGrad);

        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES tp = {};
        tp.startPoint = D2D1_POINT_2F{ 0.f, 0.f };
        tp.endPoint   = D2D1_POINT_2F{ 0.f, size.height };
        D2D1_GRADIENT_STOP ts[2] = { { 0.f, C(kAccentB) }, { 1.f, C(kAccentA) } };
        com_ptr<ID2D1GradientStopCollection> stops3;
        if (SUCCEEDED(rt.CreateGradientStopCollection(ts, 2, D2D1_GAMMA_2_2,
                                                      D2D1_EXTEND_MODE_CLAMP, &stops3)))
            rt.CreateLinearGradientBrush(tp, stops3, &toggleGrad);
    }
}

ID2D1LinearGradientBrush* UiKit::MakeRingGrad(ID2D1RenderTarget& rt, const D2D1_COLOR_F& a,
                                              const D2D1_COLOR_F& b, float cx, float cy, float r) {
    com_ptr<ID2D1GradientStopCollection> stops;
    D2D1_GRADIENT_STOP gs[2] = { { 0.f, a }, { 1.f, b } };
    if (FAILED(rt.CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2,
                                               D2D1_EXTEND_MODE_CLAMP, &stops)))
        return nullptr;
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES gp = {};
    gp.startPoint = D2D1_POINT_2F{ cx, cy - r };
    gp.endPoint   = D2D1_POINT_2F{ cx, cy + r };
    com_ptr<ID2D1LinearGradientBrush> brush;
    if (FAILED(rt.CreateLinearGradientBrush(gp, stops, &brush)))
        return nullptr;
    return brush.detach();  // ownership transfers to caller
}

// --------------------------------------------------------- primitives ------
ArcGeom ArcPath(float cx, float cy, float radius, float startDeg, float sweepDeg) {
    ArcGeom g;
    if (FAILED(Factory()->CreatePathGeometry(&g.geo))) return g;
    com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(g.geo->Open(&sink))) return g;

    const float a0 = DegToRad(startDeg);
    const float a1 = DegToRad(startDeg + sweepDeg);
    const D2D1_POINT_2F p0 = { cx + radius * cosf(a0), cy + radius * sinf(a0) };
    const D2D1_POINT_2F p1 = { cx + radius * cosf(a1), cy + radius * sinf(a1) };

    sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_FILLED);
    D2D1_ARC_SEGMENT arc = {};
    arc.point = p1;
    arc.size  = D2D1_SIZE_F{ radius, radius };
    arc.rotationAngle = 0.f;
    arc.sweepDirection = D2D1_SWEEP_DIRECTION_CLOCKWISE;
    arc.arcSize = sweepDeg > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
    sink->AddArc(&arc);
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    return g;
}

void StrokeArc(ID2D1RenderTarget& rt, float cx, float cy, float r, float width,
               float startDeg, float sweepDeg, ID2D1Brush& brush, ID2D1StrokeStyle* style) {
    if (sweepDeg <= 0.01f) return;
    auto g = ArcPath(cx, cy, r, startDeg, sweepDeg);
    if (g.geo) rt.DrawGeometry(g.geo.get(), &brush, width, style);
}

void DrawTextCentered(ID2D1RenderTarget& rt, const wchar_t* text, IDWriteTextFormat& fmt,
                      const D2D1_RECT_F& rect, ID2D1Brush& brush) {
    fmt.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    rt.DrawText(text, (UINT32)wcslen(text), &fmt, rect, &brush, D2D1_DRAW_TEXT_OPTIONS_NONE,
                DWRITE_MEASURING_MODE_NATURAL);
}
void DrawTextLeft(ID2D1RenderTarget& rt, const wchar_t* text, IDWriteTextFormat& fmt,
                  const D2D1_RECT_F& rect, ID2D1Brush& brush) {
    fmt.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    rt.DrawText(text, (UINT32)wcslen(text), &fmt, rect, &brush, D2D1_DRAW_TEXT_OPTIONS_NONE,
                DWRITE_MEASURING_MODE_NATURAL);
}
void DrawTextRight(ID2D1RenderTarget& rt, const wchar_t* text, IDWriteTextFormat& fmt,
                   const D2D1_RECT_F& rect, ID2D1Brush& brush) {
    fmt.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    rt.DrawText(text, (UINT32)wcslen(text), &fmt, rect, &brush, D2D1_DRAW_TEXT_OPTIONS_NONE,
                DWRITE_MEASURING_MODE_NATURAL);
}

void OutlineRoundRect(ID2D1RenderTarget& rt, const D2D1_ROUNDED_RECT& rr, ID2D1Brush& brush,
                      float w) {
    rt.DrawRoundedRectangle(rr, &brush, w);
}

// ------------------------------------------------------------ icon --------
HICON MakeIcon(int px, const std::function<void(ID2D1RenderTarget&)>& draw) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = px;
    bi.bmiHeader.biHeight      = -px;      // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { ReleaseDC(nullptr, screen); return nullptr; }
    ZeroMemory(bits, (size_t)px * px * 4);

    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bmp);

    com_ptr<ID2D1DCRenderTarget> rt;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.f, 0.f, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
    HRESULT hr = Factory()->CreateDCRenderTarget(&props, &rt);
    if (SUCCEEDED(hr)) {
        RECT rc = { 0, 0, px, px };
        hr = rt->BindDC(mem, &rc);
        if (SUCCEEDED(hr)) {
            rt->BeginDraw();
            draw(*rt.get());
            rt->EndDraw();       // flush to the GDI DC — without this the icon stays blank
        }
    }
    (void)hr;

    SelectObject(mem, old);
    DeleteDC(mem);

    std::vector<DWORD> zeroMask((size_t)px * px / 32 + 1, 0);
    HBITMAP mask = CreateBitmap(px, px, 1, 1, zeroMask.data());

    ICONINFO ii = {};
    ii.fIcon   = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask  = mask;
    ii.hbmColor = bmp;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(mask);
    DeleteObject(bmp);
    ReleaseDC(nullptr, screen);
    return icon;
}

}  // namespace gfx
