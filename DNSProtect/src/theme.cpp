// ============================================================================
//  theme.cpp
// ============================================================================

#include "theme.h"

namespace dzp::theme {

namespace {

Palette g_palette;

struct FontCache {
    UINT  dpi = 96;
    HFONT fonts[5] = {};   // indexed by FontKind
};

int FontIndex(FontKind kind) { return static_cast<int>(kind); }

int Px(int pointSize, UINT dpi) { return -MulDiv(pointSize, dpi, 72); }

HFONT MakeFont(int pointSize, int weight, UINT dpi) {
    return CreateFontW(Px(pointSize, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

FontCache g_cache;

}  // namespace

const Palette& Pal() { return g_palette; }

void SetupFor(HWND window) {
    const UINT dpi = [window] {
        if (auto fn = reinterpret_cast<UINT (WINAPI*)(HWND)>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"))) {
            const UINT d = fn(window);
            if (d) return d;
        }
        return 96u;
    }();
    if (dpi && dpi == g_cache.dpi && g_cache.fonts[0]) return;
    g_cache.dpi = dpi;
    for (HFONT& f : g_cache.fonts) {
        if (f) { DeleteObject(f); f = nullptr; }
    }
    // [Title, Big, Body, Small, SmallBold] — index must match FontKind.
    g_cache.fonts[0] = MakeFont(15, FW_SEMIBOLD,  dpi);
    g_cache.fonts[1] = MakeFont(16, FW_SEMIBOLD,  dpi);
    g_cache.fonts[2] = MakeFont(9,  FW_NORMAL,    dpi);
    g_cache.fonts[3] = MakeFont(8,  FW_NORMAL,    dpi);
    g_cache.fonts[4] = MakeFont(8,  FW_SEMIBOLD,  dpi);
}

HFONT Font(FontKind kind) { return g_cache.fonts[FontIndex(kind)]; }

int Scale(int logicalUnits) { return MulDiv(logicalUnits, g_cache.dpi, 96); }

}  // namespace dzp::theme
