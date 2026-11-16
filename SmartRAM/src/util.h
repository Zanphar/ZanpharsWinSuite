// util.h — small shared helpers: colors, formatting, math.
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <psapi.h>
#include <shellapi.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// Tunable warning suppressions for this UI codebase.
#define UNUSED(x) (void)(x)

// ---------------------------------------------------------------- colors ---
inline D2D1_COLOR_F C(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(float((rgb >> 16) & 255) / 255.f,
                        float((rgb >> 8) & 255) / 255.f,
                        float(rgb & 255) / 255.f,
                        a);
}

inline D2D1_COLOR_F InterpColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    return D2D1_COLOR_F{ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                         a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

inline D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& c, float a) {
    return D2D1_COLOR_F{ c.r, c.g, c.b, a };
}

// ------------------------------------------------------------ geometry ----
inline float DegToRad(float d) { return d * 3.14159265358979323846f / 180.f; }
inline float D2DPointOnX(float cx, float r, float deg) { return cx + r * cosf(DegToRad(deg)); }
inline float D2DPointOnY(float cy, float r, float deg) { return cy + r * sinf(DegToRad(deg)); }

inline float LerpFloat(float a, float b, float t) { return a + (b - a) * t; }
inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
inline float EaseOutCubic(float t) { t = Clamp01(t); float u = 1.f - t; return 1.f - u * u * u; }
inline float EaseOutBack(float t) {
    t = Clamp01(t);
    const float c1 = 1.70158f, c3 = c1 + 1.f;
    float u = t - 1.f;
    return 1.f + c3 * u * u * u + c1 * u * u;
}

// ----------------------------------------------------------- formatting ---
inline std::wstring FmtDoubleText(double v, int prec) {
    wchar_t buf[64];
    swprintf_s(buf, L"%.*f", prec, v);
    return buf;
}

// "6.4 GB" / "900 MB" style.
inline std::wstring FormatBytes(ULONG64 bytes, int prec = 1) {
    const double gb = double(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0) return FmtDoubleText(gb, prec) + L" GB";
    return FmtDoubleText(double(bytes) / (1024.0 * 1024.0), prec) + L" MB";
}
