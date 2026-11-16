// ============================================================================
//  theme.h — dark colour palette, DPI scaling helpers and cached fonts.
// ============================================================================

#pragma once
#include "common.h"

namespace dzp::theme {

struct Palette {
    COLORREF Background   = RGB(24, 25, 31);
    COLORREF Card         = RGB(34, 36, 44);
    COLORREF CardHover    = RGB(42, 44, 53);
    COLORREF CardSelected = RGB(46, 56, 76);
    COLORREF Stroke       = RGB(57, 60, 71);
    COLORREF Text         = RGB(236, 238, 244);
    COLORREF TextDim      = RGB(146, 152, 164);
    COLORREF TextDisabled = RGB(104, 109, 120);
    COLORREF Accent       = RGB(84, 158, 255);
    COLORREF AccentHover  = RGB(64, 134, 236);
    COLORREF AccentPress  = RGB(52, 112, 214);
    COLORREF OnAccent     = RGB(250, 252, 255);
    COLORREF Ok           = RGB(88, 205, 143);
    COLORREF Warn         = RGB(242, 178, 84);
    COLORREF Danger       = RGB(244, 106, 100);
};

const Palette& Pal();

enum class FontKind {
    Title,    // 15 pt semibold  — window title
    Big,      // 16 pt semibold  — hero status line
    Body,     //  9 pt regular   — general UI text
    Small,    //  8 pt regular   — captions, metadata
    SmallBold,//  8 pt semibold  — example values
};

// (Re)creates the cached fonts for `window`'s DPI. Call from WM_CREATE and
// WM_DPICHANGED before painting.
void SetupFor(HWND window);

HFONT Font(FontKind kind);

// Converts logical units measured at 96 dpi into device pixels.
int Scale(int logicalUnits);

}  // namespace dzp::theme
