// Zanphar's WinSuite - drawing toolkit
#include "ui.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>

using namespace Gdiplus;

namespace theme {
COLORREF bg = RGB(13, 16, 21);
COLORREF sidebar = RGB(18, 23, 30);
COLORREF card = RGB(24, 31, 40);
COLORREF cardEdge = RGB(38, 48, 60);
COLORREF hover = RGB(33, 42, 53);
COLORREF active = RGB(45, 57, 71);
COLORREF accent = RGB(66, 220, 138);
COLORREF accentSoft = RGB(24, 58, 42);
COLORREF text = RGB(232, 238, 245);
COLORREF muted = RGB(146, 161, 178);
COLORREF faint = RGB(103, 117, 133);
COLORREF danger = RGB(240, 97, 90);
COLORREF warn = RGB(231, 184, 75);
}  // namespace theme

Fonts g_fonts;

static ULONG_PTR g_gdiplusToken = 0;

void UiStartup() {
    GdiplusStartupInput input;
    GdiplusStartup(&g_gdiplusToken, &input, nullptr);
}

void UiShutdown() {
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

static HFONT MakeFont(int points, int weight, const wchar_t* face, float scale) {
    LOGFONTW lf = {};
    lf.lfHeight = -(int)std::lround(points * scale * 96.0 / 72.0);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfPitchAndFamily = wcsstr(face, L"Consolas") ? FIXED_PITCH | FF_MODERN
                                                    : VARIABLE_PITCH | FF_SWISS;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

void Fonts::Create(float scale) {
    Destroy();
    base = MakeFont(10, FW_NORMAL, L"Segoe UI", scale);
    small = MakeFont(9, FW_NORMAL, L"Segoe UI", scale);
    tiny = MakeFont(8, FW_NORMAL, L"Segoe UI", scale);
    h1 = MakeFont(21, FW_SEMIBOLD, L"Segoe UI", scale);
    h2 = MakeFont(12, FW_SEMIBOLD, L"Segoe UI", scale);
    h3 = MakeFont(10, FW_SEMIBOLD, L"Segoe UI", scale);
    big = MakeFont(25, FW_SEMIBOLD, L"Segoe UI", scale);
    mono = MakeFont(9, FW_NORMAL, L"Consolas", scale);
    monoSmall = MakeFont(8, FW_NORMAL, L"Consolas", scale);
}

void Fonts::Destroy() {
    for (HFONT* f : {&base, &small, &tiny, &h1, &h2, &h3, &big, &mono, &monoSmall}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

void FillRect(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH brush = CreateSolidBrush(c);
    ::FillRect(dc, (LPRECT)&r, brush);
    DeleteObject(brush);
}

void FillRound(HDC dc, const RECT& r, float radius, COLORREF c) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    GraphicsPath path;
    float d = std::min({radius * 2.0f, (float)(r.right - r.left), (float)(r.bottom - r.top)});
    float x = (float)r.left, y = (float)r.top;
    float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    if (d <= 0.5f) {
        path.AddRectangle(RectF(x, y, w, h));
    } else {
        path.AddArc(x, y, d, d, 180.0f, 90.0f);
        path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
        path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
        path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }
    SolidBrush brush(Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
    g.FillPath(&brush, &path);
}

void StrokeRound(HDC dc, const RECT& r, float radius, COLORREF c, float width) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    GraphicsPath path;
    float d = std::min({radius * 2.0f, (float)(r.right - r.left), (float)(r.bottom - r.top)});
    float x = (float)r.left + width / 2.0f, y = (float)r.top + width / 2.0f;
    float w = (float)(r.right - r.left) - width, h = (float)(r.bottom - r.top) - width;
    if (d <= 0.5f) {
        path.AddRectangle(RectF(x, y, w, h));
    } else {
        path.AddArc(x, y, d, d, 180.0f, 90.0f);
        path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
        path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
        path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }
    Pen pen(Color(255, GetRValue(c), GetGValue(c), GetBValue(c)), width);
    g.DrawPath(&pen, &path);
}

void FillCircle(HDC dc, int cx, int cy, int radius, COLORREF c) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush brush(Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
    g.FillEllipse(&brush, cx - radius, cy - radius, radius * 2, radius * 2);
}

void TextAt(HDC dc, const std::wstring& s, int x, int y, HFONT f, COLORREF c) {
    if (s.empty()) return;
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    TextOutW(dc, x, y, s.c_str(), (int)s.size());
    SelectObject(dc, old);
}

void TextIn(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT flags) {
    if (s.empty()) return;
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags);
    SelectObject(dc, old);
}

int TextWidth(HDC dc, const std::wstring& s, HFONT f) {
    if (s.empty()) return 0;
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

std::wstring Ellipsize(HDC dc, const std::wstring& s, HFONT f, int maxWidth) {
    if (maxWidth <= 0) return std::wstring();
    if (TextWidth(dc, s, f) <= maxWidth) return s;
    std::wstring out = s;
    while (!out.empty() && TextWidth(dc, out + L"...", f) > maxWidth) out.pop_back();
    return out + L"...";
}

void HLine(HDC dc, int x1, int x2, int y, COLORREF c) {
    RECT r = {x1, y, x2, y + 1};
    FillRect(dc, r, c);
}

void Progress(HDC dc, const RECT& r, double pct, COLORREF fill) {
    FillRound(dc, r, (float)(r.bottom - r.top) / 2.0f, theme::hover);
    if (pct <= 0) return;
    if (pct > 100) pct = 100;
    int width = (int)std::lround((r.right - r.left) * pct / 100.0);
    if (width < (r.bottom - r.top)) width = (r.bottom - r.top);
    int limit = (int)(r.right - r.left);
    RECT fillRect = {r.left, r.top, r.left + std::min(width, limit), r.bottom};
    FillRound(dc, fillRect, (float)(r.bottom - r.top) / 2.0f, fill);
}

void Pill(HDC dc, int x, int y, const std::wstring& label, COLORREF bgc, COLORREF fg, HFONT f,
          int* outWidth) {
    int padX = 9;
    int h = 21;
    int w = TextWidth(dc, label, f) + padX * 2;
    RECT r = {x, y, x + w, y + h};
    FillRound(dc, r, (float)h / 2.0f, bgc);
    RECT tr = r;
    TextIn(dc, label, tr, f, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (outWidth) *outWidth = w;
}

WholeRowList FitWholeRows(int top, int bottom, int rowHeight, int gap, int count,
                          int scroll) {
    WholeRowList list;
    const int pitch = rowHeight + gap > 0 ? rowHeight + gap : 1;
    const int height = bottom - top;

    // Only as many rows as fit whole; the viewport ends on the last row's bottom edge,
    // never inside a row.
    int view = 0;
    if (rowHeight > 0 && height >= rowHeight) {
        const int rows = (height + gap) / pitch;
        view = rows * pitch - gap;
    }
    if (view < 0) view = 0;
    list.bottom = top + view;

    // The content is the same shape - rows with gaps between them - so the furthest
    // scroll is a whole number of rows as well.
    const int content = count > 0 ? count * pitch - gap : 0;
    const int furthest = content - view;
    list.maxScroll = furthest > 0 ? furthest / pitch * pitch : 0;

    const int offset = scroll > 0 ? scroll : 0;
    int snapped = (offset + pitch / 2) / pitch * pitch;  // nearest row, both ways
    if (snapped > list.maxScroll) snapped = list.maxScroll;
    list.scroll = snapped;
    return list;
}

void Button(HDC dc, const RECT& r, const std::wstring& label, bool primary, bool hovered,
            bool enabled) {
    if (primary) {
        COLORREF fill = !enabled ? theme::hover : hovered ? RGB(88, 236, 160) : theme::accent;
        FillRound(dc, r, 8.0f, fill);
        TextIn(dc, label, r, g_fonts.h3, RGB(9, 26, 18), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        FillRound(dc, r, 8.0f, hovered ? theme::hover : theme::card);
        StrokeRound(dc, r, 8.0f, hovered ? theme::active : theme::cardEdge, 1.0f);
        TextIn(dc, label, r, g_fonts.h3, enabled ? theme::text : theme::faint,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}
