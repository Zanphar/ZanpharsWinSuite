// Internet Connection Utility
// Copyright (c) 2026-2027 Charles McDonald. BSD 3-Clause License.
// Part of Zanphar's WinSuite; see LICENSE.md in the suite root.

// Internet Connection Utility - custom drawn Win32 / GDI+ interface
#include "ui.h"

#include "../res/resource.h"
#include "doctor.h"

#include <gdiplus.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace {

const UINT kRefreshMsg = WM_APP + 21;

constexpr int kBg = 0x070B12;
constexpr int kSide = 0x0A1019;
constexpr int kCard = 0x0E1623;
constexpr int kCardHi = 0x131F30;
constexpr int kBorder = 0x1A2433;
constexpr int kBorderHi = 0x2A3A51;
constexpr int kText = 0xE8EFF9;
constexpr int kDim = 0x8698AE;
constexpr int kFaint = 0x5C6D82;
constexpr int kAccent = 0x2DD4BF;
constexpr int kBlue = 0x4F8DFD;

Color C(int rgb, int a = 255) {
    return Color((BYTE)a, (BYTE)((rgb >> 16) & 0xFF), (BYTE)((rgb >> 8) & 0xFF), (BYTE)(rgb & 0xFF));
}

FontFamily* Fam() {
    static FontFamily f(L"Segoe UI");
    return &f;
}
FontFamily* MonoFam() {
    static FontFamily f(L"Consolas");
    return &f;
}

Font* F(float px, bool bold = false) {
    static std::map<int, Font*> cache;
    int key = (int)(px * 10) + (bold ? 100000 : 0);
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto* f = new Font(Fam(), px, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
        it = cache.emplace(key, f).first;
    }
    return it->second;
}

Font* FM(float px) {
    static std::map<int, Font*> cache;
    int key = (int)(px * 10);
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto* f = new Font(MonoFam(), px, FontStyleRegular, UnitPixel);
        it = cache.emplace(key, f).first;
    }
    return it->second;
}

void Txt(Graphics& g, const std::wstring& s, Font* f, Color c, RectF r,
         StringAlignment ha = StringAlignmentNear, StringAlignment va = StringAlignmentNear,
         bool wrap = false) {
    if (s.empty()) return;
    StringFormat fmt;
    fmt.SetAlignment(ha);
    fmt.SetLineAlignment(va);
    fmt.SetTrimming(wrap ? StringTrimmingNone : StringTrimmingEllipsisCharacter);
    fmt.SetHotkeyPrefix(HotkeyPrefixNone);
    if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, f, r, &fmt, &b);
}

float TextH(Graphics& g, const std::wstring& s, Font* f, float w) {
    StringFormat fmt;
    RectF r(0, 0, w, 4000), out;
    g.MeasureString(s.c_str(), -1, f, r, &fmt, &out);
    return out.Height;
}

void Round(GraphicsPath& p, RectF r, float rad) {
    float d = rad * 2;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

void FillRound(Graphics& g, RectF r, float rad, Color c) {
    GraphicsPath p;
    Round(p, r, rad);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

void StrokeRound(Graphics& g, RectF r, float rad, Color c, float w = 1.f) {
    GraphicsPath p;
    Round(p, r, rad);
    Pen pen(c, w);
    g.DrawPath(&pen, &p);
}

// Typed wrappers keep GDI+'s INT/REAL overloads from becoming ambiguous.
void Ln(Graphics& g, const Pen* p, float x1, float y1, float x2, float y2) {
    g.DrawLine(p, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2);
}
void Ell(Graphics& g, const Pen* p, float x, float y, float w, float h) {
    g.DrawEllipse(p, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
}
void EllF(Graphics& g, const Brush* b, float x, float y, float w, float h) {
    g.FillEllipse(b, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
}
void Arc(Graphics& g, const Pen* p, float x, float y, float w, float h, float a, float s) {
    g.DrawArc(p, (REAL)x, (REAL)y, (REAL)w, (REAL)h, (REAL)a, (REAL)s);
}

void Glow(Graphics& g, float cx, float cy, float r, int rgb, int strength) {
    for (int i = 6; i >= 1; --i) {
        float rr = r * (0.55f + i * 0.14f);
        SolidBrush b(C(rgb, (int)(strength * (7 - i) / 42.0)));
        EllF(g, &b, cx - rr, cy - rr, rr * 2, rr * 2);
    }
}

void Bars(Graphics& g, float x, float y, int pct, Color c) {
    for (int i = 0; i < 4; ++i) {
        float h = 4 + i * 4.0f;
        RectF r(x + i * 6, y + (18 - h), 4, h);
        bool on = pct >= (i + 1) * 25 - 1;
        FillRound(g, r, 1.5f, on ? c : C(0x2A3A4E));
    }
}

enum HitKind {
    HIT_NONE = 0,
    HIT_NAV = 1,
    HIT_CHIP = 2,
    HIT_ROW = 3,
    HIT_ROWFIX = 4,
    HIT_FIXAPPLY = 5,
    HIT_SCAN = 6,
    HIT_FIXALL = 7,
    HIT_ELEVATE = 8,
    HIT_COPY = 9,
    HIT_ABOUT = 10
};

struct Hit {
    int kind = HIT_NONE;
    int idx = 0;
    RectF r;
};

struct RowVis {
    int idx = 0;
    RectF r;
    float h = 58;
};

struct App {
    HWND hwnd = nullptr;
    Doctor doc;
    AppState st;
    int tab = 0;
    int filter = 0;
    float scroll[4] = {0, 0, 0, 0};
    int expanded = -1;
    float dispScore = 0, spin = 0;
    float S = 1.f;
    int cw = 0, ch = 0;
    Bitmap* bb = nullptr;
    Graphics* bg = nullptr;
    std::vector<Hit> hits;
    std::vector<RowVis> rows;
    float contentH = 0;
    ULONGLONG lastPoll = 0;
    std::wstring toast;
    ULONGLONG toastUntil = 0;
    int hoverKind = 0, hoverIdx = -1;
    bool started = false;
    unsigned long long lastFixCounter = 0;
};

App g_app;

// ------------------------------------------------------------------ helpers
const FixDef* findFix(const AppState& st, const std::string& id) {
    for (auto& f : st.fixes)
        if (f.id == id) return &f;
    return nullptr;
}

void Toast(const std::wstring& s) {
    g_app.toast = s;
    g_app.toastUntil = GetTickCount64() + 4200;
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

bool ConfirmFix(const std::wstring& name, const std::wstring& note) {
    std::wstring msg = L"Apply this repair?\n\n" + name + L"\n\n" + note;
    return MessageBoxW(g_app.hwnd, msg.c_str(), L"Internet Connection Utility \u2014 confirm repair",
                       MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

std::wstring BuildReport(const AppState& st) {
    std::wstring r;
    r += L"Internet Connection Utility \u2014 Connection Doctor report\r\n";
    r += L"Generated " + st.scannedAt + L"  \u00b7  score " + std::to_wstring(st.score) + L"/100\r\n\r\n";
    r += L"Adapter : " + st.adapter.name + L" (" + st.adapter.desc + L")\r\n";
    r += L"Address : " + net::utf82wstr(st.adapter.ipv4) + L" / " + net::utf82wstr(st.adapter.mask) +
         L"   gateway " + net::utf82wstr(st.adapter.gateway) + L"\r\n";
    if (!st.adapter.dns.empty()) {
        r += L"DNS     : ";
        for (size_t i = 0; i < st.adapter.dns.size(); ++i) {
            if (i) r += L", ";
            r += net::utf82wstr(st.adapter.dns[i]);
        }
        r += L"\r\n";
    }
    r += L"\r\n";
    for (auto& i : st.issues) {
        r += L"[" + std::wstring(sevLabel(i.sev)) + L"] " + i.title;
        if (!i.metric.empty()) r += L"  (" + i.metric + L")";
        r += L"\r\n    " + i.detail + L"\r\n";
        if (i.sev == Sev::Warn || i.sev == Sev::Critical) {
            if (!i.advice.empty()) r += L"    " + i.advice + L"\r\n";
            if (!i.fixLabel.empty()) r += L"    Fix: " + i.fixLabel + L"\r\n";
        }
    }
    r += L"\r\nFixes are applied at your own risk. Some require administrator rights.\r\n";
    return r;
}

void CopyReport() {
    std::wstring text = BuildReport(g_app.st);
    if (!OpenClipboard(g_app.hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* p = GlobalLock(mem);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();
    Toast(L"Report copied to the clipboard.");
}

// ------------------------------------------------------------------ chrome
void DrawBackdrop(Graphics& g, float W, float H) {
    SolidBrush bg(C(kBg));
    g.FillRectangle(&bg, RectF(0, 0, W, H));

    Glow(g, W - 40, 40, 300, kBlue, 46);
    Glow(g, 210, H - 30, 280, kAccent, 30);
    Glow(g, W * 0.62f, 250, 420, 0x1E3A8A, 34);
}

void NavIcon(Graphics& g, int tab, float x, float y, Color c) {
    Pen pen(c, 1.7f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    if (tab == 0) {
        Ell(g, &pen, x + 1, y + 1, 14, 14);
        PointF pts[4] = {{x + 4.2f, y + 9}, {x + 7, y + 6.2f}, {x + 10, y + 11.2f}, {x + 13.4f, y + 7.4f}};
        g.DrawLines(&pen, pts, 4);
    } else if (tab == 1) {
        for (int i = 0; i < 3; ++i) {
            float inset = i * 3.6f;
            Arc(g, &pen, x + inset, y + inset + 2, 16 - inset * 2, 16 - inset * 2, 200, 140);
        }
        SolidBrush b(c);
        EllF(g, &b, x + 6.5f, y + 11, 3.4f, 3.4f);
    } else if (tab == 2) {
        Ln(g, &pen, x + 3, y + 12.5f, x + 13, y + 2.5f);
        SolidBrush b(c);
        EllF(g, &b, x + 1.2f, y + 11.6f, 4.6f, 4.6f);
        EllF(g, &b, x + 11.2f, y + 1.6f, 4.6f, 4.6f);
    } else {
        for (int i = 0; i < 3; ++i) {
            float yy = y + 3 + i * 5;
            Ln(g, &pen, x + 2, yy, x + 14, yy);
        }
    }
}

void DrawSidebar(Graphics& g, float W, float H) {
    SolidBrush side(C(kSide));
    g.FillRectangle(&side, RectF(0, 0, 232, H));
    Pen edge(C(0x121A26));
    Ln(g, &edge, 232, 0, 232, H);

    // brand mark
    // Brand mark mirrors the application icon: gradient tile, signal arcs, ICU.
    RectF mark(22, 26, 34, 34);
    LinearGradientBrush mg(RectF(mark.X, mark.Y, mark.Width, mark.Height), C(0x23C9B7),
                           C(0x3F6FEA), LinearGradientModeForwardDiagonal);
    FillRound(g, mark, 10, C(0x101B26));
    GraphicsPath p;
    Round(p, mark, 10);
    g.FillPath(&mg, &p);
    Pen ap(Color(72, 3, 26, 40), 1.2f);
    ap.SetStartCap(LineCapRound);
    ap.SetEndCap(LineCapRound);
    for (int i = 0; i < 2; ++i) {
        float rr = 7.5f + i * 4.5f;
        Arc(g, &ap, mark.X + 17 - rr, mark.Y + 25 - rr, rr * 2, rr * 2, 205, 130);
    }
    Txt(g, L"ICU", F(9, true), Color(255, 255, 255, 255),
        RectF(mark.X, mark.Y + 1, mark.Width, mark.Height - 5), StringAlignmentCenter,
        StringAlignmentCenter);

    Txt(g, L"Internet Connection", F(14.5f, true), C(kText), RectF(64, 24, 164, 18));
    Txt(g, L"UTILITY  \u00b7  CONNECTION DOCTOR", F(8, true), C(0x8093A9),
        RectF(64, 43, 168, 12));

    const wchar_t* labels[4] = {L"Overview", L"Wi-Fi", L"Repairs", L"Activity"};
    int issues = g_app.st.nWarn + g_app.st.nCrit;
    for (int i = 0; i < 4; ++i) {
        RectF r(14, 116 + i * 46, 204, 40);
        bool active = g_app.tab == i;
        bool hover = g_app.hoverKind == HIT_NAV && g_app.hoverIdx == i;
        if (active) {
            FillRound(g, r, 9, C(0x122230));
            StrokeRound(g, r, 9, C(0x1D3A44));
            SolidBrush bar(C(kAccent));
            FillRound(g, RectF(r.X + 1, r.Y + 11, 3, 18), 1.5f, C(kAccent));
        } else if (hover) {
            FillRound(g, r, 9, C(0x0F1926));
        }
        Color ic = active ? C(kAccent) : C(kDim);
        NavIcon(g, i, r.X + 20, r.Y + 11, ic);
        Txt(g, labels[i], F(12.5f, active), active ? C(kText) : C(kDim),
            RectF(r.X + 48, r.Y, 128, r.Height), StringAlignmentNear, StringAlignmentCenter);
        if (i == 2 && issues > 0) {
            RectF badge(r.GetRight() - 40, r.Y + 11, 26, 18);
            FillRound(g, badge, 9, C(0x3A1D1D));
            Txt(g, std::to_wstring(issues), F(10, true), C(0xF87171), badge, StringAlignmentCenter,
                StringAlignmentCenter);
        }
        g_app.hits.push_back({HIT_NAV, i, r});
    }

    // sidebar footer
    float by = H - 216;
    SolidBrush sep(C(0x152031));
    g.FillRectangle(&sep, RectF(14, by, 204, 1));
    std::wstring pc = net::computerName();
    Txt(g, pc, F(11.5f, true), C(0xC2CFDF), RectF(18, by + 16, 190, 18));
    Txt(g, net::osVersion(), F(9.5f), C(kFaint), RectF(18, by + 34, 196, 14));

    RectF adminPill(14, by + 56, 204, 30);
    bool elevHover = !g_app.st.adminMode && g_app.hoverKind == HIT_ELEVATE &&
                     g_app.hoverIdx == 1;
    FillRound(g, adminPill, 8,
              g_app.st.adminMode ? C(0x0F2A26) : elevHover ? C(0x33280F) : C(0x251C10));
    if (!g_app.st.adminMode) {
        StrokeRound(g, adminPill, 8, C(elevHover ? 0x7A6329 : 0x3B2F1A));
        g_app.hits.push_back({HIT_ELEVATE, 1, adminPill});
    }
    SolidBrush dot(g_app.st.adminMode ? C(kAccent) : C(0xFBBF24));
    EllF(g, &dot, adminPill.X + 12, adminPill.Y + 12.5f, 5, 5);
    Txt(g, g_app.st.adminMode ? L"Running as administrator" : L"Standard user session",
        F(10.5f), g_app.st.adminMode ? C(0x7FE8D9) : C(0xE7C07B),
        RectF(adminPill.X + 24, adminPill.Y, g_app.st.adminMode ? 176 : 122, adminPill.Height),
        StringAlignmentNear, StringAlignmentCenter);
    if (!g_app.st.adminMode) {
        Txt(g, L"ELEVATE \u2197", F(7.5f, true), C(elevHover ? 0xFBBF24 : 0x9A7C3C),
            RectF(adminPill.GetRight() - 66, adminPill.Y, 54, adminPill.Height),
            StringAlignmentFar, StringAlignmentCenter);
    }
    Txt(g, L"v1.4  \u00b7  built with Win32", F(9.5f), C(0x4A5A6E), RectF(18, by + 92, 190, 14));

    // Copyright and licence, always visible, doubling as the About entry point.
    RectF aboutLine(14, by + 108, 204, 33);
    bool aboutHover = g_app.hoverKind == HIT_ABOUT;
    Txt(g, L"\u00a9 2026-2027 Charles McDonald", F(9.5f), C(aboutHover ? 0xB6C7D8 : 0x8798AC),
        RectF(18, aboutLine.Y, 152, 15));
    Txt(g, L"ABOUT", F(7.5f, true), C(aboutHover ? 0x7FE8D9 : 0x62788E),
        RectF(aboutLine.GetRight() - 62, aboutLine.Y, 56, 15), StringAlignmentFar,
        StringAlignmentCenter);
    Txt(g, L"BSD 3-Clause \u00b7 Zanphar's WinSuite", F(8.5f), C(0x5D6E84),
        RectF(18, aboutLine.Y + 16, 196, 12));
    g_app.hits.push_back({HIT_ABOUT, 0, aboutLine});
}

// ------------------------------------------------------------------ widgets
void Button(Graphics& g, RectF r, const std::wstring& label, bool primary, bool hover,
            bool enabled, float fontPx = 12) {
    if (primary) {
        if (enabled) {
            SolidBrush shadow(C(0x000000, 90));
            FillRound(g, RectF(r.X, r.Y + 2, r.Width, r.Height), 10, C(0x000000, 80));
            LinearGradientBrush lg(RectF(r.X, r.Y, r.Width, r.Height),
                                   C(hover ? 0x5AEBD8 : 0x3FE0CE), C(hover ? 0x6C9CFF : 0x4F8DFD),
                                   LinearGradientModeHorizontal);
            GraphicsPath p;
            Round(p, r, 10);
            g.FillPath(&lg, &p);
        } else {
            FillRound(g, r, 10, C(0x172532));
        }
        Txt(g, label, F(fontPx, true), enabled ? C(0x04211F) : C(kFaint), r,
            StringAlignmentCenter, StringAlignmentCenter);
    } else {
        FillRound(g, r, 10, hover && enabled ? C(0x18273A) : C(0x121D2B));
        StrokeRound(g, r, 10, hover && enabled ? C(0x31465F) : C(kBorder));
        Txt(g, label, F(fontPx, true), enabled ? C(0xD5E1F0) : C(0x53657A), r,
            StringAlignmentCenter, StringAlignmentCenter);
    }
}

void Chip(Graphics& g, RectF r, const std::wstring& label, bool active, bool hover) {
    FillRound(g, r, 8, active ? C(0x12303A) : hover ? C(0x111C2A) : C(0x0C1521));
    StrokeRound(g, r, 8, active ? C(0x2A6A70) : C(kBorder));
    Txt(g, label, F(11, active), active ? C(kAccent) : C(kDim), r, StringAlignmentCenter,
        StringAlignmentCenter);
}

void Pill(Graphics& g, float x, float y, const std::wstring& text, int rgb) {
    float w = 22 + text.size() * 6.4f;
    RectF r(x, y, w, 20);
    FillRound(g, r, 10, C(rgb, 30));
    StrokeRound(g, r, 10, C(rgb, 80));
    Txt(g, text, F(8.5f, true), C(rgb), r, StringAlignmentCenter, StringAlignmentCenter);
}

void SectionHead(Graphics& g, float x, float y, float w, const std::wstring& title,
                 const std::wstring& right) {
    Txt(g, title, F(12, true), C(0xB9C9DC), RectF(x, y, w - 200, 18));
    if (!right.empty())
        Txt(g, right, F(10.5f), C(kFaint), RectF(x + w - 220, y, 220, 18), StringAlignmentFar);
}

// ------------------------------------------------------------------ hero
void DrawHero(Graphics& g, RectF r) {
    FillRound(g, r, 16, C(kCard));
    StrokeRound(g, r, 16, C(kBorder));
    SolidBrush sheen(Color(10, 255, 255, 255));
    g.FillRectangle(&sheen, RectF(r.X + 1, r.Y + 1, r.Width - 2, 1));

    const AppState& st = g_app.st;
    bool scanning = st.scanning;
    float pct = scanning ? st.progress : st.score / 100.f;
    if (!st.hasResult && !scanning) pct = 0;
    float shown = scanning ? std::max(0.02f, st.progress)
                           : std::max(g_app.dispScore / 100.f, 0.f);

    PointF c(r.X + 100, r.Y + r.Height / 2);
    float rad = 58;
    int ringRgb = scanning ? kAccent : (st.score >= 80 ? 0x34D399 : st.score >= 55 ? 0xFBBF24 : 0xF87171);
    Pen track(C(0x1A2534), 9);
    Ell(g, &track, c.X - rad, c.Y - rad, rad * 2, rad * 2);

    if (shown > 0.001f) {
        for (int i = 3; i >= 1; --i) {
            Pen glow(Color(70 / i, (BYTE)((ringRgb >> 16) & 0xFF), (BYTE)((ringRgb >> 8) & 0xFF),
                          (BYTE)(ringRgb & 0xFF)),
                     9 + i * 5);
            Arc(g, &glow, c.X - rad, c.Y - rad, rad * 2, rad * 2, -90, shown * 359.9f);
        }
        Pen pen(C(ringRgb), 9);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        Arc(g, &pen, c.X - rad, c.Y - rad, rad * 2, rad * 2, -90, shown * 359.9f);
    }

    if (scanning) {
        std::wstring p = std::to_wstring((int)(st.progress * 100)) + L"%";
        Txt(g, p, F(22, true), C(kText), RectF(c.X - rad, c.Y - 16, rad * 2, 26),
            StringAlignmentCenter);
        Txt(g, L"SCANNING", F(7.5f, true), C(kAccent), RectF(c.X - rad, c.Y + 12, rad * 2, 12),
            StringAlignmentCenter);
    } else {
        Txt(g, st.hasResult ? std::to_wstring(st.score) : L"\u2014", F(34, true), C(kText),
            RectF(c.X - rad, c.Y - 24, rad * 2, 38), StringAlignmentCenter);
        Txt(g, L"SCORE", F(7.5f, true), C(kFaint), RectF(c.X - rad, c.Y + 14, rad * 2, 12),
            StringAlignmentCenter);
    }

    float colX = r.X + 196;
    float colW = r.Width - 196 - 24;
    std::wstring headline = st.headline;
    std::wstring sub;
    if (scanning) {
        headline = L"Scanning your connection";
        sub = st.phase;
    } else if (st.fixing) {
        headline = L"Applying repairs";
        sub = st.phase.empty() ? std::wstring(L"Working\u2026") : st.phase;
    } else if (!st.hasResult) {
        headline = L"Ready when you are";
        sub = L"Automated checks across the stack, DNS, routing, Wi-Fi and Windows itself.";
    } else {
        sub = std::to_wstring(st.issues.size()) + L" checks completed  \u00b7  " +
              std::to_wstring(st.nCrit) + L" critical  \u00b7  " + std::to_wstring(st.nWarn) +
              L" warnings  \u00b7  " + st.scannedAt;
    }
    Txt(g, headline, F(21, true), C(kText), RectF(colX, r.Y + 20, colW, 28));
    Txt(g, sub, F(11.5f), C(kDim), RectF(colX, r.Y + 50, colW, 18));

    float tileY = r.Y + 82;
    float gap = 9;
    float tileW = (colW - gap * 4) / 5.f;
    for (int i = 0; i < 5; ++i) {
        RectF t(colX + i * (tileW + gap), tileY, tileW, 60);
        FillRound(g, t, 10, C(0x101B29));
        StrokeRound(g, t, 10, C(0x182433));
        std::wstring label, value, unit;
        int rgb = kDim;
        if (i < (int)st.metrics.size()) {
            label = st.metrics[i].label;
            value = st.metrics[i].value;
            unit = st.metrics[i].unit;
            rgb = st.metrics[i].sev == Sev::Pass
                      ? 0x34D399
                      : st.metrics[i].sev == Sev::Warn
                            ? 0xFBBF24
                            : st.metrics[i].sev == Sev::Critical ? 0xF87171 : kDim;
        } else {
            const wchar_t* labels[5] = {L"LATENCY", L"JITTER", L"LOSS", L"DNS", L"DOWN"};
            const wchar_t* units[5] = {L"ms", L"ms", L"%", L"ms", L"Mbps"};
            label = labels[i];
            unit = units[i];
            value = L"\u2014";
        }
        Txt(g, label, F(7.5f, true), C(kFaint), RectF(t.X + 11, t.Y + 9, t.Width - 16, 12));
        Txt(g, value, F(17, true), C(rgb), RectF(t.X + 11, t.Y + 23, t.Width - 30, 24));
        Txt(g, unit, F(8.5f), C(kFaint),
            RectF(t.GetRight() - 40, t.GetBottom() - 19, 30, 12), StringAlignmentFar);
    }

    // latency sparkline
    RectF sp(colX, r.Y + 150, colW, 22);
    SolidBrush base(C(0x1C2939));
    g.FillRectangle(&base, RectF(sp.X, sp.Y + sp.Height - 1, sp.Width, 1));
    const auto& samples = st.spark;
    if (samples.size() >= 2) {
        double mx = 0;
        for (double v : samples) mx = std::max(mx, v);
        mx = std::max(mx * 1.45, 18.0);
        std::vector<PointF> pts;
        float step = sp.Width / (float)(samples.size() - 1);
        for (size_t i = 0; i < samples.size(); ++i) {
            float y = sp.Y + sp.Height - (float)(samples[i] / mx) * (sp.Height - 3);
            pts.push_back(PointF(sp.X + i * step, y));
        }
        GraphicsPath area;
        area.AddLine(pts.front().X, sp.Y + sp.Height, pts.front().X, pts.front().Y);
        for (size_t i = 1; i < pts.size(); ++i) area.AddLine(pts[i - 1], pts[i]);
        area.AddLine(pts.back().X, pts.back().Y, pts.back().X, sp.Y + sp.Height);
        area.CloseFigure();
        LinearGradientBrush ag(RectF(sp.X, sp.Y, sp.Width, sp.Height), Color(70, 45, 212, 191),
                               Color(0, 45, 212, 191), LinearGradientModeVertical);
        g.FillPath(&ag, &area);
        Pen line(C(kAccent), 1.9f);
        g.DrawLines(&line, pts.data(), (INT)pts.size());
        for (size_t i = 0; i < samples.size(); ++i) {
            if (samples[i] <= 0.001) {
                SolidBrush d(C(0xF87171));
                EllF(g, &d, pts[i].X - 2, pts[i].Y - 2, 4, 4);
            }
        }
    } else {
        Txt(g, scanning ? L"Collecting latency samples\u2026" : L"No latency samples yet",
            F(9.5f), C(0x4E5F74), RectF(sp.X, sp.Y, sp.Width, sp.Height), StringAlignmentNear,
            StringAlignmentCenter);
    }
}

// ------------------------------------------------------------------ rows
float ExpandedHeight(Graphics& g, const Issue& i, float w) {
    float h = 10;
    if (!i.advice.empty()) h += TextH(g, i.advice, F(11.5f), w);
    if (!i.steps.empty()) {
        h += 6;
        for (auto& s : i.steps) h += TextH(g, s, F(11), w - 30) + 3;
    }
    if (!i.fixId.empty()) h += 44;
    return h + 8;
}

void DrawIssue(Graphics& g, const Issue& is, RectF r, bool expanded, bool hover) {
    int rgb = sevColor(is.sev);
    FillRound(g, r, 11, hover || expanded ? C(kCardHi) : C(kCard));
    StrokeRound(g, r, 11, expanded ? C(0x2C4655) : hover ? C(kBorderHi) : C(kBorder));

    PointF dc(r.X + 26, r.Y + 27);
    SolidBrush halo(C(rgb, 34));
    EllF(g, &halo, dc.X - 10, dc.Y - 10, 20, 20);
    SolidBrush dot(C(rgb));
    EllF(g, &dot, dc.X - 4, dc.Y - 4, 8, 8);

    Txt(g, is.title, F(12.5f, true), C(kText), RectF(r.X + 46, r.Y + 9, r.Width - 210, 20));
    Txt(g, is.detail, F(11), C(kDim), RectF(r.X + 46, r.Y + 29, r.Width - 200, 18));

    if (!is.metric.empty()) {
        Txt(g, is.metric, F(11.5f, true), C(rgb), RectF(r.GetRight() - 194, r.Y + 12, 140, 18),
            StringAlignmentFar);
        Txt(g, sevLabel(is.sev), F(7.5f, true), C(rgb, 170),
            RectF(r.GetRight() - 194, r.Y + 31, 140, 12), StringAlignmentFar);
    }

    // chevron
    Pen ch(C(0x64768C), 1.6f);
    ch.SetStartCap(LineCapRound);
    ch.SetEndCap(LineCapRound);
    float cx = r.GetRight() - 24, cy = r.Y + 29;
    if (expanded) {
        Ln(g, &ch, cx - 4, cy + 2, cx, cy - 3);
        Ln(g, &ch, cx, cy - 3, cx + 4, cy + 2);
    } else {
        Ln(g, &ch, cx - 4, cy - 3, cx, cy + 2);
        Ln(g, &ch, cx, cy + 2, cx + 4, cy - 3);
    }
}

void DrawIssueExpanded(Graphics& g, const Issue& is, RectF r) {
    float x = r.X + 46;
    float w = r.Width - 46 - 26;
    float y = r.Y + 56;
    SolidBrush line(C(0x1B2A3B));
    g.FillRectangle(&line, RectF(x, y, w, 1));
    y += 12;

    if (!is.advice.empty()) {
        float h = TextH(g, is.advice, F(11.5f), w);
        Txt(g, is.advice, F(11.5f), C(0xAFC0D4), RectF(x, y, w, h), StringAlignmentNear,
            StringAlignmentNear, true);
        y += h + 6;
    }
    for (auto& s : is.steps) {
        SolidBrush b(C(0x3F5771));
        EllF(g, &b, x + 2, y + 6, 3.5f, 3.5f);
        float h = TextH(g, s, F(11), w - 32);
        Txt(g, s, F(11), C(0x8FA2B8), RectF(x + 14, y, w - 32, h), StringAlignmentNear,
            StringAlignmentNear, true);
        y += h + 3;
    }

    if (!is.fixId.empty()) {
        RectF btn(r.GetRight() - 24 - 168, r.GetBottom() - 46, 168, 32);
        Button(g, btn, is.fixLabel, true, g_app.hoverKind == HIT_ROWFIX, true, 10.5f);
        g_app.hits.push_back({HIT_ROWFIX, 0, btn});
        std::wstring note = is.admin ? L"Requires administrator" : L"No elevation needed";
        if (is.risky) note += L"  \u00b7  changes settings";
        if (g_app.st.adminMode && is.admin) note = L"Elevated session  \u00b7  " + note;
        Txt(g, note, F(9.5f), C(kFaint), RectF(x, r.GetBottom() - 40, w - 190, 16));
    } else {
        std::wstring note = is.steps.empty()
                                ? L"Nothing to repair \u2014 this check needs no action."
                                : L"Manual remediation only \u2014 follow the steps above.";
        Txt(g, note, F(9.5f), C(kFaint), RectF(x, r.GetBottom() - 32, w, 16));
    }
}

// ------------------------------------------------------------------ overview
void DrawOverview(Graphics& g, float W, float H, float contentY, float contentBottom) {
    const AppState& st = g_app.st;
    RectF hero(232 + 26, contentY, W - 232 - 52, 188);
    DrawHero(g, hero);

    if (st.issues.empty() && !st.scanning) {
        RectF empty(hero.X, hero.GetBottom() + 20, hero.Width, 190);
        FillRound(g, empty, 14, C(0x0C1421));
        StrokeRound(g, empty, 14, C(kBorder));
        Glow(g, empty.X + empty.Width / 2, empty.Y + 70, 130, kAccent, 26);
        Txt(g, L"No diagnostics yet", F(16, true), C(kText),
            RectF(empty.X, empty.Y + 52, empty.Width, 24), StringAlignmentCenter);
        Txt(g, L"Run a full scan to measure latency, loss, DNS, routing, Wi-Fi and Windows settings.",
            F(11.5f), C(kDim), RectF(empty.X + 60, empty.Y + 80, empty.Width - 120, 34),
            StringAlignmentCenter, StringAlignmentNear, true);
        RectF cta(empty.X + empty.Width / 2 - 90, empty.Y + 126, 180, 38);
        Button(g, cta, L"Run full scan", true, g_app.hoverKind == HIT_SCAN, true, 11.5f);
        g_app.hits.push_back({HIT_SCAN, 0, cta});
        return;
    }

    float filterY = hero.GetBottom() + 16;
    int issueCount = st.nWarn + st.nCrit;
    struct {
        const wchar_t* label;
        int count;
        float w;
    } chips[3] = {{L"All checks", (int)st.issues.size(), 104},
                  {L"Issues", issueCount, 88},
                  {L"Passed", st.nPass, 92}};
    float cx = hero.X;
    for (int i = 0; i < 3; ++i) {
        RectF r(cx, filterY, chips[i].w, 28);
        std::wstring label = std::wstring(chips[i].label) + L"  " + std::to_wstring(chips[i].count);
        Chip(g, r, label, g_app.filter == i, g_app.hoverKind == HIT_CHIP && g_app.hoverIdx == i);
        g_app.hits.push_back({HIT_CHIP, i, r});
        cx += chips[i].w + 8;
    }
    Txt(g, st.hasResult ? L"Last scan " + st.scannedAt : L"Scanning\u2026", F(10.5f), C(kFaint),
        RectF(hero.GetRight() - 240, filterY + 5, 240, 18), StringAlignmentFar);

    // filtered list
    std::vector<int> vis;
    for (int i = 0; i < (int)st.issues.size(); ++i) {
        Sev s = st.issues[i].sev;
        if (g_app.filter == 1 && !(s == Sev::Warn || s == Sev::Critical)) continue;
        if (g_app.filter == 2 && s != Sev::Pass) continue;
        vis.push_back(i);
    }

    RectF list(hero.X, filterY + 40, hero.Width, contentBottom - (filterY + 40));
    float y = list.Y - g_app.scroll[0];
    g_app.rows.clear();
    g.SetClip(list);
    float rowW = list.Width - (vis.size() * 66 > list.Height ? 8 : 0);
    for (int idx : vis) {
        const Issue& is = st.issues[idx];
        bool expanded = g_app.expanded == idx;
        float h = 56;
        if (expanded) h += ExpandedHeight(g, is, rowW - 60);
        RectF r(list.X, y, rowW, h);
        if (r.GetBottom() > list.Y - 4 && r.Y < list.GetBottom() + 4) {
            bool hover = g_app.hoverKind == HIT_ROW && g_app.hoverIdx == idx;
            DrawIssue(g, is, RectF(r.X, r.Y, r.Width, expanded ? h : 56), expanded, hover);
            if (expanded) DrawIssueExpanded(g, is, r);
            g_app.rows.push_back({idx, r, h});
            g_app.hits.push_back({HIT_ROW, idx, RectF(r.X, r.Y, r.Width, expanded ? h : 56)});
        }
        y += h + 8;
    }
    g.ResetClip();
    g_app.contentH = (y + g_app.scroll[0]) - list.Y;

    float maxScroll = std::max(0.f, g_app.contentH - list.Height);
    if (g_app.scroll[0] > maxScroll) g_app.scroll[0] = maxScroll;

    if (g_app.contentH > list.Height) {
        float trackH = list.Height;
        float thumbH = std::max(36.f, trackH * list.Height / g_app.contentH);
        float t = g_app.scroll[0] / std::max(1.f, g_app.contentH - list.Height);
        RectF thumb(list.GetRight() - 5, list.Y + t * (trackH - thumbH), 3, thumbH);
        FillRound(g, thumb, 1.5f, C(0x2B3C50));
    }
}

// ------------------------------------------------------------------ wifi
void DrawWifi(Graphics& g, float W, float H, float top, float bottom) {
    const AppState& st = g_app.st;
    const net::Wifi& w = st.wifi;
    float leftX = 232 + 26, leftW = 400;
    float rightX = leftX + leftW + 16, rightW = W - 26 - rightX;
    float panelH = bottom - top;

    RectF A(leftX, top, leftW, panelH);
    FillRound(g, A, 16, C(kCard));
    StrokeRound(g, A, 16, C(kBorder));
    Txt(g, L"CURRENT NETWORK", F(8.5f, true), C(kFaint), RectF(A.X + 22, A.Y + 20, 300, 14));

    if (!w.supported) {
        Txt(g, L"No wireless adapter", F(17, true), C(kText), RectF(A.X + 22, A.Y + 48, A.Width - 44, 24));
        Txt(g, L"This machine has no Wi-Fi hardware, or the wireless service is unavailable. "
                L"Ethernet diagnostics above are unaffected.",
            F(11.5f), C(kDim), RectF(A.X + 22, A.Y + 82, A.Width - 44, 60), StringAlignmentNear,
            StringAlignmentNear, true);
        } else if (!w.connected) {
        Txt(g, L"Not connected", F(17, true), C(kText), RectF(A.X + 22, A.Y + 48, A.Width - 44, 24));
        Txt(g, w.radioOn ? L"The radio is on, but no wireless network is associated. Traffic is using "
                           L"another adapter if one is present."
                         : L"The wireless radio is switched off (airplane mode or hardware switch).",
            F(11.5f), C(kDim), RectF(A.X + 22, A.Y + 80, A.Width - 44, 60), StringAlignmentNear,
            StringAlignmentNear, true);

        float y = A.Y + 148;
        std::wstring strongest = L"\u2014";
        if (!st.neighbors.empty()) {
            strongest = st.neighbors[0].ssid + L"  \u00b7  " +
                        (st.neighbors[0].band5 ? L"5 GHz" : st.neighbors[0].band6 ? L"6 GHz" : L"2.4 GHz");
        }
        struct KV2 {
            const wchar_t* k;
            std::wstring v;
        } rows2[3] = {{L"Radio", w.radioOn ? L"On" : L"Off"},
                      {L"Networks in range", std::to_wstring(st.neighbors.size())},
                      {L"Strongest", strongest}};
        for (auto& kv : rows2) {
            Txt(g, kv.k, F(10), C(kFaint), RectF(A.X + 22, y, 160, 16));
            Txt(g, kv.v, F(11, true), C(0xC8D6E8), RectF(A.X + 22, y + 16, A.Width - 48, 18));
            y += 40;
        }

        SolidBrush hintBar(C(0x14232F));
        GraphicsPath hp;
        Round(hp, RectF(A.X + 20, A.GetBottom() - 96, A.Width - 40, 70), 10);
        g.FillPath(&hintBar, &hp);
        Txt(g, L"Join a network from the Windows network flyout, or keep using Ethernet. "
                L"Wireless checks run automatically once associated.",
            F(10.5f), C(0x93A7BE), RectF(A.X + 34, A.GetBottom() - 84, A.Width - 68, 48),
            StringAlignmentNear, StringAlignmentNear, true);
    } else {
        Txt(g, w.ssid, F(20, true), C(kText), RectF(A.X + 22, A.Y + 44, A.Width - 44, 28));
        Txt(g, w.phy + L"  \u00b7  " + w.auth, F(11), C(kDim),
            RectF(A.X + 22, A.Y + 74, A.Width - 44, 18));

        int rgb = w.signalPct >= 65 ? 0x34D399 : w.signalPct >= 40 ? 0xFBBF24 : 0xF87171;
        Glow(g, A.X + 82, A.Y + 140, 70, rgb, 24);
        Txt(g, std::to_wstring(w.signalPct) + L"%", F(30, true), C(rgb),
            RectF(A.X + 30, A.Y + 112, 110, 40));
        Txt(g, L"SIGNAL", F(8, true), C(kFaint), RectF(A.X + 32, A.Y + 150, 110, 12));
        Txt(g, std::to_wstring(w.rssi) + L" dBm", F(11), C(kDim),
            RectF(A.X + 30, A.Y + 164, 110, 18));
        Bars(g, A.X + 150, A.Y + 118, w.signalPct, C(rgb));

        float y = A.Y + 106;
        float vx = A.X + 196;
        struct KV {
            const wchar_t* k;
            std::wstring v;
        };
        std::wstring band = w.band6 ? L"6 GHz" : w.band5 ? L"5 GHz" : L"2.4 GHz";
        std::wstring rate = net::fmt0(w.rxKbps / 1000.0) + L" / " + net::fmt0(w.txKbps / 1000.0) +
                            L" Mbps";
        KV rows[4] = {{L"Band", band + L"  \u00b7  ch " + std::to_wstring(w.channel)},
                      {L"Link rate", rate},
                      {L"Security", w.cipher},
                      {L"BSSID", w.bssid}};
        for (auto& kv : rows) {
            Txt(g, kv.k, F(10), C(kFaint), RectF(vx, y, 100, 16));
            Txt(g, kv.v, F(10.5f, true), C(0xCFDCEC), RectF(vx, y + 15, A.Width - (vx - A.X) - 20, 16));
            y += 36;
        }

        float sy = A.Y + 276;
        SolidBrush div(C(0x182433));
        g.FillRectangle(&div, RectF(A.X + 22, sy, A.Width - 44, 1));
        Txt(g, L"PROFILE", F(8, true), C(kFaint), RectF(A.X + 22, sy + 12, 90, 12));
        Txt(g, w.profile.empty() ? L"\u2014" : w.profile, F(11, true), C(0xC3D2E4),
            RectF(A.X + 110, sy + 8, A.Width - 140, 18));
    }

    RectF B(rightX, top, rightW, panelH);
    FillRound(g, B, 16, C(kCard));
    StrokeRound(g, B, 16, C(kBorder));
    Txt(g, L"NEARBY NETWORKS", F(8.5f, true), C(kFaint), RectF(B.X + 22, B.Y + 20, 240, 14));
    Txt(g, std::to_wstring(st.neighbors.size()) + L" visible", F(9.5f), C(kFaint),
        RectF(B.GetRight() - 180, B.Y + 19, 158, 14), StringAlignmentFar);

    float y = B.Y + 46;
    int shown = 0;
    for (auto& n : st.neighbors) {
        if (shown >= 6) break;
        if (n.ssid.empty() || n.ssid == w.ssid) continue;
        RectF row(B.X + 16, y, B.Width - 32, 38);
        FillRound(g, row, 8, C(0x101B29));
        int rgb = n.rssi > -60 ? 0x34D399 : n.rssi > -72 ? 0xFBBF24 : 0x8698AE;
        Bars(g, row.X + 12, row.Y + 11, std::max(0, std::min(100, 2 * (n.rssi + 100))), C(rgb));
        Txt(g, n.ssid, F(11, true), C(0xD5E2F0), RectF(row.X + 44, row.Y + 4, row.Width - 170, 16));
        Txt(g, std::to_wstring(n.rssi) + L" dBm", F(9), C(kFaint),
            RectF(row.X + 44, row.Y + 20, 120, 12));
        std::wstring meta = (n.band5 ? L"5G" : n.band6 ? L"6G" : L"2.4G");
        meta += L"  \u00b7  ch " + std::to_wstring(n.channel);
        Txt(g, meta, F(9.5f, true), C(0x93A6BC), RectF(row.GetRight() - 122, row.Y, 110, row.Height),
            StringAlignmentFar, StringAlignmentCenter);
        y += 44;
        shown++;
    }
    if (shown == 0) {
        Txt(g, L"No other networks detected in this scan.", F(11), C(kFaint),
            RectF(B.X + 22, B.Y + 50, B.Width - 44, 20));
    }

    // channel occupancy
    float cy = std::min(bottom - 118, y + 10);
    if (cy > B.Y + 60) {
        Txt(g, L"2.4 GHz CHANNEL OCCUPANCY", F(8, true), C(kFaint),
            RectF(B.X + 22, cy, 300, 14));
        int counts[11] = {};
        for (auto& n : st.neighbors) {
            if (n.band5 || n.band6) continue;
            if (n.channel >= 1 && n.channel <= 11) counts[n.channel - 1]++;
        }
        if (w.connected && !w.band5 && !w.band6 && w.channel >= 1 && w.channel <= 11)
            counts[w.channel - 1]++;
        int mx = 1;
        for (int c : counts) mx = std::max(mx, c);
        float bw = (B.Width - 60) / 11.f;
        for (int i = 0; i < 11; ++i) {
            float h = 6 + (counts[i] / (float)mx) * 34;
            RectF bar(B.X + 24 + i * bw, cy + 76 - h, bw - 8, h);
            bool own = w.connected && !w.band5 && !w.band6 && w.channel == i + 1;
            FillRound(g, bar, 3, own ? C(0x2DD4BF, 220) : C(0x2E4866));
            Txt(g, std::to_wstring(i + 1), F(8.5f), own ? C(kAccent) : C(0x556A85),
                RectF(bar.X - 4, cy + 78, bw, 12), StringAlignmentCenter);
        }
    }
}

// ------------------------------------------------------------------ repairs
void DrawRepairs(Graphics& g, float W, float H, float top, float bottom) {
    float x = 232 + 26, w = W - 232 - 52;
    if (g_app.st.fixing && !g_app.st.phase.empty()) {
        Txt(g, g_app.st.phase, F(11), C(0xFBBF24), RectF(x, top - 4, w, 18));
    } else {
        Txt(g, L"Every repair is reversible or documented before it runs. Items marked ADMIN need "
                L"an elevated session.",
            F(11), C(kDim), RectF(x, top - 4, w, 18));
    }

    RectF list(x, top + 22, w, bottom - top - 22);
    const auto& fixes = g_app.st.fixes;
    float y = list.Y - g_app.scroll[2];
    g.SetClip(list);
    for (int i = 0; i < (int)fixes.size(); ++i) {
        const FixDef& f = fixes[i];
        RectF r(list.X, y, list.Width - 6, 84);
        if (r.GetBottom() > list.Y - 4 && r.Y < list.GetBottom() + 4) {
            bool hover = g_app.hoverKind == HIT_FIXAPPLY && g_app.hoverIdx == i;
            FillRound(g, r, 11, hover ? C(kCardHi) : C(kCard));
            StrokeRound(g, r, 11, hover ? C(kBorderHi) : C(kBorder));
            Txt(g, f.name, F(12.5f, true), C(kText), RectF(r.X + 20, r.Y + 14, r.Width - 180, 18));
            Txt(g, f.desc, F(10.8f), C(kDim), RectF(r.X + 20, r.Y + 35, r.Width - 190, 32),
                StringAlignmentNear, StringAlignmentNear, true);
            float bx = r.X + 20;
            if (f.admin) {
                Pill(g, bx, r.Y + 60, L"ADMIN", 0xFBBF24);
                bx += 22 + 5 * 6.4f + 6;
            }
            if (f.reboot) {
                Pill(g, bx, r.Y + 60, L"RESTART", 0x60A5FA);
                bx += 22 + 7 * 6.4f + 6;
            }
            if (f.risky) Pill(g, bx, r.Y + 60, L"CHANGES", 0xF87171);

            RectF btn(r.GetRight() - 122, r.Y + 24, 96, 34);
            Button(g, btn, L"Apply", false, hover, true, 11);
            g_app.hits.push_back({HIT_FIXAPPLY, i, btn});
            g_app.hits.push_back({HIT_NONE, -1, r});
        }
        y += 92;
    }
    g.ResetClip();
    g_app.contentH = (y + g_app.scroll[2]) - list.Y;
    float maxScroll = std::max(0.f, g_app.contentH - list.Height);
    if (g_app.scroll[2] > maxScroll) g_app.scroll[2] = maxScroll;
}

// ------------------------------------------------------------------ activity
void DrawActivity(Graphics& g, float W, float H, float top, float bottom) {
    float x = 232 + 26, w = W - 232 - 52;
    RectF list(x, top + 4, w, bottom - top - 8);
    FillRound(g, list, 14, C(0x0C1421));
    StrokeRound(g, list, 14, C(kBorder));
    g.SetClip(RectF(list.X + 1, list.Y + 1, list.Width - 2, list.Height - 2));

    const auto& logs = g_app.st.logs;
    float lineH = 26;
    float contentH = logs.size() * lineH + 28;
    float off = std::max(0.f, contentH - list.Height);
    float y = list.Y + 14 - off;
    for (auto& l : logs) {
        if (y + lineH > list.Y && y < list.GetBottom()) {
            int rgb = l.level == 3 ? 0xF87171 : l.level == 2 ? 0xFBBF24 : l.level == 1 ? 0x34D399 : 0x647A93;
            SolidBrush d(C(rgb));
            EllF(g, &d, list.X + 24, y + 9, 5, 5);
            Txt(g, l.time, FM(10), C(0x55687F), RectF(list.X + 38, y, 60, lineH), StringAlignmentNear,
                StringAlignmentCenter);
            Txt(g, l.text, F(11.2f), C(l.level == 0 ? 0xB6C6D8 : 0xE0EAF6),
                RectF(list.X + 102, y, list.Width - 126, lineH), StringAlignmentNear,
                StringAlignmentCenter);
        }
        y += lineH;
    }
    g.ResetClip();
    if (logs.empty())
        Txt(g, L"No activity recorded yet.", F(11), C(kFaint), list, StringAlignmentCenter,
            StringAlignmentCenter);
}

// ------------------------------------------------------------------ footer
void DrawFooter(Graphics& g, float W, float H) {
    float fy = H - 76;
    SolidBrush bg(Color(210, 9, 15, 24));
    g.FillRectangle(&bg, RectF(0, fy, W, 76));
    SolidBrush line(C(0x162232));
    g.FillRectangle(&line, RectF(232, fy, W - 232, 1));

    if (g_app.st.scanning || g_app.st.fixing) {
        float pw = (W - 232) * std::max(0.02f, g_app.st.progress);
        LinearGradientBrush pg(RectF(232, fy, std::max(1.f, pw), 2), C(kAccent), C(kBlue),
                               LinearGradientModeHorizontal);
        g.FillRectangle(&pg, RectF(232, fy, std::max(1.f, pw), 2));
    }

    float bx = 232 + 26;
    if (!g_app.st.adminMode) {
        SolidBrush d(C(0xFBBF24));
        EllF(g, &d, bx, fy + 15, 6, 6);
        Txt(g, L"Some repairs change system settings and need administrator rights.",
            F(10.5f), C(0xB9A379), RectF(bx + 14, fy + 8, 430, 16));
        RectF eb(bx, fy + 30, 208, 34);
        bool hov = g_app.hoverKind == HIT_ELEVATE && g_app.hoverIdx == 0;
        FillRound(g, eb, 10, hov ? C(0x3B2E12) : C(0x2A2210));
        StrokeRound(g, eb, 10, C(hov ? 0xB18A2E : 0x5A4720));
        // lock glyph
        Pen lp(C(0xFBBF24), 1.6f);
        lp.SetStartCap(LineCapRound);
        lp.SetEndCap(LineCapRound);
        Arc(g, &lp, eb.X + 15, eb.Y + 9, 9, 10, 180, 180);
        FillRound(g, RectF(eb.X + 12, eb.Y + 15, 15, 11), 2.5f, C(0xFBBF24));
        Txt(g, L"Relaunch as administrator", F(10.5f, true), C(0xF5DFA8),
            RectF(eb.X + 34, eb.Y, eb.Width - 42, eb.Height), StringAlignmentNear,
            StringAlignmentCenter);
        g_app.hits.push_back({HIT_ELEVATE, 0, eb});
    } else {
        SolidBrush d(C(kAccent));
        EllF(g, &d, bx, fy + 33, 6, 6);
        Txt(g, L"Administrator session \u2014 every repair is available.", F(10.5f), C(0x8FD9CC),
            RectF(bx + 14, fy + 26, 460, 18));
    }

    bool busy = g_app.st.scanning || g_app.st.fixing;
    float right = W - 26;
    RectF primary(right - 172, fy + 18, 172, 40);
    std::wstring plabel = g_app.st.scanning ? L"Scanning\u2026"
                                            : g_app.st.fixing ? L"Repairing\u2026"
                                                              : g_app.st.hasResult ? L"Run full scan"
                                                                                   : L"Start scan";
    Button(g, primary, plabel, true, g_app.hoverKind == HIT_SCAN, !busy, 11.5f);
    g_app.hits.push_back({HIT_SCAN, 0, primary});

    int fixable = 0;
    for (auto& i : g_app.st.issues)
        if (!i.fixId.empty() && (i.sev == Sev::Warn || i.sev == Sev::Critical)) fixable++;
    RectF fixBtn(primary.X - 156, fy + 18, 146, 40);
    if (fixable > 0) {
        std::wstring label = L"Fix all (" + std::to_wstring(fixable) + L")";
        Button(g, fixBtn, label, false, g_app.hoverKind == HIT_FIXALL, !busy, 11.5f);
        g_app.hits.push_back({HIT_FIXALL, 0, fixBtn});
    }
    RectF copy(fixBtn.X - 118, fy + 18, 108, 40);
    if (g_app.st.hasResult) {
        Button(g, copy, L"Copy report", false, g_app.hoverKind == HIT_COPY, true, 10.5f);
        g_app.hits.push_back({HIT_COPY, 0, copy});
    }
}

// ------------------------------------------------------------------ toast
void DrawToast(Graphics& g, float W, float H) {
    if (g_app.toast.empty()) return;
    if (GetTickCount64() > g_app.toastUntil) {
        g_app.toast.clear();
        return;
    }
    float w = std::min(560.f, 60.f + g_app.toast.size() * 6.6f);
    RectF r(W / 2 - w / 2, H - 136, w, 42);
    FillRound(g, r, 12, C(0x14212F));
    StrokeRound(g, r, 12, C(0x2C5A5E));
    SolidBrush d(C(kAccent));
    EllF(g, &d, r.X + 16, r.Y + 18, 6, 6);
    Txt(g, g_app.toast, F(11), C(0xD8E5F2), RectF(r.X + 30, r.Y, r.Width - 44, r.Height),
        StringAlignmentNear, StringAlignmentCenter);
}

// ------------------------------------------------------------------ render
void Render(App& a) {
    if (!a.bg) return;
    Graphics& g = *a.bg;
    g.ResetTransform();
    g.Clear(C(kBg));
    g.ScaleTransform(a.S, a.S);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

    float W = a.cw / a.S;
    float H = a.ch / a.S;
    a.hits.clear();
    a.hits.push_back({HIT_NONE, -1, RectF(0, 0, W, H)}); // background swallows clicks

    DrawBackdrop(g, W, H);
    DrawSidebar(g, W, H);

    const wchar_t* titles[4] = {L"Connection Overview", L"Wireless analysis", L"Repair toolbox",
                                L"Activity log"};
    const wchar_t* subs[4] = {
        L"Every failure mode that can break a connection, measured in one run.",
        L"Signal, channel congestion and security of the wireless link.",
        L"One-click fixes with explicit risk, elevation and restart requirements.",
        L"Everything this session measured and changed."};
    float cx = 232 + 26;
    float headerY = 22;
    Txt(g, titles[a.tab], F(22, true), C(kText), RectF(cx, headerY, 620, 30));
    Txt(g, subs[a.tab], F(11), C(kDim), RectF(cx, headerY + 32, 640, 18));

    // status pill
    std::wstring status;
    int sRgb = 0x647A93;
    if (a.st.scanning) {
        status = L"SCANNING";
        sRgb = kAccent;
    } else if (a.st.fixing) {
        status = L"REPAIRING";
        sRgb = 0xFBBF24;
    } else if (a.st.hasResult) {
        status = a.st.nCrit ? L"ATTENTION" : a.st.nWarn ? L"WARNINGS" : L"HEALTHY";
        sRgb = a.st.nCrit ? 0xF87171 : a.st.nWarn ? 0xFBBF24 : 0x34D399;
    } else {
        status = L"IDLE";
    }
    float pw = 30 + status.size() * 7.2f;
    RectF pill(W - 26 - pw, headerY + 4, pw, 24);
    FillRound(g, pill, 12, C(sRgb, 24));
    StrokeRound(g, pill, 12, C(sRgb, 90));
    float pulse = a.st.scanning ? (0.55f + 0.45f * (float)sin(a.spin * 2.0)) : 1.f;
    SolidBrush pd(C(sRgb, (int)(255 * pulse)));
    EllF(g, &pd, pill.X + 12, pill.Y + 9.5f, 5, 5);
    Txt(g, status, F(8.5f, true), C(sRgb), RectF(pill.X + 22, pill.Y, pill.Width - 26, pill.Height),
        StringAlignmentNear, StringAlignmentCenter);

    float top = 84;
    float bottom = H - 90;
    if (a.tab == 0)
        DrawOverview(g, W, H, top, bottom);
    else if (a.tab == 1)
        DrawWifi(g, W, H, top, bottom);
    else if (a.tab == 2)
        DrawRepairs(g, W, H, top, bottom);
    else
        DrawActivity(g, W, H, top, bottom);

    DrawFooter(g, W, H);
    DrawToast(g, W, H);
}

void Resize(App& a, int w, int h) {
    if (w <= 0 || h <= 0) return;
    delete a.bg;
    a.bg = nullptr;
    delete a.bb;
    a.bb = nullptr;
    a.cw = w;
    a.ch = h;
    a.bb = new Bitmap(w, h, PixelFormat32bppPARGB);
    a.bg = new Graphics(a.bb);
}

// ------------------------------------------------------------------ interaction
void ApplyFixById(const std::string& fixId) {
    const FixDef* def = findFix(g_app.st, fixId);
    std::wstring name = def ? def->name : net::utf82wstr(fixId);
    std::wstring note = def ? def->desc : L"";
    bool risky = def ? def->risky : false;
    bool admin = def ? def->admin : false;
    if (admin && !g_app.st.adminMode) {
        std::wstring ask = L"\u201c" + name + L"\u201d needs administrator rights.\n\n"
                           L"Relaunch Internet Connection Utility as administrator now?\n\n"
                           L"This repair is queued and applied automatically once the elevated "
                           L"instance starts.";
        if (MessageBoxW(g_app.hwnd, ask.c_str(), L"Internet Connection Utility \u2014 administrator rights",
                        MB_ICONINFORMATION | MB_YESNO) == IDYES) {
            if (!net::savePendingRepairs({fixId})) {
                Toast(L"Could not queue the repair for elevation.");
                return;
            }
            if (net::relaunchAsAdmin()) {
                PostMessageW(g_app.hwnd, WM_CLOSE, 0, 0);
            } else {
                net::clearPendingRepairs();
                Toast(L"Elevation was cancelled.");
            }
        }
        return;
    }
    if (risky && !ConfirmFix(name, note + L"\n\nThe connection may drop for a few seconds.")) return;
    g_app.doc.runFix(fixId, g_app.hwnd);
    Toast(L"Applying: " + name);
}

void ShowAbout() {
    std::wstring m;
    m += L"Internet Connection Utility\n";
    m += L"Connection Doctor  \u00b7  Version 1.4\n";
    m += L"Native Win32 / GDI+, no third-party libraries\n\n";
    m += L"\u00a9 2026-2027 Charles McDonald.\n";
    m += L"BSD 3-Clause License.\n\n";
    m += L"Part of Zanphar's WinSuite. See\n";
    m += L"LICENSE.md in the suite root for\n";
    m += L"the full terms.\n\n";
    m += L"Provided \u201cas is\u201d, without warranty of any kind.\n";
    m += L"Network probes go to Cloudflare, Google and\n";
    m += L"Microsoft only to measure reachability and\n";
    m += L"throughput; no telemetry is collected.";
    MessageBoxW(g_app.hwnd, m.c_str(), L"About Internet Connection Utility",
                MB_ICONINFORMATION | MB_OK);
}

void OnClick(App& a, float mx, float my) {
    PointF p(mx, my);
    for (int i = (int)a.hits.size() - 1; i >= 0; --i) {
        const Hit& h = a.hits[i];
        if (h.kind == HIT_NONE && h.idx == -1) continue; // background marker
        if (!h.r.Contains(p)) continue;
        switch (h.kind) {
            case HIT_NAV:
                a.tab = h.idx;
                a.expanded = -1;
                InvalidateRect(a.hwnd, nullptr, FALSE);
                return;
            case HIT_CHIP:
                a.filter = h.idx;
                a.scroll[0] = 0;
                InvalidateRect(a.hwnd, nullptr, FALSE);
                return;
            case HIT_ROW:
                a.expanded = (a.expanded == h.idx) ? -1 : h.idx;
                InvalidateRect(a.hwnd, nullptr, FALSE);
                return;
            case HIT_ROWFIX:
                if (a.expanded >= 0 && a.expanded < (int)a.st.issues.size()) {
                    std::string fid = a.st.issues[a.expanded].fixId;
                    if (!fid.empty()) ApplyFixById(fid);
                }
                return;
            case HIT_FIXAPPLY:
                if (h.idx >= 0 && h.idx < (int)a.st.fixes.size())
                    ApplyFixById(a.st.fixes[h.idx].id);
                return;
            case HIT_SCAN:
                if (!a.st.scanning && !a.st.fixing) {
                    a.expanded = -1;
                    a.doc.startScan(a.hwnd);
                }
                return;
            case HIT_FIXALL:
                if (!a.st.scanning && !a.st.fixing) {
                    int n = 0, adminCount = 0, userCount = 0;
                    for (auto& is : a.st.issues) {
                        if (is.fixId.empty() ||
                            !(is.sev == Sev::Warn || is.sev == Sev::Critical))
                            continue;
                        n++;
                        const FixDef* d = findFix(a.st, is.fixId);
                        if (d && d->admin) adminCount++;
                        else userCount++;
                    }
                    if (n == 0) {
                        Toast(L"No failing check has an automatic repair.");
                        return;
                    }

                    bool skipAdmin = false;
                    if (adminCount > 0 && !a.st.adminMode) {
                        std::wstring ask =
                            std::to_wstring(adminCount) + L" of " + std::to_wstring(n) +
                            L" repairs need administrator rights.\n\n"
                            L"Yes \u2014 relaunch Internet Connection Utility elevated and apply all of them.\n"
                            L"No \u2014 apply only the " + std::to_wstring(userCount) +
                            L" repair(s) available to a standard user.\n"
                            L"Cancel \u2014 do nothing.";
                        int r = MessageBoxW(a.hwnd, ask.c_str(),
                                            L"Internet Connection Utility \u2014 administrator rights",
                                            MB_ICONQUESTION | MB_YESNOCANCEL);
                        if (r == IDCANCEL) return;
                        if (r == IDYES) {
                            // Queue every failing repair so the elevated instance applies
                            // them automatically instead of losing the request on relaunch.
                            std::vector<std::string> queue;
                            for (auto& is : a.st.issues) {
                                if (is.fixId.empty() ||
                                    !(is.sev == Sev::Warn || is.sev == Sev::Critical))
                                    continue;
                                if (std::find(queue.begin(), queue.end(), is.fixId) == queue.end())
                                    queue.push_back(is.fixId);
                            }
                            if (!net::savePendingRepairs(queue)) {
                                Toast(L"Could not queue the repairs for elevation.");
                                return;
                            }
                            if (net::relaunchAsAdmin()) {
                                PostMessageW(a.hwnd, WM_CLOSE, 0, 0);
                            } else {
                                net::clearPendingRepairs();
                                Toast(L"Elevation was cancelled.");
                            }
                            return;
                        }
                        skipAdmin = true;
                    }

                    int applyCount = skipAdmin ? userCount : n;
                    if (applyCount == 0) {
                        Toast(L"These repairs all need administrator rights.");
                        return;
                    }
                    if (MessageBoxW(a.hwnd,
                                    (L"Apply " + std::to_wstring(applyCount) +
                                     L" repairs now?\n\nActions are logged. Repairs marked as "
                                     L"changing settings may briefly interrupt the connection.")
                                        .c_str(),
                                    L"Internet Connection Utility \u2014 fix all", MB_ICONWARNING | MB_YESNO) != IDYES)
                        return;
                    a.doc.fixAll(a.hwnd, skipAdmin);
                    Toast(L"Applying repairs\u2026");
                }
                return;
            case HIT_ELEVATE:
                if (net::relaunchAsAdmin()) PostMessageW(a.hwnd, WM_CLOSE, 0, 0);
                else Toast(L"Elevation was cancelled.");
                return;
            case HIT_COPY:
                CopyReport();
                return;
            case HIT_ABOUT:
                ShowAbout();
                return;
            default:
                return;
        }
    }
}

void OnMove(App& a, float mx, float my) {
    PointF p(mx, my);
    int kind = 0, idx = -1;
    for (int i = (int)a.hits.size() - 1; i >= 0; --i) {
        const Hit& h = a.hits[i];
        if (h.kind == HIT_NONE) continue;
        if (h.r.Contains(p)) {
            kind = h.kind;
            idx = h.idx;
            break;
        }
    }
    if (kind != a.hoverKind || idx != a.hoverIdx) {
        a.hoverKind = kind;
        a.hoverIdx = idx;
        InvalidateRect(a.hwnd, nullptr, FALSE);
    }
    bool hand = kind == HIT_NAV || kind == HIT_CHIP || kind == HIT_ROW || kind == HIT_ROWFIX ||
                kind == HIT_FIXAPPLY || kind == HIT_SCAN || kind == HIT_FIXALL ||
                kind == HIT_ELEVATE || kind == HIT_COPY;
    SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
}

void OnWheel(App& a, int delta) {
    float step = delta > 0 ? -64.f : 64.f;
    int t = a.tab;
    if (t == 3) return;
    a.scroll[t] += step;
    if (a.scroll[t] < 0) a.scroll[t] = 0;
    float viewH = (a.ch / a.S) - 90 - 84;
    float maxS = std::max(0.f, a.contentH - viewH);
    if (a.scroll[t] > maxS) a.scroll[t] = maxS;
    InvalidateRect(a.hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------------ window
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App& a = g_app;
    switch (msg) {
        case WM_CREATE: {
            a.hwnd = hwnd;
            HDC dc = GetDC(hwnd);
            a.S = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f;
            ReleaseDC(hwnd, dc);
            if (a.S < 1.f) a.S = 1.f;
            RECT rc;
            GetClientRect(hwnd, &rc);
            Resize(a, rc.right, rc.bottom);
            SetTimer(hwnd, 1, 33, nullptr);
            SetTimer(hwnd, 2, 600, nullptr); // auto-start first scan
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
            return 0;
        }
        case WM_TIMER:
            if (wp == 2) {
                KillTimer(hwnd, 2);
                if (!a.started) {
                    a.started = true;
                    a.doc.startScan(hwnd);
                }
                return 0;
            }
            if (wp == 1) {
                ULONGLONG now = GetTickCount64();
                if (now - a.lastPoll >= 90) {
                    a.lastPoll = now;
                    bool wasBusy = a.st.scanning || a.st.fixing;
                    a.st = a.doc.state();
                    if (a.st.fixCounter != a.lastFixCounter) {
                        a.lastFixCounter = a.st.fixCounter;
                        Toast(a.st.fixNote);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    if (!a.st.fixing && !a.st.scanning && a.expanded >= (int)a.st.issues.size())
                        a.expanded = -1;
                    if (wasBusy || a.st.scanning || a.st.fixing) InvalidateRect(hwnd, nullptr, FALSE);
                }
                bool animating = a.st.scanning || a.st.fixing ||
                                 std::fabs(a.dispScore - (float)a.st.score) > 0.2f;
                if (a.st.scanning) a.spin += 0.14f;
                float target = (float)a.st.score;
                float diff = target - a.dispScore;
                if (std::fabs(diff) > 0.15f) a.dispScore += diff * 0.14f;
                else a.dispScore = target;
                if (animating) InvalidateRect(hwnd, nullptr, FALSE);
                if (!a.toast.empty() && now > a.toastUntil) {
                    a.toast.clear();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            return 0;
        case kRefreshMsg:
            a.st = a.doc.state();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SIZE:
            Resize(a, LOWORD(lp), HIWORD(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (!a.bb) Resize(a, 1, 1);
            Render(a);
            Graphics screen(dc);
            if (a.bb) screen.DrawImage(a.bb, 0, 0, a.cw, a.ch);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            float mx = (float)GET_X_LPARAM(lp) / a.S;
            float my = (float)GET_Y_LPARAM(lp) / a.S;
            OnMove(a, mx, my);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            float mx = (float)GET_X_LPARAM(lp) / a.S;
            float my = (float)GET_Y_LPARAM(lp) / a.S;
            OnClick(a, mx, my);
            return 0;
        }
        case WM_MOUSEWHEEL:
            OnWheel(a, GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_F5) {
                if (!a.st.scanning && !a.st.fixing) a.doc.startScan(hwnd);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                a.expanded = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = (LONG)(1000 * a.S);
            mmi->ptMinTrackSize.y = (LONG)(640 * a.S);
            return 0;
        }
        case WM_DPICHANGED: {
            RECT* nr = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, nr->left, nr->top, nr->right - nr->left,
                         nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
            a.S = HIWORD(wp) / 96.0f;
            if (a.S < 1.f) a.S = 1.f;
            RECT rc;
            GetClientRect(hwnd, &rc);
            Resize(a, rc.right, rc.bottom);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 2);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

} // namespace

int runApp(HINSTANCE inst, int nCmdShow) {
    // Per-monitor DPI awareness when available.
    auto setCtx = (BOOL(WINAPI*)(HANDLE))GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                        "SetProcessDpiAwarenessContext");
    if (setCtx) setCtx((HANDLE)-4);
    else SetProcessDPIAware();

    GdiplusStartupInput gi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gi, nullptr) != Ok) return 1;

    net::init();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(7, 11, 18));
    wc.lpszClassName = L"InternetConnectionUtilityWindow";
    HICON iconBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON),
                                        GetSystemMetrics(SM_CYSMICON), 0);
    wc.hIcon = iconBig ? iconBig : LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = iconSmall ? iconSmall : LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    HDC screen = GetDC(nullptr);
    float s = GetDeviceCaps(screen, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, screen);
    if (s < 1.f) s = 1.f;

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int wantW = (int)(1180 * s);
    int wantH = (int)(792 * s);
    int maxW = (wa.right - wa.left) - 80;
    int maxH = (wa.bottom - wa.top) - 60;
    if (wantW > maxW) wantW = maxW;
    if (wantH > maxH) wantH = maxH;
    int x = wa.left + ((wa.right - wa.left) - wantW) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - wantH) / 2;

    HWND hwnd = CreateWindowExW(0, L"InternetConnectionUtilityWindow", L"Internet Connection Utility \u2014 Connection Doctor",
                                WS_OVERLAPPEDWINDOW, x, y, wantW, wantH, nullptr, nullptr, inst,
                                nullptr);
    if (!hwnd) {
        GdiplusShutdown(token);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // A repair requested before the UAC relaunch is applied only now that we are elevated.
    if (net::elevated()) {
        std::vector<std::string> pending = net::loadPendingRepairs();
        if (!pending.empty()) {
            net::clearPendingRepairs();
            Toast(L"Resuming " + std::to_wstring(pending.size()) + L" queued repair(s)\u2026");
            g_app.doc.runFixList(pending, hwnd);
        }
    } else {
        net::clearPendingRepairs();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    delete g_app.bg;
    delete g_app.bb;
    GdiplusShutdown(token);
    return 0;
}
