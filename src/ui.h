// Zanphar's WinSuite - drawing toolkit
#pragma once

#include <windows.h>
#include <string>

namespace theme {
extern COLORREF bg;
extern COLORREF sidebar;
extern COLORREF card;
extern COLORREF cardEdge;
extern COLORREF hover;
extern COLORREF active;
extern COLORREF accent;
extern COLORREF accentSoft;
extern COLORREF text;
extern COLORREF muted;
extern COLORREF faint;
extern COLORREF danger;
extern COLORREF warn;
}  // namespace theme

struct Fonts {
    HFONT base = nullptr;
    HFONT small = nullptr;
    HFONT tiny = nullptr;
    HFONT h1 = nullptr;
    HFONT h2 = nullptr;
    HFONT h3 = nullptr;
    HFONT big = nullptr;
    HFONT mono = nullptr;
    HFONT monoSmall = nullptr;

    void Create(float scale);
    void Destroy();
};

extern Fonts g_fonts;

void UiStartup();
void UiShutdown();

void FillRect(HDC dc, const RECT& r, COLORREF c);
void FillRound(HDC dc, const RECT& r, float radius, COLORREF c);
void StrokeRound(HDC dc, const RECT& r, float radius, COLORREF c, float width = 1.0f);
void FillCircle(HDC dc, int cx, int cy, int radius, COLORREF c);

void TextAt(HDC dc, const std::wstring& s, int x, int y, HFONT f, COLORREF c);
void TextIn(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT flags);
int TextWidth(HDC dc, const std::wstring& s, HFONT f);
std::wstring Ellipsize(HDC dc, const std::wstring& s, HFONT f, int maxWidth);

void HLine(HDC dc, int x1, int x2, int y, COLORREF c);
void Progress(HDC dc, const RECT& r, double pct, COLORREF fill);
void Pill(HDC dc, int x, int y, const std::wstring& label, COLORREF bgc, COLORREF fg,
          HFONT f, int* outWidth = nullptr);

void Button(HDC dc, const RECT& r, const std::wstring& label, bool primary, bool hovered,
            bool enabled = true);

// A list viewport that only ever shows whole rows. The clipped bottom lands on a row
// boundary and the scroll offset is a whole number of row pitches, so neither the wheel
// nor the clipping can leave a row cut in half at the bottom.
struct WholeRowList {
    int bottom = 0;     // where the clipped list ends
    int maxScroll = 0;  // always a whole number of rows
    int scroll = 0;     // the offset to draw the rows with
};

WholeRowList FitWholeRows(int top, int bottom, int rowHeight, int gap, int count, int scroll);
