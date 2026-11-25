// Zanphar's WinSuite - page rendering and page-level actions
#include "app.h"
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>

float g_scale = 1.0f;

static int S(int v) { return (int)std::lround((double)v * g_scale); }
static RECT R(int l, int t, int r, int b) {
    RECT x = {l, t, r, b};
    return x;
}

// Defined with the Settings rows further down, used by the Task Manager page above them.
static void TickBox(HDC dc, const RECT& cb, bool ticked);

// Defined with the list filters below, used by the Network page's host field above them.
static void InputField(HDC dc, std::vector<Hit>& hits, const RECT& rr, const std::wstring& text,
                       const wchar_t* placeholder, bool focused, int hoverId, int id,
                       int clearId);

// ------------------------------------------------------------------- state
void CpuMeter::Update() {
    FILETIME i, k, u;
    if (!GetSystemTimes(&i, &k, &u)) return;
    ULONGLONG ni = ((ULONGLONG)i.dwHighDateTime << 32) | i.dwLowDateTime;
    ULONGLONG nk = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime;
    ULONGLONG nu = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
    if (primed) {
        ULONGLONG idleDelta = ni - idle;
        ULONGLONG totalDelta = (nk - kernel) + (nu - user);
        if (totalDelta > 0) {
            double value = 100.0 * (double)(totalDelta - idleDelta) / (double)totalDelta;
            percent = std::clamp(value, 0.0, 100.0);
        }
    }
    idle = ni;
    kernel = nk;
    user = nu;
    primed = true;
}

void LoadSystemPage(AppState& st) {
    st.sys = CollectSystemInfo();
    st.storage = CollectStorageDevices();
    if (!st.cpu.primed) {
        // The meter needs two samples before it can report a delta.
        st.cpu.Update();
        Sleep(120);
    }
    st.cpu.Update();
}

void LoadMemoryPage(AppState& st) {
    st.mem = CollectMemStats();
    st.memProcs = TopMemoryProcesses(12);
}

void LoadBrowsersPage(AppState& st) {
    st.browsers = BuildBrowserCatalog();
    std::vector<std::wstring> running = RunningBrowsers(st.browsers);
    for (auto& browser : st.browsers) {
        browser.running =
            std::find(running.begin(), running.end(), browser.name) != running.end();
        if (browser.running) browser.selected = false;
        if (!browser.installed) browser.selected = false;
    }
    st.browserScanned = false;
    st.scrollBrowsers = 0;
}

void LoadDnsPage(AppState& st) {
    st.dnsAdapters = LoadDnsAdapters();
}

void ToggleBrowserEntry(AppState& st, int index) {
    if (index < 0 || index >= (int)st.browsers.size()) return;
    BrowserEntry& browser = st.browsers[(size_t)index];
    if (!browser.installed || browser.running) return;
    browser.selected = !browser.selected;
}

void LoadStartupPage(AppState& st) {
    st.startup = LoadStartupItems();
    st.startupMessage.clear();
    st.startupMessageIsError = false;
}

void ToggleStartupItem(AppState& st, int index) {
    if (index < 0 || index >= (int)st.startup.size()) return;
    StartupItem& item = st.startup[index];
    bool target = !item.enabled;
    std::wstring error;
    if (SetStartupEnabled(item, target, error)) {
        item.enabled = target;
        st.startupMessage = target ? (L"Turned on: " + item.name) : (L"Turned off: " + item.name);
        st.startupMessageIsError = false;
    } else {
        st.startupMessage = L"Could not change \"" + item.name + L"\": " + error;
        st.startupMessageIsError = true;
    }
}

void LoadDeactivatePage(AppState& st) {
    st.services = LoadServices();
    st.deactivateMessage.clear();
    st.deactivateMessageIsError = false;
}

void ToggleService(AppState& st, int index) {
    if (st.deactivateBusy || index < 0 || index >= (int)st.services.size()) return;
    // Copy it: reloading the list below replaces the vector this reference points into.
    const ServiceEntry entry = st.services[(size_t)index];

    ServiceChange change;
    std::wstring verb;
    if (entry.deactivated) {
        change = ReactivateService(entry);
        verb = L"Reactivated";
    } else {
        change = DeactivateService(entry);
        verb = L"Deactivated";
    }

    st.services = LoadServices();
    if (change.ok) {
        st.deactivateMessage = verb + L" \"" + entry.display + L"\"";
        if (change.stillRunning)
            st.deactivateMessage += L" - Windows would not stop it, so it keeps running until the "
                                    L"next restart";
        st.deactivateMessageIsError = false;
    } else {
        st.deactivateMessage = L"Could not change \"" + entry.display + L"\": " + change.error;
        st.deactivateMessageIsError = true;
    }
}

void ReactivateAllServices(AppState& st) {
    if (st.deactivateBusy) return;
    std::vector<ServiceEntry> targets;
    for (const auto& service : st.services)
        if (service.deactivated) targets.push_back(service);
    if (targets.empty()) return;

    int restored = 0;
    std::wstring firstError;
    for (const auto& service : targets) {
        ServiceChange change = ReactivateService(service);
        if (change.ok)
            ++restored;
        else if (firstError.empty())
            firstError = service.display + L": " + change.error;
    }

    st.services = LoadServices();
    if (restored == (int)targets.size()) {
        wchar_t buf[160];
        swprintf(buf, 160, L"Reactivated %d service%ls.", restored,
                 restored == 1 ? L"" : L"s");
        st.deactivateMessage = buf;
        st.deactivateMessageIsError = false;
    } else {
        wchar_t buf[200];
        swprintf(buf, 200, L"Reactivated %d of %d. ", restored, (int)targets.size());
        st.deactivateMessage = std::wstring(buf) + firstError;
        st.deactivateMessageIsError = true;
    }
}

std::wstring BuildReport(const AppState& st) {
    std::wstring report = SystemReportText(st.sys, st.cpu.percent);
    if (st.storage.empty()) return report;

    report += L"\r\nStorage devices\r\n---------------\r\n";
    for (const auto& d : st.storage) {
        std::wstring line = d.root + (d.label.empty() ? L"" : L" (" + d.label + L")");
        line += L": " + d.typeName;
        if (!d.fileSystem.empty()) line += L", " + d.fileSystem;
        if (!d.bus.empty()) line += L", " + d.bus;
        if (!d.model.empty()) line += L", " + d.model;
        line += L" - ";
        if (d.total)
            line += FormatBytes(d.freeBytes) + L" free of " + FormatBytes(d.total);
        else
            line += d.accessible ? L"capacity unavailable" : L"no media";
        report += line + L"\r\n";
    }
    return report;
}

// ----------------------------------------------------------------- layout
static const int kSidebarW = 214;

static void Card(HDC dc, const RECT& r, bool hovered) {
    FillRound(dc, r, 10.0f * g_scale, hovered ? theme::hover : theme::card);
    StrokeRound(dc, r, 10.0f * g_scale, theme::cardEdge, 1.0f);
}

static int Header(HDC dc, int x, int y, int cw, const wchar_t* title, const wchar_t* subtitle) {
    TextAt(dc, title, x, y, g_fonts.h1, theme::text);
    TextAt(dc, subtitle, x, y + S(42), g_fonts.small, theme::muted);
    return y + S(92);
}

static void IconGlyph(HDC dc, int kind, int x, int y, COLORREF c) {
    // Draws an 18 x 18 geometric mark so no icon font is required.
    switch (kind) {
        case 0:  // dashboard: four tiles
            FillRound(dc, R(x + 1, y + 1, x + 8, y + 8), 2.f * g_scale, c);
            FillRound(dc, R(x + 10, y + 1, x + 17, y + 8), 2.f * g_scale, c);
            FillRound(dc, R(x + 1, y + 10, x + 8, y + 17), 2.f * g_scale, c);
            FillRound(dc, R(x + 10, y + 10, x + 17, y + 17), 2.f * g_scale, c);
            break;
        case 1:  // cleanup: bin
            FillRound(dc, R(x + 4, y + 6, x + 14, y + 17), 2.f * g_scale, c);
            FillRound(dc, R(x + 2, y + 3, x + 16, y + 5), 1.f * g_scale, c);
            FillRound(dc, R(x + 7, y + 1, x + 11, y + 3), 1.f * g_scale, c);
            break;
        case 2: {  // startup: play
            POINT pts[3] = {{x + 5, y + 2}, {x + 16, y + 9}, {x + 5, y + 16}};
            HBRUSH brush = CreateSolidBrush(c);
            HPEN pen = CreatePen(PS_SOLID, 1, c);
            HGDIOBJ ob = SelectObject(dc, brush);
            HGDIOBJ op = SelectObject(dc, pen);
            Polygon(dc, pts, 3);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(brush);
            DeleteObject(pen);
            break;
        }
        case 5:  // memory: two memory sticks
            FillRound(dc, R(x + 1, y + 4, x + 17, y + 9), 2.f * g_scale, c);
            FillRound(dc, R(x + 1, y + 11, x + 17, y + 16), 2.f * g_scale, c);
            FillRound(dc, R(x + 4, y + 8, x + 14, y + 12), 1.f * g_scale, theme::sidebar);
            FillRound(dc, R(x + 4, y + 15, x + 14, y + 19), 1.f * g_scale, theme::sidebar);
            break;
        case 6:  // deactivate: pause bars
            FillRound(dc, R(x + 4, y + 2, x + 8, y + 17), 2.f * g_scale, c);
            FillRound(dc, R(x + 10, y + 2, x + 14, y + 17), 2.f * g_scale, c);
            break;
        case 7: {  // browsers: globe
            FillRound(dc, R(x + 1, y + 1, x + 17, y + 17), 8.f * g_scale, c);
            FillRound(dc, R(x + 2, y + 7, x + 16, y + 11), 0.5f * g_scale, theme::sidebar);
            FillRound(dc, R(x + 8, y + 2, x + 10, y + 16), 0.5f * g_scale, theme::sidebar);
            break;
        }
        case 8: {  // dns: shield
            POINT pts[5] = {{x + 9, y + 1}, {x + 17, y + 5}, {x + 15, y + 14},
                            {x + 9, y + 18}, {x + 3, y + 14}};
            HBRUSH brush = CreateSolidBrush(c);
            HPEN pen = CreatePen(PS_SOLID, 1, c);
            HGDIOBJ ob = SelectObject(dc, brush);
            HGDIOBJ op = SelectObject(dc, pen);
            Polygon(dc, pts, 5);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(brush);
            DeleteObject(pen);
            break;
        }
        case 9: {  // boost: lightning
            POINT pts[6] = {{x + 10, y + 1},  {x + 4, y + 11}, {x + 8, y + 11},
                            {x + 7, y + 18}, {x + 14, y + 8}, {x + 9, y + 8}};
            HBRUSH brush = CreateSolidBrush(c);
            HPEN pen = CreatePen(PS_SOLID, 1, c);
            HGDIOBJ ob = SelectObject(dc, brush);
            HGDIOBJ op = SelectObject(dc, pen);
            Polygon(dc, pts, 6);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(brush);
            DeleteObject(pen);
            break;
        }
        case 4: {  // large files: document with lines
            FillRound(dc, R(x + 3, y + 1, x + 15, y + 17), 2.5f * g_scale, c);
            for (int i = 0; i < 3; ++i) {
                int ly = y + 5 + i * 4;
                FillRound(dc, R(x + 6, ly, x + 12, ly + 1), 0.5f * g_scale, theme::sidebar);
            }
            break;
        }
        case 11: {  // services: cog
            FillCircle(dc, x + 9, y + 9, 6, c);
            FillCircle(dc, x + 9, y + 9, 2, theme::sidebar);
            FillRound(dc, R(x + 7, y, x + 11, y + 3), 1.f * g_scale, c);
            FillRound(dc, R(x + 7, y + 15, x + 11, y + 18), 1.f * g_scale, c);
            FillRound(dc, R(x, y + 7, x + 3, y + 11), 1.f * g_scale, c);
            FillRound(dc, R(x + 15, y + 7, x + 18, y + 11), 1.f * g_scale, c);
            break;
        }
        case 12: {  // Task Manager: bars
            FillRound(dc, R(x + 1, y + 10, x + 5, y + 17), 1.f * g_scale, c);
            FillRound(dc, R(x + 7, y + 5, x + 11, y + 17), 1.f * g_scale, c);
            FillRound(dc, R(x + 13, y + 1, x + 17, y + 17), 1.f * g_scale, c);
            break;
        }
        case 13: {  // internet & networking: linked nodes
            HPEN pen = CreatePen(PS_SOLID, 2, c);
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, x + 9, y + 9, nullptr);
            LineTo(dc, x + 3, y + 3);
            MoveToEx(dc, x + 9, y + 9, nullptr);
            LineTo(dc, x + 15, y + 3);
            MoveToEx(dc, x + 9, y + 9, nullptr);
            LineTo(dc, x + 3, y + 15);
            MoveToEx(dc, x + 9, y + 9, nullptr);
            LineTo(dc, x + 15, y + 15);
            SelectObject(dc, old);
            DeleteObject(pen);
            FillCircle(dc, x + 9, y + 9, 3, c);
            FillCircle(dc, x + 3, y + 3, 2, c);
            FillCircle(dc, x + 15, y + 3, 2, c);
            FillCircle(dc, x + 3, y + 15, 2, c);
            FillCircle(dc, x + 15, y + 15, 2, c);
            break;
        }
        case 14: {  // storage: two stacked drives
            FillRound(dc, R(x + 2, y + 2, x + 16, y + 9), 2.f * g_scale, c);
            FillRound(dc, R(x + 2, y + 10, x + 16, y + 17), 2.f * g_scale, c);
            FillCircle(dc, x + 13, y + 5, 1.5f * g_scale, theme::sidebar);
            FillCircle(dc, x + 13, y + 13, 1.5f * g_scale, theme::sidebar);
            break;
        }
        case 10: {  // settings: three sliders
            for (int i = 0; i < 3; ++i) {
                int ly = y + 3 + i * 6;
                FillRound(dc, R(x + 1, ly, x + 17, ly + 2), 1.f * g_scale, c);
            }
            FillCircle(dc, x + 12, y + 4, 3, c);
            FillCircle(dc, x + 6, y + 10, 3, c);
            FillCircle(dc, x + 11, y + 16, 3, c);
            break;
        }
        default: {  // system: chip
            FillRound(dc, R(x + 4, y + 4, x + 15, y + 15), 2.f * g_scale, c);
            FillRound(dc, R(x + 7, y + 7, x + 12, y + 12), 1.f * g_scale, theme::sidebar);
            for (int i = 0; i < 3; ++i) {
                int off = x + 7 + i * 3;
                FillRound(dc, R(off, y, off + 2, y + 3), 1.f * g_scale, c);
                FillRound(dc, R(off, y + 16, off + 2, y + 18), 1.f * g_scale, c);
            }
            int offs[3] = {y + 7, y + 10, y + 13};
            for (int oy : offs) {
                FillRound(dc, R(x, oy, x + 3, oy + 2), 1.f * g_scale, c);
                FillRound(dc, R(x + 16, oy, x + 18, oy + 2), 1.f * g_scale, c);
            }
            break;
        }
    }
}

static void PaintSidebar(HDC dc, int w, int h, AppState& st, std::vector<Hit>& hits) {
    int sw = S(kSidebarW);
    FillRect(dc, R(0, 0, sw, h), theme::sidebar);
    FillRect(dc, R(sw - 1, 0, sw, h), theme::cardEdge);

    RECT logo = R(S(22), S(24), S(22) + S(40), S(24) + S(40));
    FillRound(dc, logo, 11.0f * g_scale, theme::accent);
    TextIn(dc, L"Z", logo, g_fonts.h1, RGB(9, 26, 18),
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    TextAt(dc, L"ZANPHAR'S", S(74), S(26), g_fonts.tiny, theme::muted);
    TextAt(dc, L"WinSuite", S(74), S(41), g_fonts.h2, theme::text);

    struct NavItem {
        const wchar_t* label;
        int icon;
        Page page;
        int id;
    };
    const NavItem nav[] = {
        {L"Dashboard", 0, Page::Dashboard, NavDashboard},
        {L"Cleanup", 1, Page::Cleanup, NavCleanup},
        {L"Browsers", 7, Page::Browsers, NavBrowsers},
        {L"Memory", 5, Page::Memory, NavMemory},
        {L"DNS", 8, Page::Dns, NavDns},
        {L"Networking", 13, Page::Network, NavNetwork},
        {L"Game Booster", 9, Page::Boost, NavBoost},
        {L"Large files", 4, Page::LargeFiles, NavLargeFiles},
        {L"Startup", 2, Page::Startup, NavStartup},
        {L"Deactivate", 6, Page::Deactivate, NavDeactivate},
        {L"Services", 11, Page::Services, NavServices},
        {L"Task Manager", 12, Page::Tasks, NavTasks},
        {L"System", 3, Page::System, NavSystem},
        {L"Storage", 14, Page::Storage, NavStorage},
        {L"Settings", 10, Page::Settings, NavSettings},
    };

    // Every entry has to fit above the footer. The rows shrink first, down to a point
    // where the labels are still readable; past that the sidebar scrolls, which is what
    // the added pages made necessary.
    constexpr int kNavCount = (int)(sizeof(nav) / sizeof(nav[0]));
    const int footerTop = h - S(150);
    int itemH = S(46);
    const int navRoom = std::max(0, (int)(footerTop - S(112) - S(8)));
    if (navRoom > 0 && (int)kNavCount * (itemH + S(4)) > navRoom)
        itemH = std::max(S(28), navRoom / kNavCount - S(4));
    const int contentH = (int)kNavCount * (itemH + S(4));
    const int maxScroll = std::max(0, contentH - navRoom - S(4));
    st.scrollNav = std::clamp(st.scrollNav, 0, maxScroll);

    int savedNav = SaveDC(dc);
    IntersectClipRect(dc, 0, S(112), sw - S(14), footerTop - S(8));
    int y = S(112) - st.scrollNav;
    for (const auto& item : nav) {
        RECT row = R(S(12), y, sw - S(12), y + itemH);
        bool active = st.page == item.page;
        bool hovered = st.hoverId == item.id;
        if (active) {
            FillRound(dc, row, 9.0f * g_scale, theme::hover);
            FillRound(dc, R(row.left, row.top + S(11), row.left + S(3), row.bottom - S(11)),
                      1.5f * g_scale, theme::accent);
        } else if (hovered) {
            FillRound(dc, row, 9.0f * g_scale, RGB(25, 32, 40));
        }
        COLORREF fg = active ? theme::text : (hovered ? theme::text : theme::muted);
        IconGlyph(dc, item.icon, row.left + S(16), row.top + S(14), active ? theme::accent : fg);
        TextIn(dc, item.label, R(row.left + S(48), row.top, row.right - S(10), row.bottom),
               g_fonts.h3, fg,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        // Only the part of a row that is on screen answers a click, so a half-scrolled
        // entry cannot take presses from the logo above it or the footer below.
        RECT hit = row;
        hit.top = std::max(hit.top, (LONG)S(112));
        hit.bottom = std::min(hit.bottom, (LONG)(footerTop - S(8)));
        if (hit.bottom > hit.top) hits.push_back({hit, item.id, true});
        y += itemH + S(4);
    }
    RestoreDC(dc, savedNav);

    // A slim indicator, drawn only when the list actually scrolls.
    if (maxScroll > 0) {
        int trackTop = S(112), trackBottom = footerTop - S(8);
        int trackH = trackBottom - trackTop;
        int barH = std::max(S(28), navRoom * trackH / std::max(1, contentH));
        int barY = trackTop + (trackH - barH) * st.scrollNav / std::max(1, maxScroll);
        FillRound(dc, R(sw - S(8), barY, sw - S(4), barY + barH), 2.0f * g_scale, theme::active);
    }

    int footerY = footerTop;
    HLine(dc, S(20), sw - S(20), footerY, theme::cardEdge);
    TextAt(dc, L"Version 2.0.0", S(22), footerY + S(16), g_fonts.small, theme::muted);
    TextAt(dc, L"Native Win32 - no telemetry", S(22), footerY + S(38), g_fonts.tiny, theme::faint);
    const wchar_t* modeLabel = st.elevated          ? L"Administrator"
                               : st.promptSuppressed ? L"Standard user - no prompt"
                                                     : L"Standard user";
    TextAt(dc, modeLabel, S(22), footerY + S(60), g_fonts.tiny,
           st.elevated ? theme::accent : theme::faint);

    if (!st.elevated) {
        RECT elevate = R(S(18), footerY + S(84), sw - S(18), footerY + S(114));
        bool hovered = st.hoverId == HitRestartAdmin;
        FillRound(dc, elevate, 7.0f * g_scale, hovered ? theme::hover : theme::card);
        StrokeRound(dc, elevate, 7.0f * g_scale, hovered ? theme::accent : theme::cardEdge, 1.0f);
        std::wstring label = Ellipsize(dc, L"Restart as administrator", g_fonts.small,
                                       (elevate.right - elevate.left) - S(12));
        TextIn(dc, label, elevate, g_fonts.small, hovered ? theme::accent : theme::text,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        hits.push_back({elevate, HitRestartAdmin, true});
    }
}

static void RecordHit(std::vector<Hit>& hits, RECT r, int id, bool enabled = true) {
    hits.push_back({r, id, enabled});
}

// The outcome banner the list pages use: green for a done action, red for a refusal.
static void ColorRefMessage(HDC dc, const RECT& rr, const std::wstring& text, bool error) {
    FillRound(dc, rr, 8.0f * g_scale, error ? RGB(46, 24, 24) : RGB(20, 34, 29));
    StrokeRound(dc, rr, 8.0f * g_scale, error ? RGB(88, 44, 44) : RGB(34, 60, 50), 1.0f);
    std::wstring line = Ellipsize(dc, text, g_fonts.small, (rr.right - rr.left) - S(24));
    TextIn(dc, line, R(rr.left + S(12), rr.top, rr.right - S(12), rr.bottom), g_fonts.small,
           error ? theme::danger : theme::accent, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void StatCard(HDC dc, const RECT& r, const wchar_t* label, const std::wstring& value,
                     const std::wstring& sub, double pct, COLORREF fill) {
    Card(dc, r, false);
    int pad = S(20);
    TextAt(dc, label, r.left + pad, r.top + S(16), g_fonts.tiny, theme::muted);
    TextAt(dc, value, r.left + pad, r.top + S(34), g_fonts.big, theme::text);
    TextAt(dc, sub, r.left + pad, r.top + S(78), g_fonts.small, theme::muted);
    RECT bar = R(r.left + pad, r.bottom - S(18), r.right - pad, r.bottom - S(12));
    Progress(dc, bar, pct, fill);
}

// -------------------------------------------------------------- dashboard
static void PaintDashboard(HDC dc, int cx, int cw, int h, AppState& st, std::vector<Hit>& hits) {
    int y = Header(dc, cx, S(28), cw, L"Dashboard",
                   L"A quick read on this PC, and the clean-up jobs worth doing.");
    (void)y;

    RECT refresh = R(cx + cw - S(96), S(30), cx + cw, S(62));
    Button(dc, refresh, L"Refresh", false, st.hoverId == HitRefresh);
    RecordHit(hits, refresh, HitRefresh);

    const SysInfo& sys = st.sys;
    uint64_t used = sys.ramTotal > sys.ramAvail ? sys.ramTotal - sys.ramAvail : 0;
    double memPct = sys.ramTotal ? 100.0 * (double)used / (double)sys.ramTotal : 0.0;

    const DriveInfo* primary = nullptr;
    for (const auto& d : sys.drives)
        if (d.root == L"C:\\") primary = &d;
    if (!primary && !sys.drives.empty()) primary = &sys.drives.front();
    double diskPct = (primary && primary->total)
                         ? 100.0 * (double)(primary->total - primary->freeBytes) / (double)primary->total
                         : 0.0;

    int gap = S(16);
    int top = S(128);
    int cardH = S(124);
    int cardW = (cw - gap * 2) / 3;
    wchar_t buf[256];

    swprintf(buf, 256, L"%d%%", (int)std::lround(memPct));
    std::wstring memSub = FormatBytes(used) + L" used of " + FormatBytes(sys.ramTotal);
    StatCard(dc, R(cx, top, cx + cardW, top + cardH), L"MEMORY IN USE", buf, memSub, memPct,
             theme::accent);

    std::wstring cpuValue = st.cpu.primed
                                ? std::to_wstring((int)std::lround(st.cpu.percent)) + L"%"
                                : std::wstring(L"-");
    swprintf(buf, 256, L"%u cores / %u threads", sys.physicalCpus, sys.logicalCpus);
    StatCard(dc, R(cx + cardW + gap, top, cx + cardW * 2 + gap, top + cardH), L"CPU LOAD",
             cpuValue, buf, st.cpu.percent, theme::warn);

    if (primary) {
        swprintf(buf, 256, L"%d%%", (int)std::lround(diskPct));
        std::wstring diskSub = FormatBytes(primary->freeBytes) + L" free of " +
                               FormatBytes(primary->total) + L"  (" + primary->root + L")";
        StatCard(dc, R(cx + (cardW + gap) * 2, top, cx + cw, top + cardH), L"DISK IN USE", buf,
                 diskSub, diskPct, RGB(96, 165, 250));
    } else {
        StatCard(dc, R(cx + (cardW + gap) * 2, top, cx + cw, top + cardH), L"DISK IN USE", L"-",
                 L"No fixed drives reported", 0, theme::muted);
    }

    int rowTop = top + cardH + gap;
    int rowH = S(300);
    int leftW = cw * 58 / 100;

    RECT sysCard = R(cx, rowTop, cx + leftW, rowTop + rowH);
    Card(dc, sysCard, false);
    TextAt(dc, L"This PC", sysCard.left + S(22), sysCard.top + S(18), g_fonts.h3, theme::text);

    struct KV {
        const wchar_t* key;
        std::wstring value;
    };
    KV rows[] = {
        {L"Computer", sys.computer.empty() ? L"-" : sys.computer},
        {L"Windows", sys.osName + L" (" + sys.osBuild + L")"},
        {L"Processor", sys.cpuName.empty() ? L"Unknown" : sys.cpuName},
        {L"Memory", FormatBytes(used) + L" used of " + FormatBytes(sys.ramTotal)},
        {L"Signed in as", sys.user.empty() ? L"-" : sys.user},
        {L"Uptime", FormatDuration(sys.uptimeSeconds)},
    };
    int ry = sysCard.top + S(52);
    int labelW = S(104);
    for (const auto& kv : rows) {
        TextAt(dc, kv.key, sysCard.left + S(22), ry + S(2), g_fonts.small, theme::faint);
        RECT vr = R(sysCard.left + S(22) + labelW, ry - S(2), sysCard.right - S(18), ry + S(20));
        TextIn(dc, Ellipsize(dc, kv.value, g_fonts.base, vr.right - vr.left), vr, g_fonts.base,
               theme::text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        ry += S(40);
    }

    RECT driveCard = R(cx + leftW + gap, rowTop, cx + cw, rowTop + rowH);
    Card(dc, driveCard, false);
    TextAt(dc, L"Storage", driveCard.left + S(22), driveCard.top + S(18), g_fonts.h3, theme::text);

    // Every mounted volume, same source as the System page: fixed, removable,
    // optical, network and RAM disks all appear here, not just the fixed ones. The list
    // scrolls when there are more drives than the card can show, so none of them is
    // silently left out.
    int volumes = (int)st.storage.size();
    int listTop = driveCard.top + S(52);
    int listBottom = (int)driveCard.bottom - S(12);
    int storageRowH = S(54);
    int maxRows = std::max(1, (listBottom - listTop) / storageRowH);
    bool volumesScroll = volumes > maxRows;
    int maxScroll = volumesScroll ? (volumes - maxRows) * storageRowH : 0;
    st.scrollDashboard = std::clamp(st.scrollDashboard / storageRowH * storageRowH, 0, maxScroll);
    int shown = volumesScroll ? maxRows : volumes;

    if (volumes > 0) {
        if (volumesScroll)
            swprintf(buf, 256, L"%d volumes  -  scroll for all", volumes);
        else
            swprintf(buf, 256, L"%d volume%ls", volumes, volumes == 1 ? L"" : L"s");
        TextIn(dc, buf,
               R(driveCard.left + S(110), driveCard.top + S(20), driveCard.right - S(22),
                 driveCard.top + S(38)),
               g_fonts.tiny, theme::faint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    int firstRow = st.scrollDashboard / storageRowH;
    int savedRows = SaveDC(dc);
    IntersectClipRect(dc, driveCard.left, listTop, driveCard.right, listBottom);
    for (int i = firstRow; i < std::min(volumes, firstRow + shown); ++i) {
        int dy = listTop + (i - firstRow) * storageRowH;
        if (dy + storageRowH <= listTop || dy >= listBottom) continue;
        const StorageDevice& d = st.storage[(size_t)i];
        double pct = d.total ? 100.0 * (double)(d.total - d.freeBytes) / (double)d.total : 0.0;
        std::wstring label = d.root + (d.label.empty() ? std::wstring() : L"  " + d.label);
        label = Ellipsize(dc, label, g_fonts.h3, (driveCard.right - driveCard.left) / 2);
        TextAt(dc, label, driveCard.left + S(22), dy, g_fonts.h3,
               d.accessible ? theme::text : theme::muted);

        if (d.typeName != L"Fixed") {
            int textW = TextWidth(dc, label, g_fonts.h3);
            int room = (driveCard.right - driveCard.left) - S(190);
            if (textW + S(70) < room)
                Pill(dc, driveCard.left + S(22) + textW + S(10), dy + S(1), d.typeName,
                     theme::active, theme::muted, g_fonts.tiny);
        }

        std::wstring freeText =
            d.total ? FormatBytes(d.freeBytes) + L" free of " + FormatBytes(d.total)
                    : (d.accessible ? std::wstring(L"capacity unavailable")
                                    : std::wstring(L"no media"));
        TextIn(dc, freeText,
               R(driveCard.left + S(22), dy + S(2), driveCard.right - S(22), dy + S(24)),
               g_fonts.small, theme::muted, DT_RIGHT | DT_TOP | DT_SINGLELINE);
        RECT bar = R(driveCard.left + S(22), dy + S(30), driveCard.right - S(22), dy + S(36));
        Progress(dc, bar, pct, pct > 90 ? theme::danger : theme::accent);
    }
    RestoreDC(dc, savedRows);

    if (maxScroll > 0) {
        int viewH = maxRows * storageRowH;
        int barH = std::max(S(26), viewH * shown / std::max(1, volumes));
        int barY = listTop + (viewH - barH) * st.scrollDashboard / std::max(1, maxScroll);
        FillRound(dc, R(driveCard.right - S(10), barY, driveCard.right - S(6), barY + barH),
                  2.0f * g_scale, theme::active);
    }
    if (volumes == 0)
        TextAt(dc, L"No storage volumes found", driveCard.left + S(22), listTop, g_fonts.small,
               theme::muted);

    int actionY = rowTop + rowH + S(18);
    int btnH = S(40);
    RECT scan = R(cx, actionY, cx + S(168), actionY + btnH);
    Button(dc, scan, L"Scan for junk files", true, st.hoverId == HitQuickScan);
    RecordHit(hits, scan, HitQuickScan);

    RECT startupBtn = R(scan.right + S(12), actionY, scan.right + S(12) + S(180), actionY + btnH);
    Button(dc, startupBtn, L"Review startup apps", false, st.hoverId == HitQuickStartup);
    RecordHit(hits, startupBtn, HitQuickStartup);

    TextAt(dc, L"WinSuite reads only what it needs and writes nothing outside your temp folders and "
                L"its own settings key.",
           cx, actionY + btnH + S(20), g_fonts.tiny, theme::faint);
    (void)h;
}

// ---------------------------------------------------------------- cleanup
static void ChipButton(HDC dc, const RECT& r, const std::wstring& label, bool active,
                       bool hovered) {
    FillRound(dc, r, 8.0f * g_scale,
              active ? theme::accentSoft : (hovered ? theme::hover : theme::card));
    StrokeRound(dc, r, 8.0f * g_scale, active ? theme::accent : theme::cardEdge, 1.0f);
    TextIn(dc, label, r, g_fonts.small, active ? theme::accent : theme::muted,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void PaintCleanup(HDC dc, int cx, int cw, int h, AppState& st, std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"Cleanup",
           L"Review what will go before anything is deleted. Temp caches are ticked for you; "
           L"your Downloads folder and the Recycle Bin are not.");

    int by = S(112);
    int btnH = S(38);
    // The same button starts a scan and stops one: while a pass runs it reads Cancel and
    // asks the worker to stop, then says Stopping until the worker has acknowledged.
    RECT scanBtn = R(cx, by, cx + S(126), by + btnH);
    const wchar_t* scanLabel = L"Scan now";
    bool scanPrimary = true;
    bool scanEnabled = true;
    int scanHit = HitScan;
    if (st.cleaning) {
        scanLabel = L"Cleaning...";
        scanEnabled = false;
    } else if (st.scanning) {
        scanLabel = st.scanStopping ? L"Stopping..." : L"Cancel";
        scanPrimary = false;
        scanEnabled = !st.scanStopping;
        scanHit = HitScanCancel;
    }
    Button(dc, scanBtn, scanLabel, scanPrimary, st.hoverId == scanHit, scanEnabled);
    RecordHit(hits, scanBtn, scanHit, scanEnabled);

    uint64_t selBytes = 0, allBytes = 0, selFiles = 0;
    int selCount = 0;
    for (const auto& g : st.groups) {
        allBytes += g.bytes;
        // Unmeasured rows (a stopped pass) are not part of the clean, so they are not
        // part of the total the button acts on either.
        if (g.selected && g.exists && g.scanned) {
            selBytes += g.bytes;
            selFiles += g.files;
            ++selCount;
        }
    }
    bool canClean = st.hasScanned && !st.scanning && !st.cleaning && selCount > 0 && selBytes > 0;

    RECT cleanBtn = R(cx + S(140), by, cx + S(140) + S(170), by + btnH);
    Button(dc, cleanBtn, L"Clean selected", false, st.hoverId == HitClean, canClean);
    TextIn(dc, L"Clean selected", cleanBtn, g_fonts.h3,
           canClean ? theme::danger : theme::faint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RecordHit(hits, cleanBtn, HitClean, canClean);

    wchar_t buf[256];
    std::wstring status;
    COLORREF statusColor = theme::muted;
    if (st.scanning) {
        // The rows behind the list fill in one at a time; this line says how far it is.
        int done = 0;
        for (const auto& g : st.groups)
            if (g.scanned) ++done;
        if (!st.groups.empty()) {
            swprintf(buf, 256, L"Measuring folders... %d of %d", done, (int)st.groups.size());
            status = buf;
        } else {
            status = L"Measuring folders...";
        }
    } else if (st.hasScanned) {
        if (selCount > 0) {
            swprintf(buf, 256, L"%ls selected  -  %ls files in %d folders",
                     FormatBytes(selBytes).c_str(), FormatCount(selFiles).c_str(), selCount);
            status = buf;
            statusColor = theme::text;
        } else {
            swprintf(buf, 256, L"%ls found  -  nothing selected", FormatBytes(allBytes).c_str());
            status = buf;
        }
    } else {
        status = L"Not scanned yet";
    }
    // Bulk ticks sit beside the buttons, matching the quiet links on the other pages.
    auto Link = [&](const RECT& r, const wchar_t* label, int id) {
        bool hover = st.hoverId == id;
        TextIn(dc, label, r, g_fonts.small, hover ? theme::accent : theme::muted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (hover)
            HLine(dc, r.left, r.left + TextWidth(dc, label, g_fonts.small), r.bottom - S(10),
                  theme::accent);
        RecordHit(hits, r, id);
    };
    RECT selectAll = R(cx + S(322), by, cx + S(322) + S(80), by + btnH);
    Link(selectAll, L"Select all", HitCleanupSelectAll);
    RECT clearAll = R(selectAll.right + S(16), by, selectAll.right + S(16) + S(74), by + btnH);
    Link(clearAll, L"Clear all", HitCleanupClearAll);

    // The Order switch sits at the right end of a summary line under the buttons, and the
    // status takes what is left of it, ellipsizing rather than running under the switch
    // when the window is narrow.
    int chipH = S(28);
    int orderY = by + btnH + S(8);
    const wchar_t* const sizeLabel = L"Biggest first";
    const wchar_t* const builtInLabel = L"Built-in order";
    int sizeW = std::max(S(64), TextWidth(dc, sizeLabel, g_fonts.small) + S(28));
    int builtInW = std::max(S(64), TextWidth(dc, builtInLabel, g_fonts.small) + S(28));
    RECT builtInChip = R(cx + cw - builtInW, orderY, cx + cw, orderY + chipH);
    RECT sizeChip =
        R(builtInChip.left - S(6) - sizeW, orderY, builtInChip.left - S(6), orderY + chipH);
    ChipButton(dc, sizeChip, sizeLabel, st.cleanSortBySize, st.hoverId == HitCleanupSortSize);
    RecordHit(hits, sizeChip, HitCleanupSortSize);
    ChipButton(dc, builtInChip, builtInLabel, !st.cleanSortBySize,
               st.hoverId == HitCleanupSortDefault);
    RecordHit(hits, builtInChip, HitCleanupSortDefault);

    RECT sr = R(cx, orderY, sizeChip.left - S(16), orderY + chipH);
    if (sr.left < sr.right)
        TextIn(dc, status, sr, g_fonts.small, statusColor,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    int listTop = orderY + chipH + S(10);
    if (st.showCleanResult) {
        RECT banner = R(cx, listTop - S(8), cx + cw, listTop - S(8) + S(36));
        COLORREF bg = st.cleanMessageIsError ? RGB(58, 26, 26) : theme::accentSoft;
        FillRound(dc, banner, 8.0f * g_scale, bg);
        TextIn(dc, st.cleanMessage,
               R(banner.left + S(14), banner.top, banner.right - S(14), banner.bottom),
               g_fonts.small, st.cleanMessageIsError ? theme::danger : theme::accent,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        listTop += S(40);
    }

    int listBottom = h - S(62);
    int rowH = S(66), gap = S(10);
    int listH = listBottom - listTop;
    int totalH = (int)st.groups.size() * (rowH + gap);
    int maxScroll = std::max(0, totalH - listH - gap);
    if (st.scrollCleanup > maxScroll) st.scrollCleanup = maxScroll;
    if (st.scrollCleanup < 0) st.scrollCleanup = 0;

    if (!st.hasScanned && !st.scanning) {
        int cyy = listTop + listH / 2 - S(40);
        RECT titleR = R(cx, cyy, cx + cw, cyy + S(30));
        TextIn(dc, L"Nothing scanned yet", titleR, g_fonts.h2, theme::text,
               DT_CENTER | DT_TOP | DT_SINGLELINE);
        RECT subR = R(cx, cyy + S(30), cx + cw, cyy + S(56));
        TextIn(dc, L"Press Scan now to measure temporary files and caches on this PC.", subR,
               g_fonts.small, theme::muted, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, cx, listTop, cx + cw, listBottom);
    // Biggest measured folder first, in the order the shell worked out when the last pass
    // landed. A row's hit id is its place in st.groups rather than its place on screen, so
    // a click always toggles the row it was drawn under.
    bool ordered = st.cleanOrder.size() == st.groups.size();
    for (size_t pos = 0; pos < st.groups.size(); ++pos) {
        size_t i = ordered ? (size_t)st.cleanOrder[pos] : pos;
        if (i >= st.groups.size()) continue;
        const CleanGroup& g = st.groups[i];
        int ry = listTop - st.scrollCleanup + (int)pos * (rowH + gap);
        if (ry + rowH < listTop - S(4) || ry > listBottom) continue;
        RECT rr = R(cx, ry, cx + cw, ry + rowH);
        int id = HitCleanupRowBase + (int)i;
        Card(dc, rr, st.hoverId == id && g.exists);

        int checkSize = S(20);
        RECT cb = R(rr.left + S(20), ry + rowH / 2 - checkSize / 2,
                    rr.left + S(20) + checkSize, ry + rowH / 2 + checkSize / 2);
        if (!g.exists) {
            FillRound(dc, cb, 5.0f * g_scale, RGB(20, 26, 33));
            StrokeRound(dc, cb, 5.0f * g_scale, theme::active, 1.0f);
        } else if (g.selected) {
            FillRound(dc, cb, 5.0f * g_scale, theme::accent);
            HPEN pen = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(9, 26, 18));
            HGDIOBJ old = SelectObject(dc, pen);
            int midY = (cb.top + cb.bottom) / 2;
            MoveToEx(dc, cb.left + S(5), midY, nullptr);
            LineTo(dc, cb.left + S(9), midY + S(4));
            LineTo(dc, cb.right - S(5), midY - S(5));
            SelectObject(dc, old);
            DeleteObject(pen);
        } else {
            FillRound(dc, cb, 5.0f * g_scale, RGB(20, 26, 33));
            StrokeRound(dc, cb, 5.0f * g_scale, theme::active, 1.0f);
        }

        int textLeft = rr.left + S(54);
        int rightW = S(150);
        int textRight = rr.right - rightW;
        COLORREF nameColor = g.exists ? theme::text : theme::faint;
        TextIn(dc, g.name, R(textLeft, ry + S(11), textRight, ry + S(32)), g_fonts.h3, nameColor,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        std::wstring detail;
        COLORREF detailColor = theme::faint;
        if (!g.exists) {
            detail = g.error.empty() ? L"Not present on this PC" : g.error;
        } else if (!g.error.empty()) {
            detail = g.error;
            detailColor = theme::warn;
        } else {
            detail = g.path;
        }
        TextIn(dc, Ellipsize(dc, detail, g_fonts.monoSmall, textRight - textLeft),
               R(textLeft, ry + S(35), textRight, ry + S(55)), g_fonts.monoSmall, detailColor,
               DT_LEFT | DT_TOP | DT_SINGLELINE);

        // Row states for the figures, worked out before the pills because a stale figure
        // is marked next to the name.
        bool measuring = st.scanning && !g.scanned;
        // A stopped pass leaves the rows it never reached unmeasured. An earlier figure
        // stays on screen for those - dimmed and marked "earlier" - instead of being
        // thrown away for a dash; only rows with nothing measured yet show "not measured".
        bool unmeasured = !st.scanning && !g.scanned && st.hasScanned && g.exists;
        bool earlier = unmeasured && (g.bytes > 0 || g.files > 0);

        int nameW = TextWidth(dc, g.name, g_fonts.h3);
        int pillX = textLeft + nameW + S(10);
        if (earlier && pillX + S(64) < textRight) {
            Pill(dc, pillX, ry + S(9), L"earlier", RGB(30, 36, 44), theme::muted, g_fonts.tiny);
            pillX += TextWidth(dc, L"earlier", g_fonts.tiny) + S(22);
        }
        if (g.needsAdmin && g.exists && pillX + S(74) < textRight) {
            Pill(dc, pillX, ry + S(9), L"admin", RGB(48, 40, 22), theme::warn, g_fonts.tiny);
            pillX += TextWidth(dc, L"admin", g_fonts.tiny) + S(22);
        }
        if (g.personal && g.exists && pillX + S(84) < textRight) {
            // The Recycle Bin is the user's own data too, but there is no putting it back
            // once it is emptied, so the row says which of the two it is.
            Pill(dc, pillX, ry + S(9), g.recycleBin ? L"permanent" : L"your files",
                 RGB(48, 34, 22), g.selected ? theme::danger : theme::warn, g_fonts.tiny);
        }

        std::wstring sizeText;
        COLORREF sizeColor = theme::faint;
        if (measuring) {
            sizeText = L"Measuring...";
            sizeColor = theme::muted;
        } else if (earlier) {
            sizeText = FormatBytes(g.bytes);
            sizeColor = theme::faint;
        } else if (unmeasured) {
            sizeText = L"-";
            sizeColor = theme::muted;
        } else if (g.exists) {
            sizeText = FormatBytes(g.bytes);
            sizeColor = theme::text;
        } else {
            sizeText = L"-";
        }
        TextIn(dc, sizeText, R(rr.right - S(150), ry + S(13), rr.right - S(20), ry + S(36)),
               g_fonts.h3, sizeColor, DT_RIGHT | DT_TOP | DT_SINGLELINE);

        std::wstring countText;
        COLORREF countColor = theme::muted;
        if (measuring) {
            // nothing: the row is being walked right now
        } else if (earlier) {
            countText = FormatCount(g.files) + (g.recycleBin ? L" items" : L" files");
            countColor = theme::faint;
        } else if (unmeasured) {
            countText = L"not measured";
        } else if (g.exists) {
            countText = FormatCount(g.files) + (g.recycleBin ? L" items" : L" files");
        } else {
            countText = L"skipped";
        }
        TextIn(dc, countText, R(rr.right - S(150), ry + S(37), rr.right - S(20), ry + S(56)),
               g_fonts.small, countColor, DT_RIGHT | DT_TOP | DT_SINGLELINE);

        if (g.exists) {
            // A scrolled row keeps its full card in the list, so its hit is clipped to
            // what is on screen: otherwise the rows above the viewport quietly take the
            // clicks meant for the toolbar and the Order switch over them.
            RECT rowHit = rr;
            rowHit.top = std::max(rowHit.top, (LONG)listTop);
            rowHit.bottom = std::min(rowHit.bottom, (LONG)listBottom);
            RecordHit(hits, rowHit, id);
        }
    }
    RestoreDC(dc, saved);

    // The reset link shares this line, so the sentence is trimmed to the room left of it
    // rather than running underneath it on a narrower window.
    std::wstring cleanFooter =
        L"Only files inside the folders you tick are removed. Downloads is cleared only "
        L"when you tick it; ticking the Recycle Bin empties it for good.";
    TextAt(dc, Ellipsize(dc, cleanFooter, g_fonts.tiny, cw - S(196)), cx, h - S(44),
           g_fonts.tiny, theme::faint);

    // Drops the remembered ticks and nothing else, so it stays a quiet text link: no
    // dialog, no page busy, and the rows keep the figures already measured.
    RECT resetRect = R(cx + cw - S(170), h - S(48), cx + cw, h - S(26));
    bool resetHover = st.hoverId == HitCleanupReset;
    TextIn(dc, L"Reset to defaults", resetRect, g_fonts.tiny,
           resetHover ? theme::accent : theme::muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    if (resetHover) HLine(dc, resetRect.left, resetRect.right, resetRect.bottom - S(4), theme::accent);
    RecordHit(hits, resetRect, HitCleanupReset);
}

// ------------------------------------------------------------- browsers
static int ChipRow(HDC dc, std::vector<Hit>& hits, int x, int y, int idBase,
                   const wchar_t* const* labels, int count, int selected, int chipH) {
    for (int i = 0; i < count; ++i) {
        int w = std::max(S(64), TextWidth(dc, labels[i], g_fonts.small) + S(30));
        RECT chip = R(x, y, x + w, y + chipH);
        ChipButton(dc, chip, labels[i], i == selected, false);
        RecordHit(hits, chip, idBase + i);
        x += w + S(8);
    }
    return x;
}

static void PaintBrowsers(HDC dc, int cx, int cw, int h, AppState& st,
                          std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"Browsers",
           L"History, cookies, autofill, caches and session data - measured before anything goes.");

    int by = S(112);
    int btnH = S(38);
    bool idle = !st.browserScanning && !st.browserCleaning;
    const wchar_t* scanLabel =
        st.browserScanning ? L"Scanning..." : st.browserCleaning ? L"Cleaning..." : L"Scan";
    RECT scan = R(cx, by, cx + S(126), by + btnH);
    Button(dc, scan, scanLabel, true, st.hoverId == HitBrowserScan, idle);
    RecordHit(hits, scan, HitBrowserScan, idle);

    RECT close = R(cx + S(140), by, cx + S(140) + S(176), by + btnH);
    Button(dc, close, L"Close browsers", false, st.hoverId == HitBrowserClose, !st.browserCleaning);
    RecordHit(hits, close, HitBrowserClose, !st.browserCleaning);

    uint64_t selBytes = 0;
    int selCount = 0;
    for (const auto& b : st.browsers) {
        if (b.selected && b.installed && !b.running) {
            selBytes += b.bytes;
            ++selCount;
        }
    }
    bool canClean = st.browserScanned && idle && selCount > 0;
    RECT clean = R(cx + cw - S(184), by, cx + cw, by + btnH);
    Button(dc, clean, L"Clean selected", false, st.hoverId == HitBrowserClean, canClean);
    TextIn(dc, L"Clean selected", clean, g_fonts.h3, canClean ? theme::danger : theme::faint,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RecordHit(hits, clean, HitBrowserClean, canClean);

    wchar_t buf[256];
    std::wstring status;
    if (st.browserScanning) {
        status = L"Measuring browser data...";
    } else if (st.browserScanned) {
        swprintf(buf, 256, L"%ls selected  -  %d of %d browsers", FormatBytes(selBytes).c_str(),
                 selCount, (int)st.browsers.size());
        status = buf;
    } else {
        status = L"Not scanned yet";
    }
    TextIn(dc, status, R(close.right + S(16), by, clean.left - S(16), by + btnH), g_fonts.small,
           st.browserScanned ? theme::text : theme::muted,
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    int listTop = by + S(56);
    if (!st.browserMessage.empty()) {
        RECT msg = R(cx, listTop - S(6), cx + cw, listTop + S(22));
        TextIn(dc, st.browserMessage, msg, g_fonts.small,
               st.browserMessageIsError ? theme::danger : theme::accent,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        listTop += S(32);
    }

    int listBottom = h - S(62);
    int rowH = S(70), gap = S(10);
    int listH = listBottom - listTop;
    int totalH = (int)st.browsers.size() * (rowH + gap);
    int maxScroll = std::max(0, totalH - listH - gap);
    st.scrollBrowsers = std::clamp(st.scrollBrowsers, 0, maxScroll);

    bool anyInstalled = false;
    for (const auto& b : st.browsers)
        if (b.installed) anyInstalled = true;
    if (!anyInstalled) {
        int cyy = listTop + listH / 2 - S(28);
        TextIn(dc, L"No supported browser found", R(cx, cyy, cx + cw, cyy + S(30)), g_fonts.h2,
               theme::text, DT_CENTER | DT_TOP | DT_SINGLELINE);
        TextIn(dc, L"Internet Explorer, Chrome, Edge, Brave, Vivaldi, Opera, Opera GX and Firefox are recognised.",
               R(cx, cyy + S(32), cx + cw, cyy + S(56)), g_fonts.small, theme::muted,
               DT_CENTER | DT_TOP | DT_SINGLELINE);
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, cx, listTop, cx + cw, listBottom);
    for (size_t i = 0; i < st.browsers.size(); ++i) {
        const BrowserEntry& b = st.browsers[i];
        int ry = listTop - st.scrollBrowsers + (int)i * (rowH + gap);
        if (ry + rowH < listTop - S(4) || ry > listBottom) continue;
        int id = HitBrowserRowBase + (int)i;
        bool clickable = b.installed && !b.running;
        RECT rr = R(cx, ry, cx + cw, ry + rowH);
        Card(dc, rr, st.hoverId == id && clickable);

        int checkSize = S(20);
        RECT cb = R(rr.left + S(20), ry + rowH / 2 - checkSize / 2, rr.left + S(20) + checkSize,
                    ry + rowH / 2 + checkSize / 2);
        if (b.selected && clickable) {
            FillRound(dc, cb, 5.0f * g_scale, theme::accent);
            HPEN pen = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(9, 26, 18));
            HGDIOBJ old = SelectObject(dc, pen);
            int midY = (cb.top + cb.bottom) / 2;
            MoveToEx(dc, cb.left + S(5), midY, nullptr);
            LineTo(dc, cb.left + S(9), midY + S(4));
            LineTo(dc, cb.right - S(5), midY - S(5));
            SelectObject(dc, old);
            DeleteObject(pen);
        } else {
            FillRound(dc, cb, 5.0f * g_scale, RGB(20, 26, 33));
            StrokeRound(dc, cb, 5.0f * g_scale, theme::active, 1.0f);
        }

        int textLeft = rr.left + S(54);
        int textRight = rr.right - S(160);
        TextIn(dc, b.name, R(textLeft, ry + S(11), textRight, ry + S(33)), g_fonts.h3,
               b.installed ? theme::text : theme::faint, DT_LEFT | DT_TOP | DT_SINGLELINE);

        int nameW = TextWidth(dc, b.name, g_fonts.h3);
        if (b.installed && b.running)
            Pill(dc, textLeft + nameW + S(10), ry + S(9), L"running", RGB(48, 40, 22), theme::warn,
                 g_fonts.tiny);
        else if (!b.installed)
            Pill(dc, textLeft + nameW + S(10), ry + S(9), L"not installed", theme::active,
                 theme::faint, g_fonts.tiny);

        std::wstring detail = b.installed ? b.detail : L"Profile folder not found";
        TextIn(dc, Ellipsize(dc, detail, g_fonts.monoSmall, textRight - textLeft),
               R(textLeft, ry + S(36), textRight, ry + S(56)), g_fonts.monoSmall, theme::faint,
               DT_LEFT | DT_TOP | DT_SINGLELINE);

        std::wstring sizeText = b.scanned ? FormatBytes(b.bytes) : (b.installed ? L"-" : L"");
        TextIn(dc, sizeText, R(rr.right - S(160), ry + S(13), rr.right - S(20), ry + S(35)),
               g_fonts.h3, b.installed ? theme::text : theme::faint,
               DT_RIGHT | DT_TOP | DT_SINGLELINE);
        std::wstring countText =
            b.scanned && b.installed ? FormatCount(b.files) + L" files" : L"";
        TextIn(dc, countText, R(rr.right - S(160), ry + S(38), rr.right - S(20), ry + S(58)),
               g_fonts.small, theme::muted, DT_RIGHT | DT_TOP | DT_SINGLELINE);

        if (clickable) RecordHit(hits, rr, id);
    }
    RestoreDC(dc, saved);

    TextAt(dc, L"A browser that is running keeps its databases open, so it is skipped until it "
                L"closes. Cleaning signs you out of sites and clears autofill.",
           cx, h - S(44), g_fonts.tiny, theme::faint);
}

// -------------------------------------------------------------------- dns
static void PaintDns(HDC dc, int cx, int cw, int h, AppState& st, std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"DNS",
           L"Point every adapter at one trusted resolver, and keep it from being changed.");

    int by = S(112);
    int btnH = S(38);

    RECT refresh = R(cx, by, cx + S(104), by + btnH);
    Button(dc, refresh, L"Refresh", false, st.hoverId == HitDnsRefresh);
    RecordHit(hits, refresh, HitDnsRefresh);

    RECT apply = R(cx + cw - S(206), by, cx + cw, by + btnH);
    Button(dc, apply, L"Apply to all adapters", true, st.hoverId == HitDnsApply, !st.dnsBusy);
    RecordHit(hits, apply, HitDnsApply, !st.dnsBusy);

    RECT restore = R(apply.left - S(12) - S(150), by, apply.left - S(12), by + btnH);
    Button(dc, restore, L"Use system DNS", false, st.hoverId == HitDnsRestore, !st.dnsBusy);
    RecordHit(hits, restore, HitDnsRestore, !st.dnsBusy);

    int chipH = S(34);
    int cy = by + S(52);
    const auto& providers = DnsProviders();
    static std::vector<const wchar_t*> labels;
    labels.clear();
    for (const auto& provider : providers) labels.push_back(provider.name);
    ChipRow(dc, hits, cx, cy, HitDnsProviderBase, labels.data(), (int)labels.size(),
            std::clamp(st.dnsProvider, 0, (int)providers.size() - 1), chipH);

    int wy = cy + chipH + S(16);
    int wH = S(64);
    RECT watch = R(cx, wy, cx + cw, wy + wH);
    Card(dc, watch, false);
    TextAt(dc, L"Watchdog", watch.left + S(22), watch.top + S(14), g_fonts.h3, theme::text);
    TextAt(dc, L"Re-writes the servers every 1.5 s whenever anything changes them.",
           watch.left + S(120), watch.top + S(16), g_fonts.tiny, theme::faint);
    wchar_t buf[200];
    if (st.dnsIncidents > 0) {
        swprintf(buf, 200, L"%lu change(s) reverted this session", (unsigned long)st.dnsIncidents);
        TextAt(dc, buf, watch.left + S(22), watch.top + S(36), g_fonts.tiny, theme::warn);
    } else {
        TextAt(dc, st.dnsApplied ? L"Nothing has interfered so far."
                                 : L"Turns on once a provider is applied.",
               watch.left + S(22), watch.top + S(36), g_fonts.tiny, theme::muted);
    }
    RECT wBtn = R(watch.right - S(122), watch.top + S(16), watch.right - S(22), watch.top + S(48));
    ChipButton(dc, wBtn, st.dnsWatchdog ? L"On" : L"Off", st.dnsWatchdog,
               st.hoverId == HitDnsWatchdog);
    RecordHit(hits, wBtn, HitDnsWatchdog);

    int listTop = wy + wH + S(16);
    if (!st.dnsMessage.empty()) {
        RECT msg = R(cx, listTop - S(4), cx + cw, listTop + S(22));
        TextIn(dc, st.dnsMessage, msg, g_fonts.small,
               st.dnsMessageIsError ? theme::danger : theme::accent,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        listTop += S(32);
    }

    // The adapters scroll when there are more than the space left, so every adapter Windows
    // reports can be read. The wheel moves the list by a whole card, and the list ends on a
    // card edge, so no adapter is ever left half drawn at the bottom.
    int rowH = S(64), gap = S(8), rowStep = rowH + gap;
    int total = (int)st.dnsAdapters.size();
    int room = std::max(0, (h - S(62)) - listTop);
    int rowsVisible = std::max(1, (room + gap) / rowStep);
    int listH = rowsVisible * rowStep - gap;
    int listBottom = listTop + listH;
    int totalH = total * rowStep;
    int maxScroll = std::max(0, (total - rowsVisible) * rowStep);
    // Rounding rather than flooring is what makes one wheel notch (64 px) move one card
    // (72 px) instead of snapping straight back to where it started.
    st.scrollDns =
        std::clamp((st.scrollDns + rowStep / 2) / rowStep * rowStep, 0, maxScroll);

    if (total > 0) {
        // The count sits on the button row, where it cannot be mistaken for one of the
        // adapter cards, and says so only when there is more to reach.
        if (maxScroll > 0)
            swprintf(buf, 200, L"%d adapters  -  scroll for all", total);
        else
            swprintf(buf, 200, L"%d adapter%ls", total, total == 1 ? L"" : L"s");
        TextIn(dc, buf, R(cx + S(120), by, restore.left - S(16), by + btnH), g_fonts.tiny,
               theme::faint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (st.dnsAdapters.empty()) {
        TextIn(dc, L"No configured network adapter was found.",
               R(cx, listTop + S(20), cx + cw, listTop + S(50)), g_fonts.small, theme::muted,
               DT_CENTER | DT_TOP | DT_SINGLELINE);
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, cx, listTop, cx + cw, listBottom);
    for (size_t i = 0; i < st.dnsAdapters.size(); ++i) {
        const DnsAdapter& adapter = st.dnsAdapters[i];
        int ry = listTop - st.scrollDns + (int)i * (rowH + gap);
        if (ry + rowH < listTop - S(4) || ry > listBottom) continue;
        RECT rr = R(cx, ry, cx + cw, ry + rowH);
        Card(dc, rr, false);
        TextIn(dc, adapter.name, R(rr.left + S(22), ry + S(10), rr.right - S(280), ry + S(32)),
               g_fonts.h3, theme::text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        TextIn(dc, adapter.guid, R(rr.left + S(22), ry + S(34), rr.right - S(280), ry + S(54)),
               g_fonts.monoSmall, theme::faint, DT_LEFT | DT_TOP | DT_SINGLELINE);
        TextIn(dc, adapter.current,
               R(rr.right - S(280), ry, rr.right - S(22), ry + rowH), g_fonts.base, theme::muted,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    RestoreDC(dc, saved);

    // A slim bar on the right edge, drawn only when the list runs past the bottom.
    if (maxScroll > 0) {
        int barH = std::max(S(30), listH * listH / std::max(1, totalH));
        int barY = listTop + (listH - barH) * st.scrollDns / std::max(1, maxScroll);
        FillRound(dc, R(cx + cw - S(6), barY, cx + cw - S(2), barY + barH), 2.0f * g_scale,
                  theme::active);
    }

    TextAt(dc, st.elevated ? L"Changing DNS writes to the adapter registry and flushes the resolver "
                             L"cache."
                           : L"Changing DNS needs administrator rights - WinSuite will offer to "
                             L"restart elevated.",
           cx, h - S(44), g_fonts.tiny, theme::faint);
}

// ---------------------------------------------------------------- network
// The Internet & networking page. Four rows, one per tweak, each with its own switch;
// the profile buttons at the top are a convenience over the same per-tweak calls.
// "18 ms avg (12-25 ms), 0% loss" - the one line each latency reading gets.
static std::wstring LatencyLine(const LatencyResult& r) {
    int loss = r.sent > 0 ? (r.sent - r.received) * 100 / r.sent : 0;
    return std::to_wstring(r.avgMs) + L" ms avg (" + std::to_wstring(r.minMs) + L"-" +
           std::to_wstring(r.maxMs) + L" ms), " + std::to_wstring(loss) + L"% loss";
}

static void PaintNetwork(HDC dc, int cx, int cw, int h, AppState& st, std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"Internet & networking",
           L"Tried-and-true Windows settings for a pure gaming connection, each saved before "
           L"it changes.");

    // The page scrolls now that the latency check shares it with the tweak rows, so the
    // rows keep their full explanations instead of being squeezed to fit.
    const int viewTop = S(112);
    const int viewBottom = h - S(62);
    const int heroH = S(116);
    const int latencyH = S(118);
    const int blockGap = S(14);
    const int rowH = S(88), rowGap = S(8);
    const int total = (int)st.netTweaks.size();
    const int rowsH = total > 0 ? total * rowH + (total - 1) * rowGap : 0;
    const int contentH = heroH + blockGap + latencyH + blockGap + rowsH;
    const int viewH = std::max(0, viewBottom - viewTop);
    const int maxScroll = std::max(0, contentH - viewH);
    st.scrollNet = std::clamp(st.scrollNet, 0, maxScroll);

    const size_t firstHit = hits.size();
    int saved = SaveDC(dc);
    IntersectClipRect(dc, cx, viewTop, cx + cw, viewBottom);
    int y = viewTop - st.scrollNet;

    RECT hero = R(cx, y, cx + cw, y + heroH);
    Card(dc, hero, false);
    int applied = 0, revertable = 0;
    bool needApply = false;
    for (const auto& tweak : st.netTweaks) {
        if (tweak.applied) ++applied;
        if (tweak.journaled) ++revertable;
        if (tweak.available && !tweak.applied) needApply = true;
    }

    const wchar_t* headline = applied == 0     ? L"Gaming profile is off"
                              : applied == total && total > 0 ? L"Gaming profile is on"
                                                             : L"Gaming profile is partly on";
    TextAt(dc, headline, hero.left + S(24), hero.top + S(16), g_fonts.h1,
           applied == total && total > 0 ? theme::accent : theme::text);

    wchar_t buf[160];
    swprintf(buf, 160, L"%d of %d tweaks active", applied, total);
    TextAt(dc, buf, hero.left + S(24), hero.top + S(54), g_fonts.small, theme::muted);

    // The third row carries the last answer and two utility actions: the one-press
    // restore-to-Windows-defaults button, then the re-check link. The message stops short
    // of them rather than running underneath.
    int btnH = S(38);
    RECT revert = R(hero.right - S(22) - S(112), hero.top + S(20), hero.right - S(22),
                    hero.top + S(20) + btnH);
    RECT apply = R(revert.left - S(10) - S(146), hero.top + S(20), revert.left - S(10),
                   hero.top + S(20) + btnH);

    const wchar_t* defaultsLabel = L"Restore Windows defaults";
    const int defaultsW = TextWidth(dc, defaultsLabel, g_fonts.h3) + S(30);
    RECT defaults = R(hero.right - S(22) - defaultsW, hero.top + S(73), hero.right - S(22),
                      hero.top + S(101));
    RECT recheck = R(defaults.left - S(18) - S(84), hero.top + S(76), defaults.left - S(18),
                     hero.top + S(100));
    const int messageRoom = (int)(recheck.left - S(16) - (hero.left + S(24)));

    if (!st.netMessage.empty()) {
        TextIn(dc, Ellipsize(dc, st.netMessage, g_fonts.tiny, messageRoom),
               R(hero.left + S(24), hero.top + S(76), recheck.left - S(16), hero.top + S(100)),
               g_fonts.tiny, st.netMessageIsError ? theme::danger : theme::accent,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        TextIn(dc, L"Every value is saved first, so Revert can put the machine back exactly.",
               R(hero.left + S(24), hero.top + S(76), recheck.left - S(16), hero.top + S(100)),
               g_fonts.tiny, theme::faint,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    Button(dc, revert, L"Revert all", false, st.hoverId == HitNetRevert,
           revertable > 0 && !st.netApplyPending);
    RecordHit(hits, revert, HitNetRevert, revertable > 0 && !st.netApplyPending);

    Button(dc, apply, L"Apply profile", true, st.hoverId == HitNetApply,
           needApply && !st.netApplyPending);
    RecordHit(hits, apply, HitNetApply, needApply && !st.netApplyPending);

    // One press switches all four tweaks off. It only does anything while at least one
    // tweak is on; the journal still keeps what each value was, so Revert all works after.
    bool canDefaults = applied > 0 && !st.netApplyPending;
    Button(dc, defaults, defaultsLabel, false, st.hoverId == HitNetDefaults, canDefaults);
    RecordHit(hits, defaults, HitNetDefaults, canDefaults);

    bool recheckHover = st.hoverId == HitNetRefresh;
    TextIn(dc, L"Re-check", recheck, g_fonts.small,
           recheckHover ? theme::accent : theme::muted,
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    if (recheckHover)
        HLine(dc, recheck.right - TextWidth(dc, L"Re-check", g_fonts.small), recheck.right,
              recheck.bottom - S(8), theme::accent);
    RecordHit(hits, recheck, HitNetRefresh);

    // Latency check. The host is a text field so any name or IPv4 address can be used;
    // the button measures before the first reading and after that, which is what makes
    // the tweaks' effect a number rather than a claim. Applying the profile with no
    // reading yet takes the baseline itself, and applying it with one takes the
    // after-reading too, so the comparison appears without a second press.
    y = hero.bottom + blockGap;
    RECT latency = R(cx, y, cx + cw, y + latencyH);
    Card(dc, latency, false);
    TextIn(dc, L"Latency check",
           R(latency.left + S(20), latency.top + S(12), latency.left + S(190),
             latency.top + S(34)),
           g_fonts.h3, theme::text, DT_LEFT | DT_TOP | DT_SINGLELINE);
    TextIn(dc, L"Four pings to one host - measure before the tweaks, then after.",
           R(latency.left + S(200), latency.top + S(15), latency.right - S(20),
             latency.top + S(34)),
           g_fonts.tiny, theme::faint,
           DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT hostField = R(latency.left + S(20), latency.top + S(40),
                       latency.left + S(20) + S(220), latency.top + S(74));
    InputField(dc, hits, hostField, st.netHost, L"Host or IP address",
               st.inputFocus == HitNetHost, st.hoverId, HitNetHost, HitNetHostClear);

    const wchar_t* pingLabel = st.netPinging
                                   ? L"Pinging..."
                                   : (st.netBefore.ok ? L"Measure after" : L"Measure before");
    RECT ping = R(hostField.right + S(10), hostField.top, hostField.right + S(10) + S(150),
                  hostField.bottom);
    Button(dc, ping, pingLabel, true, st.hoverId == HitNetPing, !st.netPinging);
    RecordHit(hits, ping, HitNetPing, !st.netPinging);

    bool canReset = (st.netBefore.ok || st.netAfter.ok) && !st.netPinging;
    RECT reset = R(latency.right - S(20) - S(84), hostField.top, latency.right - S(20),
                   hostField.bottom);
    Button(dc, reset, L"Reset", false, st.hoverId == HitNetReset, canReset);
    RecordHit(hits, reset, HitNetReset, canReset);

    const int lineY = latency.top + S(84);
    if (!st.netBefore.ok) {
        // Applying the profile with no baseline yet measures one itself, so the hint
        // says what is happening instead of asking for a press the user already made.
        std::wstring hint;
        if (st.netApplyPending)
            hint = L"Measuring the baseline to " + st.netHost +
                   L" - the profile goes on when that reading lands.";
        else if (st.netPinging)
            hint = L"Taking the baseline to " + st.netHost + L" ...";
        else
            hint = L"No reading yet - Apply profile takes the baseline, or press Measure "
                   L"before.";
        TextIn(dc, hint, R(latency.left + S(20), lineY, latency.right - S(20), lineY + S(18)),
               g_fonts.small, st.netPinging ? theme::accent : theme::muted,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        std::wstring beforeText = L"Before  " + LatencyLine(st.netBefore);
        TextAt(dc, beforeText, latency.left + S(20), lineY, g_fonts.small, theme::muted);
        if (st.netAfter.ok) {
            int afterX = latency.left + S(20) +
                         TextWidth(dc, beforeText, g_fonts.small) + S(24);
            std::wstring afterText = L"After  " + LatencyLine(st.netAfter);
            TextIn(dc,
                   Ellipsize(dc, afterText, g_fonts.small, latency.right - S(20) - afterX),
                   R(afterX, lineY, latency.right - S(20), lineY + S(18)), g_fonts.small,
                   theme::text, DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
    }

    const int statusY = latency.top + S(102);
    if (st.netPinging) {
        TextIn(dc, st.netPingMessage.empty() ? std::wstring(L"Pinging ...") : st.netPingMessage,
               R(latency.left + S(20), statusY, latency.right - S(20), statusY + S(16)),
               g_fonts.tiny, theme::accent,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else if (!st.netPingMessage.empty()) {
        TextIn(dc, Ellipsize(dc, st.netPingMessage, g_fonts.tiny, cw - S(40)),
               R(latency.left + S(20), statusY, latency.right - S(20), statusY + S(16)),
               g_fonts.tiny, st.netPingMessageIsError ? theme::danger : theme::accent,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else if (st.netBefore.ok && st.netAfter.ok) {
        // "11 ms to 14 ms - 3 ms higher on average": the pair reads as one span first,
        // so the change is visible without subtracting two figures in your head.
        const int delta = st.netAfter.avgMs - st.netBefore.avgMs;
        COLORREF colour = delta < 0 ? theme::accent : delta > 0 ? theme::warn : theme::muted;
        std::wstring text = LatencyComparison(st.netBefore, st.netAfter);
        TextIn(dc, text, R(latency.left + S(20), statusY, latency.right - S(20), statusY + S(16)),
               g_fonts.tiny, colour, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        TextIn(dc, L"Each figure is the average of four echoes with a one-second timeout.",
               R(latency.left + S(20), statusY, latency.right - S(20), statusY + S(16)),
               g_fonts.tiny, theme::faint,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    y = latency.bottom + blockGap;
    if (total > 0) {
        for (int i = 0; i < total; ++i) {
            const NetTweakView& tweak = st.netTweaks[(size_t)i];
            int ry = y + i * (rowH + rowGap);
            RECT row = R(cx, ry, cx + cw, ry + rowH);
            Card(dc, row, false);

            const int switchW = S(112), switchH = S(32);
            RECT sw = R(row.right - S(20) - switchW, row.top + (rowH - switchH) / 2,
                        row.right - S(20), row.top + (rowH - switchH) / 2 + switchH);
            // A saved journal can be put back even when the adapter that took it is
            // temporarily gone, so journaled rows stay actionable on their own. A value
            // that was already in gaming shape is controllable too: turning it off saves
            // it and then removes it, which is the Windows default rather than a guess.
            const bool canAct = tweak.available || tweak.journaled;
            if (canAct) {
                const wchar_t* switchLabel = tweak.journaled ? L"Revert"
                                            : tweak.applied ? L"Turn off"
                                                            : L"Turn on";
                ChipButton(dc, sw, switchLabel, tweak.applied,
                           st.hoverId == HitNetRowBase + i);
                RecordHit(hits, sw, HitNetRowBase + i);
            } else {
                FillRound(dc, sw, 8.0f * g_scale, theme::card);
                StrokeRound(dc, sw, 8.0f * g_scale, theme::cardEdge, 1.0f);
                TextIn(dc, L"Unavailable", sw, g_fonts.small, theme::faint,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            int titleRoom = sw.left - S(16) - (row.left + S(20));
            TextIn(dc, Ellipsize(dc, tweak.title, g_fonts.h3, titleRoom),
                   R(row.left + S(20), row.top + S(10), sw.left - S(16), row.top + S(32)),
                   g_fonts.h3, theme::text,
                   DT_LEFT | DT_TOP | DT_SINGLELINE);
            int titleW = TextWidth(dc, tweak.title, g_fonts.h3);
            if (tweak.available && titleW + S(64) < titleRoom) {
                Pill(dc, row.left + S(20) + titleW + S(12), row.top + S(12),
                     tweak.applied ? L"ON" : L"OFF",
                     tweak.applied ? theme::accentSoft : theme::active,
                     tweak.applied ? theme::accent : theme::muted, g_fonts.tiny);
            }

            std::wstring summary = tweak.available ? tweak.summary : tweak.state;
            TextIn(dc, Ellipsize(dc, summary, g_fonts.small, titleRoom),
                   R(row.left + S(20), row.top + S(36), sw.left - S(16), row.top + S(58)),
                   g_fonts.small, tweak.available ? theme::muted : theme::warn,
                   DT_LEFT | DT_TOP | DT_SINGLELINE);

            if (rowH >= S(82)) {
                std::wstring note = tweak.state + L"  -  " + tweak.writes;
                TextIn(dc, Ellipsize(dc, note, g_fonts.tiny, titleRoom),
                       R(row.left + S(20), row.top + S(58), sw.left - S(16), row.top + S(76)),
                       g_fonts.tiny, theme::faint,
                       DT_LEFT | DT_TOP | DT_SINGLELINE);
            }
        }
    }
    RestoreDC(dc, saved);

    // Only the part of a card that is on screen answers a click, so a press in the footer
    // never reaches one that has scrolled underneath it.
    for (size_t i = firstHit; i < hits.size();) {
        RECT& r = hits[i].rect;
        r.top = std::max<LONG>(r.top, viewTop);
        r.bottom = std::min<LONG>(r.bottom, viewBottom);
        if (r.bottom <= r.top)
            hits.erase(hits.begin() + (long)i);
        else
            ++i;
    }

    if (maxScroll > 0) {
        int barH = std::max(S(30), viewH * viewH / std::max(1, contentH));
        int barY = viewTop + (viewH - barH) * st.scrollNet / std::max(1, maxScroll);
        FillRound(dc, R(cx + cw + S(10), barY, cx + cw + S(14), barY + barH), 2.0f * g_scale,
                  theme::active);
    }

    TextIn(dc,
           st.elevated
               ? L"A restart makes Windows read the new values. Revert restores exactly what "
                 L"was there before, even after a reboot."
               : L"Changing these needs administrator rights - WinSuite will offer to restart "
                 L"elevated.",
           R(cx, h - S(44), cx + cw, h - S(24)), g_fonts.tiny, theme::faint,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ------------------------------------------------------------- Game Booster
// The Game Booster page: System Turbo's profile as the first row, then the safe tweaks,
// then the advanced ones. The list scrolls because the whole point is that every row
// keeps its explanation visible rather than being squeezed to fit.
static void PaintGameBooster(HDC dc, int cx, int cw, int h, AppState& st, std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"Game Booster",
           L"System Turbo's profile plus gaming tweaks - tried and true, then the advanced ones, "
           L"each reversible.");

    int top = S(112);
    int heroH = S(112);
    RECT hero = R(cx, top, cx + cw, top + heroH);
    Card(dc, hero, false);

    int applied = 0, journaled = 0, profileRows = 0, profileApplied = 0;
    int advancedRows = 0, advancedApplied = 0;
    for (const auto& tweak : st.gameTweaks) {
        if (tweak.applied) ++applied;
        if (tweak.journaled) ++journaled;
        if (tweak.inProfile) {
            ++profileRows;
            if (tweak.applied) ++profileApplied;
        } else {
            ++advancedRows;
            if (tweak.applied) ++advancedApplied;
        }
    }
    const bool profileOn = st.boost.active;
    // The profile never touches the advanced rows, so they cannot decide whether it reads
    // as on: all of the profile's own rows plus the system row is "on", and nothing else.
    const bool profileComplete = profileOn && profileApplied == profileRows;
    const bool needApply = !profileOn || profileApplied < profileRows;
    const bool canRevert = profileOn || journaled > 0;

    const wchar_t* headline = !profileOn && applied == 0
                                  ? L"Game Booster is off"
                                  : profileComplete ? L"Game Booster is on"
                                                    : L"Game Booster is partly on";
    TextAt(dc, headline, hero.left + S(24), hero.top + S(16), g_fonts.h1,
           profileComplete ? theme::accent : theme::text);

    wchar_t buf[220];
    swprintf(buf, 220, L"System profile %ls  -  %d/%d profile tweaks  -  %d/%d advanced",
             profileOn ? L"on" : L"off", profileApplied, profileRows, advancedApplied,
             advancedRows);
    TextAt(dc, buf, hero.left + S(24), hero.top + S(54), g_fonts.small, theme::muted);

    int btnH = S(38);
    RECT revert = R(hero.right - S(22) - S(128), hero.top + S(20), hero.right - S(22),
                    hero.top + S(20) + btnH);
    Button(dc, revert, L"Revert all", false, st.hoverId == HitBoostRestore,
           canRevert && !st.boostBusy);
    RecordHit(hits, revert, HitBoostRestore, canRevert && !st.boostBusy);

    RECT apply = R(revert.left - S(10) - S(158), hero.top + S(20), revert.left - S(10),
                   hero.top + S(20) + btnH);
    Button(dc, apply, st.boostBusy ? L"Working..." : L"Apply profile", true,
           st.hoverId == HitBoostToggle, needApply && !st.boostBusy);
    RecordHit(hits, apply, HitBoostToggle, needApply && !st.boostBusy);

    if (!st.boosterMessage.empty()) {
        TextIn(dc, Ellipsize(dc, st.boosterMessage, g_fonts.tiny, apply.left - hero.left - S(46)),
               R(hero.left + S(24), hero.top + S(76), apply.left - S(16), hero.top + S(100)),
               g_fonts.tiny, st.boosterMessageIsError ? theme::danger : theme::accent,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        TextIn(dc, L"The profile covers the system row and every tried-and-true tweak.",
               R(hero.left + S(24), hero.top + S(76), apply.left - S(16), hero.top + S(100)),
               g_fonts.tiny, theme::faint,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    RECT recheck = R(hero.right - S(22) - S(84), hero.top + S(76), hero.right - S(22),
                     hero.top + S(100));
    bool recheckHover = st.hoverId == HitBoostRefresh;
    TextIn(dc, L"Re-check", recheck, g_fonts.small,
           recheckHover ? theme::accent : theme::muted,
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    if (recheckHover)
        HLine(dc, recheck.right - TextWidth(dc, L"Re-check", g_fonts.small), recheck.right,
              recheck.bottom - S(8), theme::accent);
    RecordHit(hits, recheck, HitBoostRefresh);

    int listTop = hero.bottom + S(12);
    int listBottom = h - S(56);
    int gap = S(8);

    // The row plan: profile, section label, four safe rows, section label, three advanced
    // rows, section label, then one row per background service. Labels scroll with their
    // rows so a group heading can never be left behind.
    const int labelH = S(28);
    const int profileH = S(84);
    const int tweakH = S(72);
    // The extent has to match the loop below exactly, or the last row either cannot be
    // scrolled into view or the list ends in empty space. The ADVANCED label is drawn
    // only when a row outside the profile exists, so it is counted the same way.
    bool hasAdvanced = false;
    for (const auto& tweak : st.gameTweaks)
        if (!tweak.inProfile) hasAdvanced = true;
    const int labels = hasAdvanced ? 3 : 2;  // tried and true, advanced?, background services
    const int rows = (int)(st.gameTweaks.size() + st.gameServices.size());
    int contentH = profileH + labels * labelH + rows * tweakH + (labels + 1 + rows) * gap;
    if (contentH > 0) contentH -= gap;
    const int listH = std::max(0, listBottom - listTop);
    const int maxScroll = std::max(0, contentH - listH);
    st.scrollBooster = std::clamp(st.scrollBooster, 0, maxScroll);

    int saved = SaveDC(dc);
    IntersectClipRect(dc, cx, listTop, cx + cw, listBottom);
    int y = listTop - st.scrollBooster;
    auto record = [&](const RECT& button, int id, bool enabled) {
        if (button.bottom > listTop && button.top < listBottom) RecordHit(hits, button, id, enabled);
    };

    // ---- the System Turbo profile row
    {
        RECT row = R(cx, y, cx + cw, y + profileH);
        Card(dc, row, false);
        int swW = S(112), swH = S(32);
        RECT sw = R(row.right - S(20) - swW, row.top + (profileH - swH) / 2,
                    row.right - S(20), row.top + (profileH - swH) / 2 + swH);
        TextIn(dc, L"System profile", R(row.left + S(20), row.top + S(10), sw.left - S(16),
                                       row.top + S(32)),
               g_fonts.h3, theme::text, DT_LEFT | DT_TOP | DT_SINGLELINE);
        if (profileOn)
            Pill(dc, row.left + S(20) + TextWidth(dc, L"System profile", g_fonts.h3) + S(12),
                 row.top + S(12), L"ON", theme::accentSoft, theme::accent, g_fonts.tiny);

        std::wstring summary =
            profileOn ? (st.boost.parked.empty()
                             ? L"High performance plan, no background service needed parking, "
                               L"Game DVR recording off."
                             : L"High performance plan, background services parked, Game DVR "
                               L"recording off.")
                      : L"High performance plan, fewer background services, no Game DVR "
                        L"recording.";
        TextIn(dc, Ellipsize(dc, summary, g_fonts.small, sw.left - S(36) - row.left),
               R(row.left + S(20), row.top + S(34), sw.left - S(16), row.top + S(54)),
               g_fonts.small, theme::muted, DT_LEFT | DT_TOP | DT_SINGLELINE);

        std::wstring how = profileOn
                               ? (st.boost.currentPlan.empty() ? L"High performance"
                                                               : st.boost.currentPlan) +
                                     L"  -  " + std::to_wstring(st.boost.parked.size()) +
                                     L" services parked"
                               : L"One switch for the power plan, the parked services and the "
                                 L"DVR.";
        TextIn(dc, Ellipsize(dc, how, g_fonts.tiny, sw.left - S(36) - row.left),
               R(row.left + S(20), row.top + S(56), sw.left - S(16), row.top + S(74)),
               g_fonts.tiny, theme::faint, DT_LEFT | DT_TOP | DT_SINGLELINE);

        const wchar_t* label = st.boostBusy ? L"Working..." : profileOn ? L"Restore" : L"Apply";
        bool enabled = !st.boostBusy;
        ChipButton(dc, sw, label, profileOn, st.hoverId == HitBoostProfile);
        record(sw, HitBoostProfile, enabled);
        y += profileH + gap;
    }

    // ---- safe group
    {
        TextIn(dc, L"TRIED AND TRUE", R(cx, y + S(2), cx + cw, y + labelH), g_fonts.tiny,
               theme::faint, DT_LEFT | DT_TOP | DT_SINGLELINE);
        TextIn(dc, L"applied by the profile", R(cx, y + S(2), cx + cw, y + labelH),
               g_fonts.tiny, theme::faint,
               DT_RIGHT | DT_TOP | DT_SINGLELINE);
        y += labelH + gap;
    }

    bool advancedLabelDrawn = false;
    for (size_t i = 0; i < st.gameTweaks.size(); ++i) {
        const GameTweakView& tweak = st.gameTweaks[i];
        if (!tweak.inProfile && !advancedLabelDrawn) {
            TextIn(dc, L"ADVANCED - LESS CERTAIN", R(cx, y + S(2), cx + cw, y + labelH),
                   g_fonts.tiny, theme::warn, DT_LEFT | DT_TOP | DT_SINGLELINE);
            TextIn(dc, L"not applied by the profile", R(cx, y + S(2), cx + cw, y + labelH),
                   g_fonts.tiny, theme::faint, DT_RIGHT | DT_TOP | DT_SINGLELINE);
            y += labelH + gap;
            advancedLabelDrawn = true;
        }

        RECT row = R(cx, y, cx + cw, y + tweakH);
        Card(dc, row, false);
        int swW = S(112), swH = S(30);
        RECT sw = R(row.right - S(20) - swW, row.top + (tweakH - swH) / 2,
                    row.right - S(20), row.top + (tweakH - swH) / 2 + swH);
        int titleRoom = sw.left - S(16) - (row.left + S(20));

        TextIn(dc, Ellipsize(dc, tweak.title, g_fonts.h3, titleRoom),
               R(row.left + S(20), row.top + S(9), sw.left - S(16), row.top + S(30)),
               g_fonts.h3, theme::text, DT_LEFT | DT_TOP | DT_SINGLELINE);
        int titleW = TextWidth(dc, tweak.title, g_fonts.h3);
        if (titleW + S(60) < titleRoom) {
            Pill(dc, row.left + S(20) + titleW + S(12), row.top + S(11),
                 tweak.applied ? L"ON" : L"OFF",
                 tweak.applied ? theme::accentSoft : theme::active,
                 tweak.applied ? theme::accent : theme::muted, g_fonts.tiny);
        }

        TextIn(dc, Ellipsize(dc, tweak.summary, g_fonts.small, titleRoom),
               R(row.left + S(20), row.top + S(32), sw.left - S(16), row.top + S(52)),
               g_fonts.small, theme::muted, DT_LEFT | DT_TOP | DT_SINGLELINE);
        TextIn(dc, Ellipsize(dc, tweak.how, g_fonts.tiny, titleRoom),
               R(row.left + S(20), row.top + S(54), sw.left - S(16), row.top + S(70)),
               g_fonts.tiny, theme::faint, DT_LEFT | DT_TOP | DT_SINGLELINE);

        // Every row that can be read can be switched here: Revert puts back what this
        // suite saved, Turn on applies a row that is off, and Turn off is offered even
        // when the value was set by another tool (so nothing has to be changed elsewhere
        // before this page will act).
        if (!tweak.available) {
            FillRound(dc, sw, 8.0f * g_scale, theme::card);
            StrokeRound(dc, sw, 8.0f * g_scale, theme::cardEdge, 1.0f);
            TextIn(dc, L"Unavailable", sw, g_fonts.small, theme::faint,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            const wchar_t* label =
                tweak.journaled ? L"Revert" : (tweak.applied ? L"Turn off" : L"Turn on");
            ChipButton(dc, sw, label, tweak.applied, st.hoverId == HitGameTweakBase + (int)i);
            record(sw, HitGameTweakBase + (int)i, true);
        }
        y += tweakH + gap;
    }

    // ---- background services
    // The long list of services a gaming session does not need. None of them is part of the
    // profile: what can safely be stopped depends on the machine, so each row is a switch of
    // its own, and Revert (or Revert all) puts back the start type, running state and
    // delayed flag it had before.
    {
        int alreadyOff = 0;
        for (const auto& service : st.gameServices)
            if (service.applied) ++alreadyOff;
        wchar_t caption[128];
        swprintf(caption, 128, L"%d of %d already off - not applied by the profile", alreadyOff,
                 (int)st.gameServices.size());
        TextIn(dc, L"BACKGROUND SERVICES", R(cx, y + S(2), cx + cw, y + labelH), g_fonts.tiny,
               theme::faint, DT_LEFT | DT_TOP | DT_SINGLELINE);
        TextIn(dc, caption, R(cx, y + S(2), cx + cw, y + labelH), g_fonts.tiny, theme::faint,
               DT_RIGHT | DT_TOP | DT_SINGLELINE);
        y += labelH + gap;
    }
    for (size_t i = 0; i < st.gameServices.size(); ++i) {
        const GameServiceView& service = st.gameServices[i];
        RECT row = R(cx, y, cx + cw, y + tweakH);
        Card(dc, row, false);
        int swW = S(112), swH = S(30);
        RECT sw = R(row.right - S(20) - swW, row.top + (tweakH - swH) / 2, row.right - S(20),
                    row.top + (tweakH - swH) / 2 + swH);
        int titleRoom = sw.left - S(16) - (row.left + S(20));
        const int id = HitGameServiceBase + (int)i;

        TextIn(dc, Ellipsize(dc, service.title, g_fonts.h3, titleRoom),
               R(row.left + S(20), row.top + S(9), sw.left - S(16), row.top + S(30)),
               g_fonts.h3, theme::text, DT_LEFT | DT_TOP | DT_SINGLELINE);
        int titleW = TextWidth(dc, service.title, g_fonts.h3);
        if (titleW + S(60) < titleRoom) {
            // A row with nothing installed on this PC is never "on", however it reads.
            const bool showing = service.installed > 0 && !service.applied;
            Pill(dc, row.left + S(20) + titleW + S(12), row.top + S(11), showing ? L"ON" : L"OFF",
                 showing ? theme::accentSoft : theme::active,
                 showing ? theme::accent : theme::muted, g_fonts.tiny);
        }

        TextIn(dc, Ellipsize(dc, service.summary, g_fonts.small, titleRoom),
               R(row.left + S(20), row.top + S(32), sw.left - S(16), row.top + S(52)),
               g_fonts.small, theme::muted, DT_LEFT | DT_TOP | DT_SINGLELINE);
        // Windows leans on the critical rows (Event Log, Remote Desktop, Netlogon), so their
        // state line is the warning colour rather than the usual faint grey.
        TextIn(dc, Ellipsize(dc, service.state, g_fonts.tiny, titleRoom),
               R(row.left + S(20), row.top + S(54), sw.left - S(16), row.top + S(70)),
               g_fonts.tiny, service.critical ? theme::warn : theme::faint,
               DT_LEFT | DT_TOP | DT_SINGLELINE);

        // Every switch this page draws does something: Turn off parks the services, Revert
        // restores exactly what they were, and a row Windows already had off - or that is not
        // on this PC - is labelled rather than offered as a button that would do nothing.
        if (service.installed == 0 || (service.applied && !service.journaled)) {
            FillRound(dc, sw, 8.0f * g_scale, theme::card);
            StrokeRound(dc, sw, 8.0f * g_scale, theme::cardEdge, 1.0f);
            TextIn(dc, service.installed == 0 ? L"Not installed" : L"Already off", sw,
                   g_fonts.small, theme::faint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (service.journaled) {
            ChipButton(dc, sw, L"Revert", service.applied, st.hoverId == id);
            record(sw, id, true);
        } else {
            ChipButton(dc, sw, L"Turn off", false, st.hoverId == id);
            record(sw, id, true);
        }
        y += tweakH + gap;
    }
    RestoreDC(dc, saved);

    if (maxScroll > 0) {
        int trackH = listH;
        int barH = std::max(S(30), trackH * listH / std::max(1, contentH));
        int barY = listTop + (trackH - barH) * st.scrollBooster / std::max(1, maxScroll);
        FillRound(dc, R(cx + cw + S(10), barY, cx + cw + S(14), barY + barH), 2.0f * g_scale,
                  theme::active);
    }

    TextIn(dc,
           st.elevated
               ? L"Safe tweaks apply at once; the advanced rows take a restart. Turn off puts a row "
                 L"back to the Windows default, and Revert all restores every saved value, "
                 L"services included."
               : L"Advanced tweaks need administrator rights - WinSuite will offer to restart "
                 L"elevated.",
           R(cx, h - S(42), cx + cw, h - S(22)), g_fonts.tiny, theme::faint,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ------------------------------------------------------------- large files
static std::wstring BaseName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

static void PaintLargeFiles(HDC dc, int cx, int cw, int h, AppState& st, std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"Large files",
           L"Biggest files in your profile and extra local drives. Removal goes to the Recycle Bin.");

    int by = S(112);
    int btnH = S(38);
    bool busy = st.largeScanning || st.largeDeleting;
    RECT scanBtn = R(cx, by, cx + S(126), by + btnH);
    const wchar_t* scanLabel =
        st.largeDeleting ? L"Recycling..." : st.largeScanning ? L"Scanning..." : L"Scan now";
    Button(dc, scanBtn, scanLabel, true, st.hoverId == HitLargeScan, !busy);
    RecordHit(hits, scanBtn, HitLargeScan, !busy);

    struct Chip {
        uint64_t bytes;
        const wchar_t* label;
    };
    const Chip chips[] = {{100ull << 20, L"100 MB+"},
                          {500ull << 20, L"500 MB+"},
                          {1ull << 30, L"1 GB+"},
                          {5ull << 30, L"5 GB+"}};
    int chipX = scanBtn.right + S(14);
    for (int i = 0; i < 4; ++i) {
        int chipW = TextWidth(dc, chips[i].label, g_fonts.small) + S(24);
        RECT cr = R(chipX, by + S(4), chipX + chipW, by + btnH - S(4));
        bool active = st.largeThreshold == chips[i].bytes;
        bool hovered = st.hoverId == HitLargeChipBase + i;
        FillRound(dc, cr, 7.0f * g_scale,
                  active ? theme::accentSoft : (hovered ? theme::hover : theme::card));
        StrokeRound(dc, cr, 7.0f * g_scale, active ? theme::accent : theme::cardEdge, 1.0f);
        TextIn(dc, chips[i].label, cr, g_fonts.small,
               active ? theme::accent : theme::muted,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RecordHit(hits, cr, HitLargeChipBase + i, !busy);
        chipX += chipW + S(8);
    }

    int selectedCount = 0;
    uint64_t selectedBytes = 0;
    for (const auto& f : st.largeFiles) {
        if (!f.selected) continue;
        ++selectedCount;
        selectedBytes += f.bytes;
    }
    bool canRecycle = !busy && selectedCount > 0;
    RECT recycle = R(cx + cw - S(214), by, cx + cw, by + btnH);
    Button(dc, recycle, L"Send to Recycle Bin", false, st.hoverId == HitLargeRecycle, canRecycle);
    TextIn(dc, L"Send to Recycle Bin", recycle, g_fonts.h3,
           canRecycle ? theme::danger : theme::faint,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RecordHit(hits, recycle, HitLargeRecycle, canRecycle);

    wchar_t buf[256];
    int statusY = by + btnH + S(14);
    std::wstring status;
    COLORREF statusColor = theme::muted;
    if (!st.largeMessage.empty()) {
        status = st.largeMessage;
        statusColor = st.largeMessageIsError ? theme::danger : theme::accent;
    } else if (busy) {
        status = st.largeDeleting ? L"Moving files to the Recycle Bin..." : L"Scanning folders...";
    } else if (st.largeScanned) {
        uint64_t total = 0;
        for (const auto& f : st.largeFiles) total += f.bytes;
        swprintf(buf, 256, L"%d files  -  %ls total  -  largest first",
                 (int)st.largeFiles.size(), FormatBytes(total).c_str());
        status = buf;
        statusColor = theme::text;
    } else {
        status = L"Not scanned yet";
    }
    RECT statusRect = R(cx, statusY, cx + cw, statusY + S(22));
    TextIn(dc, status, statusRect, g_fonts.small, statusColor,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (selectedCount > 0) {
        std::wstring sel = std::to_wstring(selectedCount) + L" selected  -  " +
                           FormatBytes(selectedBytes);
        TextIn(dc, sel, statusRect, g_fonts.small, theme::accent,
               DT_RIGHT | DT_TOP | DT_SINGLELINE);
    }

    int listTop = statusY + S(30);
    int listBottom = h - S(62);
    int rowH = S(66), gap = S(10);
    int listH = listBottom - listTop;
    int totalH = (int)st.largeFiles.size() * (rowH + gap);
    int maxScroll = std::max(0, totalH - listH - gap);
    st.scrollLarge = std::clamp(st.scrollLarge, 0, maxScroll);

    if (!st.largeScanned && !busy) {
        std::wstring threshold = FormatBytes(st.largeThreshold);
        int cyy = listTop + listH / 2 - S(46);
        TextIn(dc, L"No scan yet", R(cx, cyy, cx + cw, cyy + S(28)), g_fonts.h2, theme::text,
               DT_CENTER | DT_TOP | DT_SINGLELINE);
        std::wstring hint = L"Scan to find files of " + threshold +
                            L" and larger in your profile and extra drives.";
        TextIn(dc, hint, R(cx, cyy + S(30), cx + cw, cyy + S(54)), g_fonts.small, theme::muted,
               DT_CENTER | DT_TOP | DT_SINGLELINE);
    } else if (st.largeScanned && st.largeFiles.empty() && !busy) {
        std::wstring none =
            L"No files of " + FormatBytes(st.largeThreshold) + L" or larger were found.";
        TextIn(dc, none, R(cx, listTop + listH / 2 - S(14), cx + cw, listTop + listH / 2 + S(14)),
               g_fonts.h2, theme::muted, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, cx, listTop, cx + cw, listBottom);
    for (size_t i = 0; i < st.largeFiles.size(); ++i) {
        const LargeFile& f = st.largeFiles[i];
        int ry = listTop - st.scrollLarge + (int)i * (rowH + gap);
        if (ry + rowH < listTop - S(4) || ry > listBottom) continue;
        RECT rr = R(cx, ry, cx + cw, ry + rowH);
        int id = HitLargeRowBase + (int)i;
        Card(dc, rr, st.hoverId == id);

        int checkSize = S(20);
        RECT cb = R(rr.left + S(20), ry + rowH / 2 - checkSize / 2,
                    rr.left + S(20) + checkSize, ry + rowH / 2 + checkSize / 2);
        if (f.selected) {
            FillRound(dc, cb, 5.0f * g_scale, theme::accent);
            HPEN pen = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(9, 26, 18));
            HGDIOBJ old = SelectObject(dc, pen);
            int midY = (cb.top + cb.bottom) / 2;
            MoveToEx(dc, cb.left + S(5), midY, nullptr);
            LineTo(dc, cb.left + S(9), midY + S(4));
            LineTo(dc, cb.right - S(5), midY - S(5));
            SelectObject(dc, old);
            DeleteObject(pen);
        } else {
            FillRound(dc, cb, 5.0f * g_scale, RGB(20, 26, 33));
            StrokeRound(dc, cb, 5.0f * g_scale, theme::active, 1.0f);
        }

        int textLeft = rr.left + S(54);
        int textRight = rr.right - S(170);
        TextIn(dc, Ellipsize(dc, BaseName(f.path), g_fonts.h3, textRight - textLeft),
               R(textLeft, ry + S(10), textRight, ry + S(32)), g_fonts.h3, theme::text,
               DT_LEFT | DT_TOP | DT_SINGLELINE);
        TextIn(dc, Ellipsize(dc, f.path, g_fonts.monoSmall, textRight - textLeft),
               R(textLeft, ry + S(34), textRight, ry + S(54)), g_fonts.monoSmall, theme::faint,
               DT_LEFT | DT_TOP | DT_SINGLELINE);

        TextIn(dc, FormatBytes(f.bytes), R(rr.right - S(170), ry + S(12), rr.right - S(20), ry + S(36)),
               g_fonts.h3, theme::text, DT_RIGHT | DT_TOP | DT_SINGLELINE);
        TextIn(dc, FormatFileTime(f.modifiedTicks),
               R(rr.right - S(170), ry + S(36), rr.right - S(20), ry + S(56)), g_fonts.small,
               theme::muted, DT_RIGHT | DT_TOP | DT_SINGLELINE);

        if (!busy) RecordHit(hits, rr, id);
    }
    RestoreDC(dc, saved);

    TextAt(dc, L"Removed files go to the Recycle Bin, so they can be restored. Windows, Program "
                L"Files and other program folders are never listed.",
           cx, h - S(44), g_fonts.tiny, theme::faint);
}

// ----------------------------------------------------------------- memory
static void PaintMemory(HDC dc, int cx, int cw, int h, AppState& st, std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"Memory",
           L"How much is installed, what is using it, and two levels of cleaning.");

    int by = S(112);
    int btnH = S(38);
    int btnW = S(148);
    bool canAct = !st.memBusy;

    RECT deepBtn = R(cx + cw - btnW, by, cx + cw, by + btnH);
    Button(dc, deepBtn, L"Deep clean", false, st.hoverId == HitMemDeep, canAct);
    TextIn(dc, L"Deep clean", deepBtn, g_fonts.h3, canAct ? theme::danger : theme::faint,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RecordHit(hits, deepBtn, HitMemDeep, canAct);

    RECT regularBtn = R(deepBtn.left - S(12) - btnW, by, deepBtn.left - S(12), by + btnH);
    Button(dc, regularBtn, L"Regular clean", true, st.hoverId == HitMemClean, canAct);
    RecordHit(hits, regularBtn, HitMemClean, canAct);

    RECT refresh = R(regularBtn.left - S(12) - S(96), by, regularBtn.left - S(12), by + btnH);
    Button(dc, refresh, L"Refresh", false, st.hoverId == HitMemRefresh);
    RecordHit(hits, refresh, HitMemRefresh);

    if (st.memBusy)
        TextIn(dc, L"Working...", R(cx, by, refresh.left - S(16), by + btnH), g_fonts.small,
               theme::muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    const MemStats& mem = st.mem;
    uint64_t used = mem.total > mem.avail ? mem.total - mem.avail : 0;
    uint64_t freeBytes = mem.avail > mem.cached ? mem.avail - mem.cached : 0;
    double usedPct = mem.total ? 100.0 * (double)used / (double)mem.total : 0.0;
    double commitPct =
        mem.commitLimit ? 100.0 * (double)mem.committed / (double)mem.commitLimit : 0.0;
    double cachePct = mem.total ? 100.0 * (double)mem.cached / (double)mem.total : 0.0;
    wchar_t buf[256];

    int top = S(160);
    int gap = S(14);
    int cardH = S(118);
    int cardW = (cw - gap * 3) / 4;

    swprintf(buf, 256, L"%d%%", (int)std::lround(usedPct));
    StatCard(dc, R(cx, top, cx + cardW, top + cardH), L"IN USE", buf,
             FormatBytes(used) + L" of " + FormatBytes(mem.total), usedPct, theme::accent);
    StatCard(dc, R(cx + (cardW + gap), top, cx + (cardW + gap) * 2, top + cardH),
             L"AVAILABLE", FormatBytes(mem.avail), L"free plus standby", 100.0 - usedPct,
             RGB(96, 165, 250));
    StatCard(dc, R(cx + (cardW + gap) * 2, top, cx + (cardW + gap) * 3, top + cardH),
             L"COMMITTED", FormatBytes(mem.committed),
             L"of " + FormatBytes(mem.commitLimit) + L" limit", commitPct, theme::warn);
    StatCard(dc, R(cx + (cardW + gap) * 3, top, cx + cw, top + cardH), L"CACHED",
             FormatBytes(mem.cached), L"standby and system cache", cachePct,
             RGB(148, 163, 184));

    int bTop = top + cardH + S(14);
    int bH = S(200);
    RECT breakdown = R(cx, bTop, cx + cw, bTop + bH);
    Card(dc, breakdown, false);
    TextAt(dc, L"Where memory goes", breakdown.left + S(22), breakdown.top + S(14),
           g_fonts.h3, theme::text);

    swprintf(buf, 256, L"%ls processes  -  %ls threads  -  %ls handles",
             FormatCount(mem.processCount).c_str(), FormatCount(mem.threadCount).c_str(),
             FormatCount(mem.handleCount).c_str());
    TextIn(dc, buf,
           R(breakdown.left + S(200), breakdown.top + S(16), breakdown.right - S(22),
             breakdown.top + S(36)),
           g_fonts.tiny, theme::faint, DT_RIGHT | DT_TOP | DT_SINGLELINE);

    RECT bar = R(breakdown.left + S(22), breakdown.top + S(44), breakdown.right - S(22),
                 breakdown.top + S(60));
    FillRound(dc, bar, 4.0f * g_scale, theme::active);
    int innerW = bar.right - bar.left;
    double totalUnits = (double)std::max<uint64_t>(mem.total, 1);
    int usedW = (int)std::lround((double)innerW * (double)used / totalUnits);
    int cacheW = (int)std::lround((double)innerW * (double)mem.cached / totalUnits);
    usedW = std::clamp(usedW, 0, innerW);
    cacheW = std::clamp(cacheW, 0, innerW - usedW);
    if (usedW > 0)
        FillRect(dc, R(bar.left, bar.top, bar.left + usedW, bar.bottom), theme::accent);
    if (cacheW > 0)
        FillRect(dc, R(bar.left + usedW, bar.top, bar.left + usedW + cacheW, bar.bottom),
                 RGB(96, 165, 250));
    StrokeRound(dc, bar, 4.0f * g_scale, theme::cardEdge, 1.0f);

    int lx = bar.left;
    auto legend = [&](const wchar_t* label, uint64_t bytes, COLORREF colour) {
        FillRound(dc, R(lx, bar.bottom + S(13), lx + S(10), bar.bottom + S(23)),
                  3.0f * g_scale, colour);
        std::wstring text = std::wstring(label) + L"  " + FormatBytes(bytes);
        TextAt(dc, text, lx + S(16), bar.bottom + S(11), g_fonts.small, theme::muted);
        lx += S(16) + TextWidth(dc, text, g_fonts.small) + S(26);
    };
    legend(L"In use", used, theme::accent);
    legend(L"Cached", mem.cached, RGB(96, 165, 250));
    legend(L"Free", freeBytes, RGB(71, 85, 105));

    int detailTop = bar.bottom + S(36);
    int colW = (cw - S(44)) / 2;
    int rowStep = S(25);
    auto kv = [&](int col, int row, const wchar_t* label, const std::wstring& value) {
        int x = breakdown.left + S(22) + col * colW;
        int y = detailTop + row * rowStep;
        TextAt(dc, label, x, y, g_fonts.small, theme::faint);
        RECT vr = R(x + S(146), y - S(2), x + colW - S(20), y + S(20));
        TextIn(dc, Ellipsize(dc, value, g_fonts.base, vr.right - vr.left), vr, g_fonts.base,
               theme::text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    kv(0, 0, L"Installed", FormatBytes(mem.total));
    kv(0, 1, L"In use", FormatBytes(used));
    kv(0, 2, L"Available", FormatBytes(mem.avail));
    kv(0, 3, L"Committed", FormatBytes(mem.committed));
    kv(1, 0, L"Commit limit", FormatBytes(mem.commitLimit));
    kv(1, 1, L"Cached", FormatBytes(mem.cached));
    kv(1, 2, L"Paged pool", FormatBytes(mem.kernelPaged));
    kv(1, 3, L"Non-paged pool", FormatBytes(mem.kernelNonPaged));

    // Smart RAM's options, folded into the page that already does its cleaning.
    int optTop = breakdown.bottom + S(14);
    int optH = S(84);
    RECT options = R(cx, optTop, cx + cw, optTop + optH);
    Card(dc, options, false);
    TextAt(dc, L"Automatic clean", options.left + S(22), options.top + S(12), g_fonts.h3,
           theme::text);
    TextAt(dc, L"Clean without pressing a button while the suite is open", options.left + S(184),
           options.top + S(14), g_fonts.tiny, theme::faint);

    static const wchar_t* kAutoLabels[] = {L"Off", L"65%", L"75%", L"85%", L"95%"};
    static const int kAutoValues[] = {0, 65, 75, 85, 95};
    int autoIndex = 0;
    if (st.memPrefs.autoClean) {
        int best = 1;
        for (int i = 2; i < 5; ++i) {
            if (std::abs(kAutoValues[i] - st.memPrefs.threshold) <
                std::abs(kAutoValues[best] - st.memPrefs.threshold))
                best = i;
        }
        autoIndex = best;
    }
    int chipH = S(30);
    int chipY = options.top + S(42);
    ChipRow(dc, hits, options.left + S(22), chipY, HitMemAutoBase, kAutoLabels, 5, autoIndex,
            chipH);

    int clipW = S(158);
    RECT clip = R(options.right - S(22) - clipW, chipY, options.right - S(22), chipY + chipH);
    ChipButton(dc, clip, st.memPrefs.clearClipboard ? L"Clipboard: on" : L"Clipboard: off",
               st.memPrefs.clearClipboard, st.hoverId == HitMemClipboard);
    RecordHit(hits, clip, HitMemClipboard);
    int idleW = S(120);
    RECT idle = R(clip.left - S(8) - idleW, chipY, clip.left - S(8), chipY + chipH);
    ChipButton(dc, idle, st.memPrefs.idleClean ? L"Idle: on" : L"Idle: off",
               st.memPrefs.idleClean, st.hoverId == HitMemIdle);
    RecordHit(hits, idle, HitMemIdle);

    int procTop = options.bottom + S(14);
    int procBottom = h - S(60);
    RECT procs = R(cx, procTop, cx + cw, procBottom);
    if (procs.bottom > procs.top + S(60)) {
        Card(dc, procs, false);
        TextAt(dc, L"Top memory users", procs.left + S(22), procs.top + S(14), g_fonts.h3,
               theme::text);
        TextIn(dc, L"working set, largest first",
               R(procs.left + S(200), procs.top + S(16), procs.right - S(22), procs.top + S(36)),
               g_fonts.tiny, theme::faint, DT_RIGHT | DT_TOP | DT_SINGLELINE);

        int rowsTop = procs.top + S(42);
        int rowH = S(40);
        int maxRows = std::max(0, ((int)procs.bottom - S(10) - rowsTop) / rowH);
        if (st.memProcs.empty()) {
            TextAt(dc, L"No process data available.", procs.left + S(22), rowsTop + S(6),
                   g_fonts.small, theme::muted);
        }
        uint64_t maxWs = st.memProcs.empty() ? 1 : std::max<uint64_t>(st.memProcs[0].workingSet, 1);
        int saved = SaveDC(dc);
        IntersectClipRect(dc, procs.left, procs.top, procs.right, procs.bottom);
        for (int i = 0; i < (int)st.memProcs.size() && i < maxRows; ++i) {
            const ProcMem& p = st.memProcs[i];
            int ry = rowsTop + i * rowH;
            if (i > 0) HLine(dc, procs.left + S(22), procs.right - S(22), ry - S(1), theme::cardEdge);
            TextAt(dc, Ellipsize(dc, p.name, g_fonts.h3, cw - S(260)), procs.left + S(22),
                   ry + S(1), g_fonts.h3, theme::text);
            swprintf(buf, 256, L"PID %lu  -  %ls private", (unsigned long)p.pid,
                     FormatBytes(p.privateBytes).c_str());
            TextAt(dc, buf, procs.left + S(22), ry + S(18), g_fonts.tiny, theme::faint);
            RECT vr = R(procs.right - S(190), ry + S(6), procs.right - S(22), ry + S(28));
            TextIn(dc, FormatBytes(p.workingSet), vr, g_fonts.base, theme::text,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            RECT track = R(procs.left + S(22), ry + rowH - S(6), procs.right - S(22),
                           ry + rowH - S(2));
            FillRound(dc, track, 3.0f * g_scale, theme::active);
            double wsPct = 100.0 * (double)p.workingSet / (double)maxWs;
            Progress(dc, track, wsPct,
                     p.workingSet * 3 > mem.total ? theme::warn : RGB(96, 165, 250));
        }
        RestoreDC(dc, saved);
    }

    if (!st.memMessage.empty()) {
        TextIn(dc, st.memMessage, R(cx, h - S(58), cx + cw, h - S(34)), g_fonts.small,
               st.memMessageIsError ? theme::danger : theme::accent,
               DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    TextAt(dc, st.memMessage.empty()
                    ? L"Regular clean trims working sets. Deep clean also purges the standby "
                      L"cache and needs administrator rights. Neither closes a program."
                    : L"Neither clean closes a program or touches the page file.",
           cx, h - S(28), g_fonts.tiny, theme::faint);
}

// ---------------------------------------------------------------- startup
static void PaintStartup(HDC dc, int cx, int cw, int h, AppState& st, std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"Startup",
           L"Click an item to turn it off or back on. Every change can be undone here.");

    int by = S(112);
    int btnH = S(38);
    RECT refresh = R(cx, by, cx + S(110), by + btnH);
    Button(dc, refresh, L"Refresh", false, st.hoverId == HitStartupRefresh);
    RecordHit(hits, refresh, HitStartupRefresh);

    int enabled = 0, disabled = 0;
    for (const auto& item : st.startup) (item.enabled ? enabled : disabled)++;
    wchar_t buf[160];
    swprintf(buf, 160, L"%d enabled  -  %d disabled", enabled, disabled);
    TextIn(dc, buf, R(cx + S(120), by, cx + cw, by + btnH), g_fonts.small, theme::muted,
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    int listTop = by + S(56);
    if (!st.startupMessage.empty()) {
        RECT msg = R(cx, listTop - S(6), cx + cw, listTop + S(24));
        TextIn(dc, st.startupMessage, msg, g_fonts.small,
               st.startupMessageIsError ? theme::danger : theme::accent,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        listTop += S(32);
    }

    int listBottom = h - S(62);
    int rowH = S(72), gap = S(10);
    int listH = listBottom - listTop;
    int totalH = (int)st.startup.size() * (rowH + gap);
    int maxScroll = std::max(0, totalH - listH - gap);
    st.scrollStartup = std::clamp(st.scrollStartup, 0, maxScroll);

    if (st.startup.empty()) {
        int cyy = listTop + listH / 2 - S(24);
        TextIn(dc, L"No startup entries found.", R(cx, cyy, cx + cw, cyy + S(30)), g_fonts.h2,
               theme::muted, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, cx, listTop, cx + cw, listBottom);
    for (size_t i = 0; i < st.startup.size(); ++i) {
        const StartupItem& item = st.startup[i];
        int ry = listTop - st.scrollStartup + (int)i * (rowH + gap);
        if (ry + rowH < listTop - S(4) || ry > listBottom) continue;
        RECT rr = R(cx, ry, cx + cw, ry + rowH);
        int id = HitStartupRowBase + (int)i;
        Card(dc, rr, st.hoverId == id);

        int textLeft = rr.left + S(22);
        int nameWidth = TextWidth(dc, item.name, g_fonts.h3);
        int nameRoom = rr.right - S(140) - textLeft;
        TextIn(dc, Ellipsize(dc, item.name, g_fonts.h3, nameRoom),
               R(textLeft, ry + S(9), textLeft + std::max(nameRoom, S(40)), ry + S(30)),
               g_fonts.h3, item.enabled ? theme::text : theme::muted,
               DT_LEFT | DT_TOP | DT_SINGLELINE);

        if (nameWidth + S(78) < nameRoom) {
            int pillX = textLeft + nameWidth + S(10);
            if (item.systemItem)
                Pill(dc, pillX, ry + S(10), L"system", RGB(48, 40, 22), theme::warn, g_fonts.tiny);
            else
                Pill(dc, pillX, ry + S(10), L"you", theme::active, theme::muted, g_fonts.tiny);
        }

        TextIn(dc, Ellipsize(dc, item.command, g_fonts.monoSmall, cw - S(240)),
               R(textLeft, ry + S(32), rr.right - S(130), ry + S(50)), g_fonts.monoSmall,
               theme::faint, DT_LEFT | DT_TOP | DT_SINGLELINE);

        RECT stateR = R(rr.right - S(96), ry + S(24), rr.right - S(20), ry + S(48));
        if (item.enabled) {
            FillRound(dc, stateR, 12.0f * g_scale, theme::accentSoft);
            TextIn(dc, L"On", stateR, g_fonts.small, theme::accent,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            FillRound(dc, stateR, 12.0f * g_scale, theme::active);
            TextIn(dc, L"Off", stateR, g_fonts.small, theme::muted,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        TextIn(dc, item.locationLabel, R(textLeft, ry + S(51), rr.right - S(130), ry + S(68)),
               g_fonts.tiny, theme::faint, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        RecordHit(hits, rr, id);
    }
    RestoreDC(dc, saved);

    TextAt(dc, L"Disabled entries are parked in your own settings key and restored exactly as they "
                L"were. System entries need WinSuite to run as administrator.",
           cx, h - S(44), g_fonts.tiny, theme::faint);
}

// -------------------------------------------------------------- deactivate
static const wchar_t* StartTypeName(DWORD start) {
    switch (start) {
        case SERVICE_AUTO_START: return L"Automatic";
        case SERVICE_DEMAND_START: return L"Manual";
        case SERVICE_BOOT_START: return L"Boot";
        case SERVICE_SYSTEM_START: return L"System";
        case SERVICE_DISABLED: return L"Disabled";
        default: return L"Unknown";
    }
}

static void PaintDeactivate(HDC dc, int cx, int cw, int h, AppState& st,
                            std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"Deactivate",
           L"Turn off services that start with Windows and are not part of Windows. "
           L"Everything here can be switched back on.");

    int by = S(112);
    int btnH = S(38);
    RECT refresh = R(cx, by, cx + S(110), by + btnH);
    Button(dc, refresh, L"Refresh", false, st.hoverId == HitDeactivateRefresh);
    RecordHit(hits, refresh, HitDeactivateRefresh);

    int on = 0, off = 0;
    for (const auto& service : st.services) {
        if (service.deactivated)
            ++off;
        else
            ++on;
    }

    RECT restore = R(cx + S(122), by, cx + S(122) + S(150), by + btnH);
    Button(dc, restore, L"Reactivate all", false, st.hoverId == HitDeactivateRestoreAll,
           off > 0);
    RecordHit(hits, restore, HitDeactivateRestoreAll, off > 0);

    wchar_t buf[200];
    swprintf(buf, 200, L"%d on  -  %d deactivated", on, off);
    TextIn(dc, buf, R(cx + S(290), by, cx + cw, by + btnH), g_fonts.small, theme::muted,
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    int listTop = by + S(56);
    if (!st.deactivateMessage.empty()) {
        RECT msg = R(cx, listTop - S(6), cx + cw, listTop + S(24));
        TextIn(dc, st.deactivateMessage, msg, g_fonts.small,
               st.deactivateMessageIsError ? theme::danger : theme::accent,
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        listTop += S(32);
    }

    int listBottom = h - S(62);
    int rowH = S(88), gap = S(10);
    int listH = listBottom - listTop;
    int totalH = (int)st.services.size() * (rowH + gap);
    int maxScroll = std::max(0, totalH - listH - gap);
    st.scrollDeactivate = std::clamp(st.scrollDeactivate, 0, maxScroll);

    if (st.services.empty()) {
        int emptyY = listTop + listH / 2 - S(24);
        TextIn(dc, L"No third-party services start with Windows on this PC.",
               R(cx, emptyY, cx + cw, emptyY + S(30)), g_fonts.h2, theme::muted,
               DT_CENTER | DT_TOP | DT_SINGLELINE);
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, cx, listTop, cx + cw, listBottom);
    for (size_t i = 0; i < st.services.size(); ++i) {
        const ServiceEntry& service = st.services[i];
        int ry = listTop - st.scrollDeactivate + (int)i * (rowH + gap);
        if (ry + rowH < listTop - S(4) || ry > listBottom) continue;
        RECT rr = R(cx, ry, cx + cw, ry + rowH);
        int id = HitDeactivateRowBase + (int)i;
        Card(dc, rr, st.hoverId == id);

        int textLeft = rr.left + S(22);
        int nameRoom = rr.right - S(150) - textLeft;
        int nameWidth = TextWidth(dc, service.display, g_fonts.h3);
        TextIn(dc, Ellipsize(dc, service.display, g_fonts.h3, nameRoom),
               R(textLeft, ry + S(9), textLeft + std::max(nameRoom, S(40)), ry + S(31)),
               g_fonts.h3, service.deactivated ? theme::muted : theme::text,
               DT_LEFT | DT_TOP | DT_SINGLELINE);

        if (nameWidth + S(100) < nameRoom) {
            int pillX = textLeft + nameWidth + S(10);
            if (service.state == SERVICE_RUNNING)
                Pill(dc, pillX, ry + S(10), L"running", theme::active, theme::muted,
                     g_fonts.tiny);
            else if (service.state == SERVICE_STOPPED)
                Pill(dc, pillX, ry + S(10), L"stopped", theme::active, theme::faint,
                     g_fonts.tiny);
        }

        TextIn(dc, Ellipsize(dc, service.name + (service.imagePath.empty()
                                                     ? std::wstring()
                                                     : L"  " + service.imagePath),
                             g_fonts.monoSmall, cw - S(250)),
               R(textLeft, ry + S(35), rr.right - S(130), ry + S(53)), g_fonts.monoSmall,
               theme::faint, DT_LEFT | DT_TOP | DT_SINGLELINE);

        std::wstring detail;
        if (service.deactivated)
            detail = std::wstring(L"Disabled by WinSuite - restores to ") +
                     StartTypeName(service.originalStart);
        else
            detail = L"Automatic start - not part of Windows";
        if (!service.note.empty()) detail += L" - " + service.note;
        TextIn(dc, detail, R(textLeft, ry + S(59), rr.right - S(130), ry + S(77)),
               g_fonts.tiny, theme::faint, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT stateR = R(rr.right - S(96), ry + S(30), rr.right - S(20), ry + S(54));
        if (service.deactivated) {
            FillRound(dc, stateR, 12.0f * g_scale, theme::active);
            TextIn(dc, L"Off", stateR, g_fonts.small, theme::muted,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            FillRound(dc, stateR, 12.0f * g_scale, theme::accentSoft);
            TextIn(dc, L"On", stateR, g_fonts.small, theme::accent,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        RecordHit(hits, rr, id, service.canDeactivate || service.deactivated);
    }
    RestoreDC(dc, saved);

    if (!st.elevated) {
        TextAt(dc, L"Reading the list needs no special rights. Switching a service off needs "
                    L"administrator rights - WinSuite offers to restart elevated when you click.",
               cx, h - S(44), g_fonts.tiny, theme::faint);
    } else {
        TextAt(dc, L"Deactivating stops the service and sets it to Disabled. Reactivate puts back "
                    L"the start type it had before.",
               cx, h - S(44), g_fonts.tiny, theme::faint);
    }
}

// ----------------------------------------------------------------- system
static void SpecRow(HDC dc, int x, int y, int width, int labelW, const wchar_t* label,
                    const std::wstring& value, COLORREF valueColor = theme::text) {
    TextAt(dc, label, x, y + S(3), g_fonts.small, theme::faint);
    RECT vr = R(x + labelW, y, x + width, y + S(22));
    TextIn(dc, Ellipsize(dc, value, g_fonts.base, vr.right - vr.left), vr, g_fonts.base,
           valueColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void PaintSystem(HDC dc, int cx, int cw, int h, AppState& st, std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"System",
           L"Everything WinSuite can read about this machine, on one page.");

    int by = S(112);
    int btnH = S(38);
    RECT copyBtn = R(cx + cw - S(266), by, cx + cw - S(126), by + btnH);
    Button(dc, copyBtn, L"Copy report", false, st.hoverId == HitCopyReport);
    RecordHit(hits, copyBtn, HitCopyReport);

    RECT refresh = R(cx + cw - S(114), by, cx + cw, by + btnH);
    Button(dc, refresh, L"Refresh", false, st.hoverId == HitRefresh);
    RecordHit(hits, refresh, HitRefresh);

    const SysInfo& sys = st.sys;
    int specsTop = by + S(58);
    int rowStep = S(36);
    int pad = S(22);
    int labelW = S(158);
    int memoryRows = 9;
    int specsH = S(20) + memoryRows * rowStep + S(12);
    RECT specs = R(cx, specsTop, cx + cw, specsTop + specsH);
    Card(dc, specs, false);
    TextAt(dc, L"This machine", specs.left + pad, specs.top + S(16), g_fonts.h3, theme::text);

    int ry = specs.top + S(50);
    wchar_t buf[256];
    SpecRow(dc, specs.left + pad, ry, cw - pad * 2, labelW, L"Computer", sys.computer);
    ry += rowStep;
    SpecRow(dc, specs.left + pad, ry, cw - pad * 2, labelW, L"Signed in", sys.user);
    ry += rowStep;
    SpecRow(dc, specs.left + pad, ry, cw - pad * 2, labelW, L"Windows",
            sys.osName + L"  (" + sys.osBuild + L")");
    ry += rowStep;
    SpecRow(dc, specs.left + pad, ry, cw - pad * 2, labelW, L"Architecture", sys.arch);
    ry += rowStep;
    SpecRow(dc, specs.left + pad, ry, cw - pad * 2, labelW, L"Processor", sys.cpuName);
    ry += rowStep;
    swprintf(buf, 256, L"%u physical cores / %u logical threads", sys.physicalCpus,
             sys.logicalCpus);
    SpecRow(dc, specs.left + pad, ry, cw - pad * 2, labelW, L"Cores", buf);
    ry += rowStep;
    swprintf(buf, 256, L"%d %%", (int)std::lround(st.cpu.percent));
    SpecRow(dc, specs.left + pad, ry, cw - pad * 2, labelW, L"Current CPU load", buf);
    ry += rowStep;
    uint64_t used = sys.ramTotal > sys.ramAvail ? sys.ramTotal - sys.ramAvail : 0;
    SpecRow(dc, specs.left + pad, ry, cw - pad * 2, labelW, L"Memory",
            FormatBytes(used) + L" used of " + FormatBytes(sys.ramTotal));
    ry += rowStep;
    SpecRow(dc, specs.left + pad, ry, cw - pad * 2, labelW, L"Uptime",
            FormatDuration(sys.uptimeSeconds));

    // Every mounted volume, not just fixed disks: removable, optical, network and RAM
    // disks are included, with the bus and model Windows reports for the device. The list
    // scrolls when it is longer than the space left, so a machine with a stack of drives
    // still shows every one of them instead of cutting the list off.
    int storeTop = specs.bottom + S(16);
    int storeBottom = h - S(70);
    int rowH = S(56);
    int rowsTop = storeTop + S(44);
    int total = (int)st.storage.size();
    int maxRows = std::max(1, (storeBottom - S(8) - rowsTop) / rowH);
    bool scrollable = total > maxRows;
    // The card is only ever as tall as whole rows, so no volume is left half drawn at the
    // bottom edge; the rest are one wheel notch away.
    int cardH = S(44) + std::max(std::min(total, maxRows), 1) * rowH + S(8);
    RECT storageCard = R(cx, storeTop, cx + cw, std::min(storeTop + cardH, storeBottom));
    Card(dc, storageCard, false);
    TextAt(dc, L"Storage devices", storageCard.left + pad, storageCard.top + S(14),
           g_fonts.h3, theme::text);

    if (scrollable)
        swprintf(buf, 256, L"%d volumes  -  scroll for all", total);
    else
        swprintf(buf, 256, L"%d volume%ls", total, total == 1 ? L"" : L"s");
    TextIn(dc, buf,
           R(storageCard.left + S(240), storageCard.top + S(16), storageCard.right - pad,
             storageCard.top + S(36)),
           g_fonts.tiny, theme::faint, DT_RIGHT | DT_TOP | DT_SINGLELINE);

    if (st.storage.empty())
        TextAt(dc, L"No storage volumes reported.", storageCard.left + pad,
               storageCard.top + S(44), g_fonts.small, theme::muted);

    // The rows are drawn inside the card, shifted by the wheel: what does not fit is a
    // scroll away rather than dropped.
    int rowAreaBottom = (int)storageCard.bottom - S(6);
    int viewH = std::max(0, rowAreaBottom - rowsTop);
    int shownRows = scrollable ? maxRows : total;
    // Scrolling snaps to a row, so a notch moves the list by one whole volume.
    int maxScroll = scrollable ? std::max(0, (total - maxRows) * rowH) : 0;
    st.scrollSystem = std::clamp(st.scrollSystem / rowH * rowH, 0, maxScroll);

    int firstRow = st.scrollSystem / rowH;
    int savedRows = SaveDC(dc);
    IntersectClipRect(dc, cx, rowsTop, cx + cw, rowAreaBottom);
    for (int i = firstRow; i < std::min(total, firstRow + shownRows); ++i) {
        int dy = rowsTop + (i - firstRow) * rowH;
        const StorageDevice& d = st.storage[(size_t)i];
        std::wstring title = d.root;
        if (!d.label.empty()) title += L"  " + d.label;
        TextAt(dc, Ellipsize(dc, title, g_fonts.h3, cw / 2), storageCard.left + pad, dy + S(4),
               g_fonts.h3, d.accessible ? theme::text : theme::muted);

        std::wstring capacity =
            d.total ? FormatBytes(d.freeBytes) + L" free of " + FormatBytes(d.total)
                    : (d.accessible ? std::wstring(L"capacity unavailable") : std::wstring(L"no media"));
        TextIn(dc, capacity,
               R(storageCard.left + S(260), dy + S(6), storageCard.right - pad, dy + S(26)),
               g_fonts.small, theme::muted, DT_RIGHT | DT_TOP | DT_SINGLELINE);

        std::wstring detail = d.typeName;
        if (!d.fileSystem.empty()) detail += L"  -  " + d.fileSystem;
        if (!d.bus.empty()) detail += L"  -  " + d.bus;
        if (!d.model.empty()) detail += L"  -  " + d.model;
        TextIn(dc, Ellipsize(dc, detail, g_fonts.tiny, cw - pad * 2 - S(30)),
               R(storageCard.left + pad, dy + S(27), storageCard.right - pad, dy + S(43)),
               g_fonts.tiny, theme::faint, DT_LEFT | DT_TOP | DT_SINGLELINE);

        if (d.total) {
            double pct = 100.0 * (double)(d.total - d.freeBytes) / (double)d.total;
            RECT bar = R(storageCard.left + pad, dy + rowH - S(8), storageCard.right - pad,
                         dy + rowH - S(3));
            FillRound(dc, bar, 2.5f * g_scale, theme::active);
            Progress(dc, bar, pct, pct > 90 ? theme::danger : theme::accent);
        }
    }
    RestoreDC(dc, savedRows);

    // A slim bar inside the card, drawn only when there is more than one screenful.
    if (maxScroll > 0) {
        int barH = std::max(S(30), viewH * shownRows / std::max(1, total));
        int barY = rowsTop + (viewH - barH) * st.scrollSystem / std::max(1, maxScroll);
        FillRound(dc, R(storageCard.right - S(10), barY, storageCard.right - S(6), barY + barH),
                  2.0f * g_scale, theme::active);
    }

    if (!st.systemMessage.empty()) {
        TextAt(dc, st.systemMessage, cx, h - S(44), g_fonts.small, theme::accent);
    } else {
        TextAt(dc, L"Report data stays on this PC. Copy report puts it on the clipboard for you.",
               cx, h - S(44), g_fonts.tiny, theme::faint);
    }
}

// ---------------------------------------------------------------- storage
// One row per local volume, with the mode Windows' defragmenter will use on
// it: /D on a mechanical disk and /L (retrim) on a solid-state drive. A volume
// whose seek-penalty query did not answer is shown but left alone, because
// running the wrong mode is worse than doing nothing.
static void PaintStorage(HDC dc, int cx, int cw, int h, AppState& st, std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"Storage",
           L"Defragment mechanical disks and retrim solid-state drives.");

    int by = S(112);
    int actionsRight = cx + cw;
    if (st.storageBusy) {
        // While defrag.exe runs, Cancel is the only action that can do anything; Refresh
        // and every row button below are drawn disabled and record disabled hits.
        RECT cancel = R(actionsRight - S(114), by, actionsRight, by + S(38));
        Button(dc, cancel, L"Cancel", false, st.hoverId == HitStorageCancel, true);
        RecordHit(hits, cancel, HitStorageCancel);
        actionsRight = cancel.left - S(10);
    }
    RECT refresh = R(actionsRight - S(114), by, actionsRight, by + S(38));
    Button(dc, refresh, L"Refresh", false, st.hoverId == HitStorageRefresh, !st.storageBusy);
    RecordHit(hits, refresh, HitStorageRefresh, !st.storageBusy);

    std::vector<int> rows;
    for (int i = 0; i < (int)st.storage.size(); ++i) {
        const StorageDevice& d = st.storage[(size_t)i];
        if (!d.total) continue;
        if (d.typeName == L"Network" || d.typeName == L"Optical") continue;
        rows.push_back(i);
    }

    int pad = S(22);
    int cardTop = by + S(58);
    int cardBottom = h - S(70);
    int rowH = S(78);
    int listTop = cardTop + S(44);
    int total = (int)rows.size();
    int maxRows = std::max(1, (cardBottom - S(8) - listTop) / rowH);
    bool scrollable = total > maxRows;
    int cardH = S(44) + std::max(std::min(total, maxRows), 1) * rowH + S(8);
    RECT card = R(cx, cardTop, cx + cw, std::min(cardTop + cardH, cardBottom));
    Card(dc, card, false);
    TextAt(dc, L"Volumes", card.left + pad, card.top + S(14), g_fonts.h3, theme::text);

    wchar_t count[128];
    swprintf(count, 128, L"%d local volume%ls", total, total == 1 ? L"" : L"s");
    TextIn(dc, count,
           R(card.left + S(240), card.top + S(16), card.right - pad, card.top + S(36)),
           g_fonts.tiny, theme::faint, DT_RIGHT | DT_TOP | DT_SINGLELINE);

    if (rows.empty()) {
        TextAt(dc, st.storageBusy ? L"Refreshing volumes..."
                                  : L"No local volume is available to optimize.",
               card.left + pad, card.top + S(44), g_fonts.small, theme::muted);
    }

    const int viewH = std::max(0, (int)card.bottom - S(6) - listTop);
    const int maxScroll = scrollable ? std::max(0, total * rowH - viewH) : 0;
    st.scrollStorage = std::clamp(st.scrollStorage, 0, maxScroll);
    st.storageScrollMax = maxScroll;
    st.storagePageStep = std::max(rowH, maxRows * rowH);

    // The thumb drags the list by pixels, so the rows are laid out from the exact offset
    // (not a row index) and their hit rects are recorded from the same dy below.
    const int first = st.scrollStorage / rowH;
    const int remainder = st.scrollStorage % rowH;
    const int shown = scrollable ? (viewH + remainder + rowH - 1) / rowH : total;
    const RECT viewport = R(card.left, listTop, card.right, (int)card.bottom - S(6));
    int saved = SaveDC(dc);
    IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);
    for (int n = first; n < std::min(total, first + shown); ++n) {
        const StorageDevice& d = st.storage[(size_t)rows[(size_t)n]];
        int dy = listTop - remainder + (n - first) * rowH;
        std::wstring title = d.root;
        if (!d.label.empty()) title += L"  " + d.label;
        TextAt(dc, Ellipsize(dc, title, g_fonts.h3, cw - S(470)), card.left + pad, dy + S(4),
               g_fonts.h3, d.accessible ? theme::text : theme::muted);

        std::wstring capacity =
            d.total ? FormatBytes(d.freeBytes) + L" free of " + FormatBytes(d.total)
                    : std::wstring(L"capacity unavailable");
        TextIn(dc, capacity,
               R(card.left + S(300), dy + S(6), card.right - pad - S(124), dy + S(24)),
               g_fonts.tiny, theme::muted, DT_RIGHT | DT_TOP | DT_SINGLELINE);

        std::wstring media = !d.mediaKnown ? std::wstring(L"media type unknown")
                            : d.ssd ? std::wstring(L"solid-state (TRIM)")
                                    : std::wstring(L"mechanical (defragment)");
        std::wstring detail = media;
        if (!d.typeName.empty()) detail += L"  -  " + d.typeName;
        if (!d.fileSystem.empty()) detail += L"  -  " + d.fileSystem;
        if (!d.bus.empty()) detail += L"  -  " + d.bus;
        if (!d.model.empty()) detail += L"  -  " + d.model;
        TextIn(dc, Ellipsize(dc, detail, g_fonts.tiny, cw - S(470)),
               R(card.left + pad, dy + S(28), card.right - pad - S(124), dy + S(46)),
               g_fonts.tiny, d.mediaKnown ? theme::muted : theme::faint,
               DT_LEFT | DT_TOP | DT_SINGLELINE);

        // The id is the drive letter, not the row's place in st.storage: a refresh can
        // reorder the volumes, and a press must never land on a different disk.
        int slot = d.root.empty() ? -1 : (int)(std::towupper(d.root[0]) - L'A');
        if (slot < 0 || slot >= 26) continue;
        RECT btn = R(card.right - pad - S(112), dy + S(22), card.right - pad, dy + S(52));
        const std::wstring refusal = StorageOptimizeRefusal(d);
        bool enabled = refusal.empty() && !st.storageBusy;
        std::wstring action = !refusal.empty() ? L"Unavailable"
                              : d.ssd          ? L"TRIM"
                                               : L"Defragment";
        Button(dc, btn, action, false, st.hoverId == HitStorageRowBase + slot, enabled);
        // A row half-scrolled into the viewport must not answer a click in the hidden half.
        RECT visibleBtn = {};
        if (IntersectRect(&visibleBtn, &btn, &viewport))
            RecordHit(hits, visibleBtn, HitStorageRowBase + slot, enabled);

        if (d.total) {
            double pct = 100.0 * (double)(d.total - d.freeBytes) / (double)d.total;
            RECT bar = R(card.left + pad, dy + rowH - S(10), card.right - pad, dy + rowH - S(6));
            FillRound(dc, bar, 2.0f * g_scale, theme::active);
            Progress(dc, bar, pct, pct > 90 ? theme::danger : theme::accent);
        }
    }
    RestoreDC(dc, saved);

    if (maxScroll > 0) {
        const int trackH = viewH;
        const int thumbH =
            std::max(S(30), (int)((long long)trackH * viewH / std::max(1, total * rowH)));
        const int travel = std::max(0, trackH - thumbH);
        const int thumbY = listTop + travel * st.scrollStorage / maxScroll;
        st.storageTrack = R(card.right - S(10), listTop, card.right - S(4), listTop + trackH);
        st.storageThumb = R(st.storageTrack.left, thumbY, st.storageTrack.right, thumbY + thumbH);
        FillRound(dc, st.storageTrack, 3.0f * g_scale, theme::active);
        FillRound(dc, st.storageThumb, 3.0f * g_scale, theme::accent);
    } else {
        st.storageTrack = {};
        st.storageThumb = {};
    }

    if (st.storageBusy) {
        RECT bar = R(cx, h - S(58), cx + cw, h - S(52));
        FillRound(dc, bar, S(3), theme::active);
        if (st.storagePercent >= 0)
            Progress(dc, bar, st.storagePercent, theme::accent);
    }
    if (!st.storageMessage.empty())
        TextAt(dc, st.storageMessage, cx, h - S(44), g_fonts.small,
               st.storageMessageIsError ? theme::danger : theme::accent);
    else
        TextAt(dc, L"Both modes use Windows' own defragmenter; no other file is written.", cx,
               h - S(44), g_fonts.tiny, theme::faint);
}

// ---------------------------------------------------------------- services
static bool RuleForExe(const std::vector<TaskRule>& rules, const std::wstring& exe,
                       size_t* index = nullptr) {
    for (size_t i = 0; i < rules.size(); ++i) {
        if (_wcsicmp(rules[i].exe.c_str(), exe.c_str()) != 0) continue;
        if (index) *index = i;
        return true;
    }
    return false;
}

// ------------------------------------------------------------ list filters
// A search box narrows a long list by name. Matching ignores case and looks for the typed
// text anywhere in the name, so "print" finds "Print Spooler".
static bool NameContains(const std::wstring& name, const std::wstring& filter) {
    if (filter.empty()) return true;
    if (name.size() < filter.size()) return false;
    for (size_t start = 0; start + filter.size() <= name.size(); ++start) {
        size_t i = 0;
        for (; i < filter.size(); ++i) {
            if (std::towlower(name[start + i]) != std::towlower(filter[i])) break;
        }
        if (i == filter.size()) return true;
    }
    return false;
}

std::vector<int> FilterServiceRows(const std::vector<ServiceRow>& rows,
                                   const std::wstring& filter) {
    std::vector<int> out;
    out.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        if (NameContains(rows[i].display, filter) || NameContains(rows[i].name, filter))
            out.push_back((int)i);
    }
    return out;
}

std::vector<int> FilterTaskRows(const std::vector<ProcRow>& rows,
                                const std::wstring& filter) {
    std::vector<int> out;
    out.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        if (NameContains(rows[i].name, filter)) out.push_back((int)i);
    }
    return out;
}

// Ticks for the bulk buttons. Names and pids are what is stored, so a list that is read
// again in a different order keeps every tick on the row it belongs to.
bool IsServiceChecked(const AppState& st, const std::wstring& name) {
    // Service names are unique without regard to case, so the tick has to be too: a load
    // that reports the same service with different capitals must keep its tick.
    for (const auto& ticked : st.serviceChecked)
        if (!_wcsicmp(ticked.c_str(), name.c_str())) return true;
    return false;
}

bool IsTaskChecked(const AppState& st, DWORD pid, uint64_t created) {
    for (const auto& ticked : st.taskChecked)
        if (ticked.pid == pid && ticked.created == created) return true;
    return false;
}

// The search box both list pages use. The field itself answers the click that puts typing
// into it, and the x at its right end clears the text without leaving the field.
static void InputField(HDC dc, std::vector<Hit>& hits, const RECT& rr, const std::wstring& text,
                        const wchar_t* placeholder, bool focused, int hoverId, int id,
                        int clearId) {
    FillRound(dc, rr, 8.0f * g_scale, RGB(17, 22, 28));
    StrokeRound(dc, rr, 8.0f * g_scale, focused ? theme::accent : theme::cardEdge, 1.0f);
    RecordHit(hits, rr, id);

    int rightPad = text.empty() ? S(12) : S(34);
    RECT textRect = R(rr.left + S(12), rr.top, rr.right - rightPad, rr.bottom);
    if (text.empty()) {
        TextIn(dc, placeholder, textRect, g_fonts.small, theme::faint,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    std::wstring shown = Ellipsize(dc, text, g_fonts.small, textRect.right - textRect.left);
    TextIn(dc, shown, textRect, g_fonts.small, theme::text,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (focused) {
        int caret = textRect.left + TextWidth(dc, shown, g_fonts.small) + S(2);
        FillRect(dc, R(caret, rr.top + S(8), caret + std::max(1, S(1)), rr.bottom - S(8)),
                 theme::accent);
    }

    RECT clear = R(rr.right - S(30), rr.top, rr.right - S(4), rr.bottom);
    TextIn(dc, L"\u00d7", clear, g_fonts.h3, hoverId == clearId ? theme::accent : theme::muted,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RecordHit(hits, clear, clearId);
}

static void PaintServices(HDC dc, int cx, int cw, int h, AppState& st, std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"Services",
           L"Every installed Windows service: its start type, starting and stopping it, and "
           L"creating, editing or removing your own entries.");

    int by = S(112);
    int btnH = S(38);
    RECT refresh = R(cx, by, cx + S(104), by + btnH);
    Button(dc, refresh, L"Refresh", false, st.hoverId == HitSvcRefresh);
    RecordHit(hits, refresh, HitSvcRefresh);

    const ServiceRow* row = nullptr;
    if (st.serviceSelected >= 0 && st.serviceSelected < (int)st.serviceRows.size())
        row = &st.serviceRows[(size_t)st.serviceSelected];
    // With rows ticked, Start and Stop work on every ticked row that needs it; with none
    // ticked they fall back to the highlighted row, the way they always did.
    bool checkedStart = false, checkedStop = false;
    for (const auto& service : st.serviceRows) {
        if (!IsServiceChecked(st, service.name)) continue;
        if (service.running)
            checkedStop = true;
        else
            checkedStart = true;
    }
    bool canStart = checkedStart || (st.serviceChecked.empty() && row && !row->running);
    bool canStop = checkedStop || (st.serviceChecked.empty() && row && row->running);
    bool canChange = row && !row->driver;
    bool thirdParty = row && !row->windows;

    RECT start = R(refresh.right + S(8), by, refresh.right + S(8) + S(92), by + btnH);
    Button(dc, start, L"Start", false, st.hoverId == HitSvcStart, canStart);
    RecordHit(hits, start, HitSvcStart, canStart);

    RECT stop = R(start.right + S(8), by, start.right + S(8) + S(92), by + btnH);
    Button(dc, stop, L"Stop", false, st.hoverId == HitSvcStop, canStop);
    RecordHit(hits, stop, HitSvcStop, canStop);

    // The entry-changing buttons sit on the right, and only light up for entries whose
    // binary is not a Windows component.
    RECT create = R(cx + cw - S(306), by, cx + cw - S(196), by + btnH);
    Button(dc, create, L"New...", false, st.hoverId == HitSvcNew);
    RecordHit(hits, create, HitSvcNew);

    RECT edit = R(create.right + S(8), by, create.right + S(8) + S(84), by + btnH);
    Button(dc, edit, L"Edit...", false, st.hoverId == HitSvcEdit, thirdParty);
    RecordHit(hits, edit, HitSvcEdit, thirdParty);

    RECT del = R(edit.right + S(8), by, edit.right + S(8) + S(84), by + btnH);
    Button(dc, del, L"Delete", false, st.hoverId == HitSvcDelete, thirdParty);
    RecordHit(hits, del, HitSvcDelete, thirdParty);

    // Start type for the selection: four chips, one press each. They stay live whenever
    // anything is ticked, because a filter that hides the highlighted row must not leave
    // the ticked services with no way to be re-typed.
    const bool anyTicked = !st.serviceChecked.empty();
    const bool typeTarget = row != nullptr || anyTicked;
    int chipH = S(28);
    int chipY = by + btnH + S(12);
    TextIn(dc, L"Start type", R(cx, chipY, cx + S(84), chipY + chipH), g_fonts.small,
           theme::muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const wchar_t* types[] = {L"Automatic", L"Automatic (delayed)", L"Manual", L"Disabled"};
    int selectedType = -1;
    if (row) {
        if (row->startType == SERVICE_AUTO_START)
            selectedType = row->delayed ? 1 : 0;
        else if (row->startType == SERVICE_DEMAND_START)
            selectedType = 2;
        else if (row->startType == SERVICE_DISABLED)
            selectedType = 3;
    }
    int x = cx + S(88);
    for (int i = 0; i < 4; ++i) {
        int labelW = TextWidth(dc, types[i], g_fonts.small) + S(26);
        RECT chip = R(x, chipY, x + labelW, chipY + chipH);
        ChipButton(dc, chip, types[i], row != nullptr && selectedType == i,
                   typeTarget && st.hoverId == HitSvcTypeBase + i);
        RecordHit(hits, chip, HitSvcTypeBase + i, typeTarget && canChange);
        x = chip.right + S(6);
    }

    // The list below shows only the entries whose display or service name contains the
    // text in the search box; an empty box shows everything.
    int searchY = chipY + chipH + S(12);
    RECT search = R(cx, searchY, cx + std::min(cw, S(320)), searchY + S(34));
    InputField(dc, hits, search, st.serviceFilter, L"Search services by name",
                st.inputFocus == HitSvcFilter, st.hoverId, HitSvcFilter, HitSvcFilterClear);

    const std::vector<int> visible = FilterServiceRows(st.serviceRows, st.serviceFilter);

    // Bulk tick controls sit at the far end of the search row, where they apply to the
    // rows the filter is showing.
    if (!visible.empty()) {
        int selY = searchY + S(4);
        RECT all = R(cx + cw - S(184), selY, cx + cw - S(100), selY + S(26));
        bool allHover = st.hoverId == HitSvcSelectAll;
        TextIn(dc, L"Select all", all, g_fonts.small,
               allHover ? theme::accent : theme::muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (allHover) HLine(dc, all.left + S(24), all.right, all.bottom - S(2), theme::accent);
        RecordHit(hits, all, HitSvcSelectAll);

        RECT clear = R(cx + cw - S(92), selY, cx + cw, selY + S(26));
        bool clearHover = st.hoverId == HitSvcClearChecked;
        TextIn(dc, L"Clear ticks", clear, g_fonts.small,
               clearHover ? theme::accent : theme::muted,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (clearHover)
            HLine(dc, clear.left + S(22), clear.right, clear.bottom - S(2), theme::accent);
        RecordHit(hits, clear, HitSvcClearChecked);
    }

    int listTop = search.bottom + S(12);
    if (!st.servicesMessage.empty()) {
        RECT banner = R(cx, listTop, cx + cw, listTop + S(34));
        ColorRefMessage(dc, banner, st.servicesMessage, st.servicesMessageIsError);
        listTop += S(42);
    }

    int listBottom = h - S(58);
    int rowH = S(62), gap = S(8);
    // Whole rows only: the clip ends on a row boundary and the wheel moves by whole rows,
    // so the bottom edge never cuts one in half.
    const WholeRowList layout =
        FitWholeRows(listTop, listBottom, rowH, gap, (int)visible.size(), st.scrollServices);
    st.scrollServices = layout.scroll;
    listBottom = layout.bottom;

    if (visible.empty()) {
        if (st.serviceRows.empty()) {
            TextAt(dc, st.servicesLoaded ? L"The service manager returned no services."
                                         : L"Reading the service database...",
                   cx, listTop, g_fonts.small, theme::muted);
        } else {
            TextIn(dc, std::wstring(L"No service name matches \"") + st.serviceFilter +
                          L"\". Clear the box to list all " +
                          std::to_wstring(st.serviceRows.size()) + L" services.",
                   R(cx, listTop, cx + cw, listTop + S(22)), g_fonts.small, theme::muted,
                   DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, cx, listTop, cx + cw, listBottom);
    for (size_t v = 0; v < visible.size(); ++v) {
        int i = visible[v];
        int ry = listTop - st.scrollServices + (int)v * (rowH + gap);
        if (ry + rowH < listTop || ry > listBottom) continue;
        const ServiceRow& s = st.serviceRows[(size_t)i];
        int id = HitSvcRowBase + i;
        bool selected = i == st.serviceSelected;
        bool ticked = IsServiceChecked(st, s.name);
        RECT rr = R(cx, ry, cx + cw, ry + rowH);
        Card(dc, rr, ticked || selected || st.hoverId == id);
        if (ticked || selected)
            FillRound(dc, R(rr.left, rr.top + S(10), rr.left + S(3), rr.bottom - S(10)),
                      1.5f * g_scale, theme::accent);

        // The tick is what the bulk Start/Stop acts on; the card also stays highlighted so
        // the row the start-type chips and Edit/Delete belong to is never in doubt.
        RECT cb = R(rr.left + S(16), rr.top + (rowH - S(20)) / 2, rr.left + S(36),
                    rr.top + (rowH - S(20)) / 2 + S(20));
        TickBox(dc, cb, ticked);

        int textRight = rr.right - S(170);
        TextIn(dc, s.display,
               R(rr.left + S(48), rr.top + S(9), textRight - S(150), rr.top + S(31)),
               g_fonts.h3, theme::text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        int pillX = rr.left + S(48) + TextWidth(dc, s.display, g_fonts.h3) + S(10);
        if (s.windows && pillX + S(70) < textRight) {
            Pill(dc, pillX, rr.top + S(7), L"Windows", RGB(30, 36, 44), theme::muted,
                 g_fonts.tiny);
            pillX += TextWidth(dc, L"Windows", g_fonts.tiny) + S(20);
        }
        if (s.driver && pillX + S(52) < textRight) {
            Pill(dc, pillX, rr.top + S(7), L"driver", RGB(48, 40, 22), theme::warn,
                 g_fonts.tiny);
        }

        std::wstring detail = s.error.empty() ? s.name + L"  " + s.imagePath : s.error;
        TextIn(dc, Ellipsize(dc, detail, g_fonts.monoSmall, textRight - (rr.left + S(48))),
               R(rr.left + S(48), rr.top + S(34), textRight, rr.top + S(54)), g_fonts.monoSmall,
               s.error.empty() ? theme::faint : theme::warn,
               DT_LEFT | DT_TOP | DT_SINGLELINE);

        std::wstring typeText = ServiceStartTypeLabel(s.startType);
        if (s.startType == SERVICE_AUTO_START && s.delayed) typeText = L"Automatic (delayed)";
        TextIn(dc, typeText, R(rr.right - S(160), rr.top + S(10), rr.right - S(20), rr.top + S(30)),
               g_fonts.small, theme::muted, DT_RIGHT | DT_TOP | DT_SINGLELINE);
        TextIn(dc, ServiceStateLabel(s.state),
               R(rr.right - S(160), rr.top + S(32), rr.right - S(20), rr.top + S(52)),
               g_fonts.small, s.running ? theme::accent : theme::faint,
               DT_RIGHT | DT_TOP | DT_SINGLELINE);

        RECT hit = rr;
        hit.top = std::max(hit.top, (LONG)listTop);
        hit.bottom = std::min(hit.bottom, (LONG)listBottom);
        RecordHit(hits, hit, id);
    }
    RestoreDC(dc, saved);

    std::wstring footer =
        st.serviceFilter.empty()
            ? std::to_wstring(st.serviceRows.size()) + L" services"
            : std::to_wstring(visible.size()) + L" of " +
                  std::to_wstring(st.serviceRows.size()) + L" services match \"" +
                  st.serviceFilter + L"\"";
    if (!st.serviceChecked.empty())
        footer += L"  -  " + std::to_wstring(st.serviceChecked.size()) + L" ticked";
    footer += L". Start and Stop act on every ticked row, or on the highlighted row when "
              L"none is ticked. Windows components can be started, stopped and re-typed; "
              L"only entries with a non-Windows binary can be edited or deleted.";
    TextIn(dc, footer, R(cx, h - S(48), cx + cw, h - S(28)), g_fonts.tiny, theme::faint,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ------------------------------------------------------------- Task Manager
static void PaintTasks(HDC dc, int cx, int cw, int h, AppState& st, std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"Task Manager",
           L"Every process on the PC: end it, start a new one, and set the cores and "
           L"priority it runs with - kept for the next boot when you ask.");

    int by = S(112);
    int btnH = S(38);
    RECT refresh = R(cx, by, cx + S(104), by + btnH);
    Button(dc, refresh, L"Refresh", false, st.hoverId == HitTaskRefresh);
    RecordHit(hits, refresh, HitTaskRefresh);

    const ProcRow* row = nullptr;
    if (st.taskSelected >= 0 && st.taskSelected < (int)st.taskRows.size())
        row = &st.taskRows[(size_t)st.taskSelected];

    // End and Force end take every ticked process at once; with nothing ticked they still
    // act on the highlighted row, so a single end is one click as before.
    bool anyCheckedTask = false;
    for (const auto& process : st.taskRows) {
        if (process.pid > 4 && IsTaskChecked(st, process.pid, process.created)) {
            anyCheckedTask = true;
            break;
        }
    }
    bool canEnd = anyCheckedTask || (row != nullptr && row->pid > 4);

    RECT end = R(refresh.right + S(8), by, refresh.right + S(8) + S(104), by + btnH);
    Button(dc, end, L"End task", false, st.hoverId == HitTaskEnd, canEnd);
    RecordHit(hits, end, HitTaskEnd, canEnd);

    RECT force = R(end.right + S(8), by, end.right + S(8) + S(104), by + btnH);
    Button(dc, force, L"Force end", false, st.hoverId == HitTaskForce, canEnd);
    RecordHit(hits, force, HitTaskForce, canEnd);

    RECT newTask = R(cx + cw - S(142), by, cx + cw, by + btnH);
    Button(dc, newTask, L"New task...", false, st.hoverId == HitTaskNew);
    RecordHit(hits, newTask, HitTaskNew);

    // The detail card grows with its chips: a machine with many cores needs several rows,
    // and the Apply buttons stay pinned to the card's right edge instead of sliding off
    // it, where they could not be clicked.
    int cardTop = by + btnH + S(12);
    const int chipH = S(26), chipGap = S(4), blockGap = S(8);
    const int chipsLeft = cx + S(92);
    const int applyW = S(104);
    const int chipsRight = cx + cw - S(20) - applyW - S(12);
    const wchar_t* priorityNames[] = {L"Idle", L"Below normal", L"Normal", L"Above normal",
                                      L"High", L"Real time"};
    const DWORD priorityValues[] = {IDLE_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS,
                                    NORMAL_PRIORITY_CLASS, ABOVE_NORMAL_PRIORITY_CLASS,
                                    HIGH_PRIORITY_CLASS, REALTIME_PRIORITY_CLASS};
    int priorityWidths[6] = {};
    int cpuCount = 0, affinityRows = 0, priorityRows = 0;
    if (row) {
        cpuCount = (int)std::min<unsigned>(st.sys.logicalCpus ? st.sys.logicalCpus : 1, 64);
        const int room = std::max(S(34), chipsRight - chipsLeft);
        const int perRow = std::max(1, (room + chipGap) / (S(34) + chipGap));
        affinityRows = (cpuCount + perRow - 1) / perRow;
        priorityRows = 1;
        int used = 0;
        for (int i = 0; i < 6; ++i) {
            priorityWidths[i] = TextWidth(dc, priorityNames[i], g_fonts.small) + S(22);
            if (used > 0 && used + chipGap + priorityWidths[i] > room) {
                ++priorityRows;
                used = 0;
            }
            used += (used > 0 ? chipGap : 0) + priorityWidths[i];
        }
    }
    const int affinityTop = cardTop + S(44);
    const int priorityTop = affinityTop + affinityRows * (chipH + S(6)) + blockGap;
    const int rememberTop = priorityTop + priorityRows * (chipH + S(6)) + blockGap;
    int cardH = row ? rememberTop - cardTop + S(38) : S(152);
    RECT card = R(cx, cardTop, cx + cw, cardTop + cardH);
    Card(dc, card, false);

    if (!row) {
        TextIn(dc, L"Select a process to set its affinity, its priority, or to save those "
                    L"choices for every future run.",
               R(card.left + S(20), card.top, card.right - S(20), card.bottom), g_fonts.small,
               theme::muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        TextIn(dc, row->name, R(card.left + S(20), card.top + S(10), card.left + S(300),
                                card.top + S(32)),
               g_fonts.h3, theme::text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        wchar_t info[192];
        swprintf(info, 192, L"PID %lu   %ls   %ls   %ls", (unsigned long)row->pid,
                 row->user.empty() ? L"unknown account" : row->user.c_str(),
                 AffinityText(row->affinity).c_str(), PriorityLabel(row->priority).c_str());
        TextIn(dc, info, R(card.left + S(310), card.top + S(13), card.right - S(20),
                            card.top + S(32)),
               g_fonts.small, theme::muted, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        TextIn(dc, L"Affinity", R(card.left + S(20), affinityTop, card.left + S(90),
                                   affinityTop + chipH),
               g_fonts.small, theme::muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        int chipX = chipsLeft;
        int chipY = affinityTop;
        for (int cpu = 0; cpu < cpuCount; ++cpu) {
            if (chipX > chipsLeft && chipX + S(34) > chipsRight) {
                chipX = chipsLeft;
                chipY += chipH + S(6);
            }
            bool on = (st.taskCpuMask >> cpu) & 1;
            wchar_t label[8];
            swprintf(label, 8, L"%d", cpu);
            RECT chip = R(chipX, chipY, chipX + S(34), chipY + chipH);
            ChipButton(dc, chip, label, on, st.hoverId == HitTaskCpuBase + cpu);
            RecordHit(hits, chip, HitTaskCpuBase + cpu);
            chipX = chip.right + chipGap;
        }
        RECT applyAffinity = R(card.right - S(20) - applyW, affinityTop, card.right - S(20),
                               affinityTop + chipH);
        Button(dc, applyAffinity, L"Apply cores", false,
               st.hoverId == HitTaskApplyAffinity, st.taskCpuMask != 0);
        RecordHit(hits, applyAffinity, HitTaskApplyAffinity, st.taskCpuMask != 0);

        TextIn(dc, L"Priority", R(card.left + S(20), priorityTop, card.left + S(90),
                                   priorityTop + chipH),
               g_fonts.small, theme::muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        chipX = chipsLeft;
        chipY = priorityTop;
        for (int i = 0; i < 6; ++i) {
            int w = priorityWidths[i];
            if (chipX > chipsLeft && chipX + w > chipsRight) {
                chipX = chipsLeft;
                chipY += chipH + S(6);
            }
            RECT chip = R(chipX, chipY, chipX + w, chipY + chipH);
            ChipButton(dc, chip, priorityNames[i], st.taskPriority == priorityValues[i],
                       st.hoverId == HitTaskPriorityBase + i);
            RecordHit(hits, chip, HitTaskPriorityBase + i);
            chipX = chip.right + chipGap;
        }
        RECT applyPriority = R(card.right - S(20) - applyW, priorityTop, card.right - S(20),
                               priorityTop + chipH);
        Button(dc, applyPriority, L"Apply class", false,
               st.hoverId == HitTaskApplyPriority);
        RecordHit(hits, applyPriority, HitTaskApplyPriority);

        int lineY = rememberTop;
        int box = S(20);
        RECT cb = R(card.left + S(20), lineY - S(9), card.left + S(20) + box, lineY - S(9) + box);
        TickBox(dc, cb, st.taskRemember);
        TextIn(dc, L"Remember these for this program, at sign-in too",
               R(cb.right + S(10), lineY - S(13), cb.right + S(360), lineY + S(13)),
               g_fonts.small, theme::muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RecordHit(hits, R(card.left + S(14), lineY - S(18), cb.right + S(370), lineY + S(18)),
                  HitTaskRemember);

        bool hasRule = RuleForExe(st.taskRules, row->name);
        RECT forget = R(card.right - S(200), lineY - S(13), card.right - S(20), lineY + S(13));
        bool forgetHover = st.hoverId == HitTaskForget;
        TextIn(dc, L"Forget the saved rule", forget, g_fonts.small,
               hasRule ? (forgetHover ? theme::accent : theme::muted) : theme::faint,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (hasRule && forgetHover) HLine(dc, forget.left, forget.right, forget.bottom, theme::accent);
        RecordHit(hits, forget, HitTaskForget, hasRule);
    }

    // Same search box as Services: only the processes whose name contains the text are
    // listed, and an empty box lists everything.
    int searchY = cardTop + cardH + S(8);
    RECT search = R(cx, searchY, cx + std::min(cw, S(320)), searchY + S(34));
    InputField(dc, hits, search, st.taskFilter, L"Search processes by name",
                st.inputFocus == HitTaskFilter, st.hoverId, HitTaskFilter, HitTaskFilterClear);

    const std::vector<int> visible = FilterTaskRows(st.taskRows, st.taskFilter);

    // Bulk tick controls, laid out the same way as the Services page.
    if (!visible.empty()) {
        int selY = searchY + S(4);
        RECT all = R(cx + cw - S(184), selY, cx + cw - S(100), selY + S(26));
        bool allHover = st.hoverId == HitTaskSelectAll;
        TextIn(dc, L"Select all", all, g_fonts.small,
               allHover ? theme::accent : theme::muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (allHover) HLine(dc, all.left + S(24), all.right, all.bottom - S(2), theme::accent);
        RecordHit(hits, all, HitTaskSelectAll);

        RECT clear = R(cx + cw - S(92), selY, cx + cw, selY + S(26));
        bool clearHover = st.hoverId == HitTaskClearChecked;
        TextIn(dc, L"Clear ticks", clear, g_fonts.small,
               clearHover ? theme::accent : theme::muted,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (clearHover)
            HLine(dc, clear.left + S(22), clear.right, clear.bottom - S(2), theme::accent);
        RecordHit(hits, clear, HitTaskClearChecked);
    }

    int listTop = search.bottom + S(10);
    if (!st.tasksMessage.empty()) {
        RECT banner = R(cx, listTop, cx + cw, listTop + S(34));
        ColorRefMessage(dc, banner, st.tasksMessage, st.tasksMessageIsError);
        listTop += S(42);
    }

    // Column header on its own line, so a scrolled row never slides under the labels.
    int headerY = listTop;
    TextAt(dc, L"Process", cx + S(42), headerY, g_fonts.tiny, theme::faint);
    TextIn(dc, L"PID", R(cx + cw - S(430), headerY, cx + cw - S(360), headerY + S(12)),
           g_fonts.tiny, theme::faint, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    TextIn(dc, L"Affinity", R(cx + cw - S(350), headerY, cx + cw - S(250), headerY + S(12)),
           g_fonts.tiny, theme::faint, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    TextIn(dc, L"Priority", R(cx + cw - S(240), headerY, cx + cw - S(140), headerY + S(12)),
           g_fonts.tiny, theme::faint, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    TextIn(dc, L"Memory", R(cx + cw - S(130), headerY, cx + cw - S(20), headerY + S(12)),
           g_fonts.tiny, theme::faint, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    int rowsTop = headerY + S(18);

    int listBottom = h - S(58);
    int rowH = S(44), gap = S(6);
    // Whole rows only, exactly like the Services list: no process row is left half drawn
    // at the bottom after a wheel notch.
    const WholeRowList layout =
        FitWholeRows(rowsTop, listBottom, rowH, gap, (int)visible.size(), st.scrollTasks);
    st.scrollTasks = layout.scroll;
    listBottom = layout.bottom;

    if (visible.empty()) {
        if (st.taskRows.empty()) {
            TextAt(dc,
                   st.tasksLoaded ? L"No processes were returned."
                                  : L"Reading the process list...",
                   cx, rowsTop, g_fonts.small, theme::muted);
        } else {
            TextIn(dc, std::wstring(L"No process name matches \"") + st.taskFilter +
                          L"\". Clear the box to list all " +
                          std::to_wstring(st.taskRows.size()) + L" processes.",
                   R(cx, rowsTop, cx + cw, rowsTop + S(22)), g_fonts.small, theme::muted,
                   DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, cx, rowsTop, cx + cw, listBottom);
    for (size_t v = 0; v < visible.size(); ++v) {
        int i = visible[v];
        int ry = rowsTop - st.scrollTasks + (int)v * (rowH + gap);
        if (ry + rowH < rowsTop || ry > listBottom) continue;
        const ProcRow& p = st.taskRows[(size_t)i];
        int id = HitTaskRowBase + i;
        bool selected = i == st.taskSelected;
        bool ticked = p.pid > 4 && IsTaskChecked(st, p.pid, p.created);
        RECT rr = R(cx, ry, cx + cw, ry + rowH);
        Card(dc, rr, ticked || selected || st.hoverId == id);
        if (ticked || selected)
            FillRound(dc, R(rr.left, rr.top + S(8), rr.left + S(3), rr.bottom - S(8)),
                      1.5f * g_scale, theme::accent);
        if (p.pid > 4) {
            RECT cb = R(rr.left + S(16), rr.top + (rowH - S(16)) / 2, rr.left + S(32),
                        rr.top + (rowH - S(16)) / 2 + S(16));
            TickBox(dc, cb, ticked);
        }
        TextIn(dc, p.name, R(rr.left + S(42), rr.top, rr.left + S(320), rr.bottom), g_fonts.h3,
               p.accessible ? theme::text : theme::faint,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        TextIn(dc, p.user, R(rr.left + S(330), rr.top, rr.left + S(470), rr.bottom),
               g_fonts.small, theme::faint,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        TextIn(dc, std::to_wstring(p.pid),
               R(rr.right - S(430), rr.top, rr.right - S(360), rr.bottom), g_fonts.small,
               theme::muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        TextIn(dc, AffinityText(p.affinity),
               R(rr.right - S(350), rr.top, rr.right - S(250), rr.bottom), g_fonts.small,
               theme::muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        TextIn(dc, PriorityLabel(p.priority),
               R(rr.right - S(240), rr.top, rr.right - S(140), rr.bottom), g_fonts.small,
               theme::muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        TextIn(dc, FormatBytes(p.workingSet),
               R(rr.right - S(130), rr.top, rr.right - S(20), rr.bottom), g_fonts.small,
               theme::muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        // The idle and System entries have no process behind them to act on, so they are
        // listed but never selectable.
        if (p.pid <= 4) continue;
        RECT hit = rr;
        hit.top = std::max(hit.top, (LONG)rowsTop);
        hit.bottom = std::min(hit.bottom, (LONG)listBottom);
        RecordHit(hits, hit, id);
    }
    RestoreDC(dc, saved);

    std::wstring footer;
    if (st.taskFilter.empty()) {
        footer = std::to_wstring(st.taskRows.size()) + L" processes";
    } else {
        footer = std::to_wstring(visible.size()) + L" of " +
                 std::to_wstring(st.taskRows.size()) + L" processes match \"" +
                 st.taskFilter + L"\"";
    }
    if (!st.taskChecked.empty())
        footer += L"  -  " + std::to_wstring(st.taskChecked.size()) + L" ticked";
    footer += L"  -  " + std::to_wstring(st.taskRules.size()) +
              L" saved rule(s) for sign-in. End task acts on every ticked row, or on the "
              L"highlighted row when none is ticked.";
    TextIn(dc, footer, R(cx, h - S(48), cx + cw, h - S(28)), g_fonts.tiny, theme::faint,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------- settings
// The exact terms this build ships under, shown by the Settings page and the read-only
// viewer. Kept here so both, and the self-test, read the same words.
const wchar_t* const kLicenseText =
    L"BSD 3-Clause License\n"
    L"\n"
    L"Copyright (c) 2026-2027, Charles McDonald\n"
    L"All rights reserved.\n"
    L"\n"
    L"Redistribution and use in source and binary forms, with or without modification, are "
    L"permitted provided that the following conditions are met:\n"
    L"\n"
    L"1. Redistributions of source code must retain the above copyright notice, this list "
    L"of conditions and the following disclaimer.\n"
    L"\n"
    L"2. Redistributions in binary form must reproduce the above copyright notice, this "
    L"list of conditions and the following disclaimer in the documentation and/or other "
    L"materials provided with the distribution.\n"
    L"\n"
    L"3. Neither the name of the copyright holder nor the names of its contributors may be "
    L"used to endorse or promote products derived from this software without specific "
    L"prior written permission.\n"
    L"\n"
    L"THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\" AND ANY "
    L"EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES "
    L"OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT "
    L"SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, "
    L"INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED "
    L"TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR "
    L"BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN "
    L"CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN "
    L"ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH "
    L"DAMAGE.\n";

static int WrappedHeight(HDC dc, const std::wstring& text, HFONT font, int width) {
    HGDIOBJ old = SelectObject(dc, font);
    RECT r = R(0, 0, width, 0);
    DrawTextW(dc, text.c_str(), (int)text.size(), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom;
}

// The same rounded tick the Cleanup rows use, so a choice reads the same way everywhere.
static void TickBox(HDC dc, const RECT& cb, bool ticked) {
    if (ticked) {
        FillRound(dc, cb, 5.0f * g_scale, theme::accent);
        HPEN pen = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(9, 26, 18));
        HGDIOBJ old = SelectObject(dc, pen);
        int midY = (cb.top + cb.bottom) / 2;
        MoveToEx(dc, cb.left + S(5), midY, nullptr);
        LineTo(dc, cb.left + S(9), midY + S(4));
        LineTo(dc, cb.right - S(5), midY - S(5));
        SelectObject(dc, old);
        DeleteObject(pen);
    } else {
        FillRound(dc, cb, 5.0f * g_scale, RGB(20, 26, 33));
        StrokeRound(dc, cb, 5.0f * g_scale, theme::active, 1.0f);
    }
}

static void PaintOptionRow(HDC dc, std::vector<Hit>& hits, const RECT& rr, bool on,
                           const wchar_t* name, const wchar_t* detail, int id, int hoverId) {
    Card(dc, rr, hoverId == id);
    int size = S(20);
    RECT cb = R(rr.left + S(20), rr.top + S(20), rr.left + S(20) + size, rr.top + S(20) + size);
    TickBox(dc, cb, on);
    TextIn(dc, name, R(rr.left + S(54), rr.top + S(14), rr.right - S(90), rr.top + S(36)),
           g_fonts.h3, theme::text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    TextIn(dc, detail, R(rr.left + S(54), rr.top + S(38), rr.right - S(24), rr.bottom - S(10)),
           g_fonts.small, theme::muted, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    TextIn(dc, on ? L"On" : L"Off",
           R(rr.right - S(80), rr.top + S(13), rr.right - S(20), rr.top + S(36)), g_fonts.small,
           on ? theme::accent : theme::muted, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    RecordHit(hits, rr, id);
}

// A row is as tall as its explanation needs. The name line and the On/Off label take the
// first S(38); the explanation is measured at the width it will be drawn in, so a narrow
// window grows the card instead of letting the last line spill past the card's edge or
// run into the next section.
static int OptionRowHeight(HDC dc, int cw, const wchar_t* detail) {
    int textW = std::max(S(80), cw - S(54) - S(24));
    int textH = WrappedHeight(dc, detail, g_fonts.small, textW);
    return std::max(S(64), S(38) + textH + S(12));
}

static void PaintSettings(HDC dc, int cx, int cw, int h, AppState& st,
                          std::vector<Hit>& hits) {
    Header(dc, cx, S(28), cw, L"Settings",
           L"How the window starts and closes, which memory clean dialogs appear, and the "
           L"licence.");

    // The page scrolls rather than dropping the licence card off a short window, and the
    // same list keeps working when more options are added later.
    const int listTop = S(112);
    const int listBottom = h - S(56);
    const int labelH = S(18);
    const int gap = S(10);
    const int sectionGap = S(14);
    const int cardH = S(140);

    const wchar_t* closeDetail =
        L"The X button hides the window in the notification area instead of exiting. "
        L"Open it again from the tray icon; the icon's menu can exit for good.";
    const wchar_t* minimizeDetail =
        L"Minimising takes the window off the taskbar and leaves it in the "
        L"notification area, where the tray icon brings it back.";
    const wchar_t* startupDetail =
        L"WinSuite opens straight into the notification area, with no window on the "
        L"desktop. Open it from the tray icon, or ask for a memory clean from the icon's "
        L"menu without opening it at all.";
    const wchar_t* toastDetail =
        L"A balloon from the tray icon reports what a memory clean started from its menu "
        L"did. Turn this off for a silent clean; the Memory page still shows the result "
        L"the next time the window is open.";
    const wchar_t* regularDetail =
        L"Shows the confirmation before trimming working sets. Nothing is closed and "
        L"no data is lost either way.";
    const wchar_t* deepDetail =
        L"Shows the standby-cache warning first. The administrator prompt still "
        L"appears when one is needed to run the clean.";

    const int closeH = OptionRowHeight(dc, cw, closeDetail);
    const int minimizeH = OptionRowHeight(dc, cw, minimizeDetail);
    const int startupH = OptionRowHeight(dc, cw, startupDetail);
    const int toastH = OptionRowHeight(dc, cw, toastDetail);
    const int regularH = OptionRowHeight(dc, cw, regularDetail);
    const int deepH = OptionRowHeight(dc, cw, deepDetail);
    const int contentH = labelH + closeH + gap + minimizeH + gap + startupH + gap + toastH +
                         sectionGap + labelH + regularH + S(8) + deepH + sectionGap + labelH +
                         cardH;
    const int listH = std::max(0, listBottom - listTop);
    const int maxScroll = std::max(0, contentH - listH);
    st.scrollSettings = std::clamp(st.scrollSettings, 0, maxScroll);

    const size_t firstHit = hits.size();
    int saved = SaveDC(dc);
    IntersectClipRect(dc, cx, listTop, cx + cw, listBottom);
    int y = listTop - st.scrollSettings;

    TextAt(dc, L"WINDOW", cx, y + S(2), g_fonts.tiny, theme::muted);
    y += labelH;

    PaintOptionRow(dc, hits, R(cx, y, cx + cw, y + closeH), st.closeToTray,
                   L"Close to system tray", closeDetail, HitSettingsCloseTray, st.hoverId);
    y += closeH + gap;

    PaintOptionRow(dc, hits, R(cx, y, cx + cw, y + minimizeH), st.minimizeToTray,
                   L"Minimize to system tray", minimizeDetail, HitSettingsMinTray, st.hoverId);
    y += minimizeH + gap;

    PaintOptionRow(dc, hits, R(cx, y, cx + cw, y + startupH), st.startInTray,
                   L"Show on system tray at startup", startupDetail, HitSettingsTrayStartup,
                   st.hoverId);
    y += startupH + gap;

    PaintOptionRow(dc, hits, R(cx, y, cx + cw, y + toastH), st.toastNotifications,
                   L"Toast notifications", toastDetail, HitSettingsToasts, st.hoverId);
    y += toastH + sectionGap;

    TextAt(dc, L"MEMORY CLEANING", cx, y + S(2), g_fonts.tiny, theme::muted);
    y += labelH;

    PaintOptionRow(dc, hits, R(cx, y, cx + cw, y + regularH), st.memPrefs.confirmRegular,
                   L"Ask before Regular clean", regularDetail, HitSettingsMemRegular, st.hoverId);
    y += regularH + S(8);

    PaintOptionRow(dc, hits, R(cx, y, cx + cw, y + deepH), st.memPrefs.confirmDeep,
                   L"Ask before Deep clean", deepDetail, HitSettingsMemDeep, st.hoverId);
    y += deepH + sectionGap;

    TextAt(dc, L"ABOUT", cx, y + S(2), g_fonts.tiny, theme::muted);
    y += labelH;

    RECT card = R(cx, y, cx + cw, y + cardH);
    Card(dc, card, false);
    TextIn(dc, L"Copyright \u00a9 2026-2027 Charles McDonald.",
           R(card.left + S(24), card.top + S(14), card.right - S(24), card.top + S(36)),
           g_fonts.h3, theme::text, DT_LEFT | DT_TOP | DT_SINGLELINE);
    TextIn(dc,
           L"This software is licensed under the terms of the BSDv3-Clause License "
           L"Agreement.",
           R(card.left + S(24), card.top + S(38), card.right - S(24), card.top + S(80)),
           g_fonts.small, theme::muted, DT_LEFT | DT_TOP | DT_WORDBREAK);

    RECT readBtn = R(card.left + S(24), card.bottom - S(54), card.left + S(24) + S(196),
                     card.bottom - S(16));
    bool readHover = st.hoverId == HitSettingsLicense;
    Button(dc, readBtn, L"Read the licence", false, readHover);
    RecordHit(hits, readBtn, HitSettingsLicense);
    TextIn(dc, L"Opens the full agreement read-only, without a cursor or an editor.",
           R(readBtn.right + S(16), readBtn.top, card.right - S(24), readBtn.bottom),
           g_fonts.tiny, theme::faint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RestoreDC(dc, saved);

    // A scrolled row is only clickable where it is actually on screen, so a press in the
    // footer can never reach a row that has scrolled underneath it.
    for (size_t i = firstHit; i < hits.size();) {
        RECT& r = hits[i].rect;
        r.top = std::max<LONG>(r.top, listTop);
        r.bottom = std::min<LONG>(r.bottom, listBottom);
        if (r.bottom <= r.top)
            hits.erase(hits.begin() + (long)i);
        else
            ++i;
    }

    if (maxScroll > 0) {
        int barH = std::max(S(30), listH * listH / std::max(1, contentH));
        int barY = listTop + (listH - barH) * st.scrollSettings / std::max(1, maxScroll);
        FillRound(dc, R(cx + cw + S(10), barY, cx + cw + S(14), barY + barH), 2.0f * g_scale,
                  theme::active);
    }

    TextIn(dc, L"The memory dialogs are on by default, so a clean never runs unannounced.",
           R(cx, h - S(42), cx + cw, h - S(22)), g_fonts.tiny, theme::faint,
           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// The read-only agreement. It covers the window rather than opening a dialog, so the
// text is drawn at full size on the app's own background and only the Close control (or
// Escape) leaves it; every hit behind it is dropped while it is up.
static void PaintLicenseView(HDC dc, int w, int h, AppState& st, std::vector<Hit>& hits) {
    hits.clear();
    FillRect(dc, R(0, 0, w, h), theme::bg);

    int pad = S(40);
    TextAt(dc, L"BSD 3-Clause License Agreement", pad, S(32), g_fonts.h1, theme::text);
    TextAt(dc, L"Read only - the agreement cannot be edited. Scroll with the mouse wheel.", pad,
           S(76), g_fonts.small, theme::muted);

    RECT closeBtn = R(w - pad - S(120), S(32), w - pad, S(32) + S(38));
    Button(dc, closeBtn, L"Close", false, st.hoverId == HitLicenseClose);
    RecordHit(hits, closeBtn, HitLicenseClose);

    // The card takes the height the text needs, up to the window: a short agreement in a
    // window-height box would otherwise float in a lot of empty space.
    int textW = (w - 2 * pad) - S(48);
    int content = WrappedHeight(dc, kLicenseText, g_fonts.small, textW);
    int cardH = std::min((h - S(38)) - S(112), content + S(40));
    RECT card = R(pad, S(112), w - pad, S(112) + cardH);
    Card(dc, card, false);
    int areaH = (card.bottom - card.top) - S(40);
    int maxScroll = std::max(0, content - areaH);
    if (st.licenseScroll > maxScroll) st.licenseScroll = maxScroll;
    if (st.licenseScroll < 0) st.licenseScroll = 0;

    int saved = SaveDC(dc);
    IntersectClipRect(dc, card.left + S(24), card.top + S(20), card.right - S(24),
                      card.bottom - S(20));
    HGDIOBJ old = SelectObject(dc, g_fonts.small);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme::text);
    RECT tr = R(card.left + S(24), card.top + S(20) - st.licenseScroll,
                card.left + S(24) + textW,
                card.top + S(20) - st.licenseScroll + content + S(20));
    DrawTextW(dc, kLicenseText, -1, &tr, DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    RestoreDC(dc, saved);

    if (maxScroll > 0)
        TextIn(dc, L"Scroll for the rest of the agreement",
               R(pad, h - S(32), w - pad, h - S(12)), g_fonts.tiny, theme::faint,
               DT_RIGHT | DT_TOP | DT_SINGLELINE);
}

// ------------------------------------------------------------------ frame
void PaintApp(HDC dc, int width, int height, AppState& st, std::vector<Hit>& hits) {
    hits.clear();
    st.width = width;
    st.height = height;

    SetBkMode(dc, TRANSPARENT);
    FillRect(dc, R(0, 0, width, height), theme::bg);

    PaintSidebar(dc, width, height, st, hits);

    int pad = S(32);
    int cx = S(kSidebarW) + pad;
    int cw = width - cx - pad;
    if (cw < S(200)) return;

    switch (st.page) {
        case Page::Dashboard:
            PaintDashboard(dc, cx, cw, height, st, hits);
            break;
        case Page::Cleanup:
            PaintCleanup(dc, cx, cw, height, st, hits);
            break;
        case Page::Browsers:
            PaintBrowsers(dc, cx, cw, height, st, hits);
            break;
        case Page::Dns:
            PaintDns(dc, cx, cw, height, st, hits);
            break;
        case Page::Network:
            PaintNetwork(dc, cx, cw, height, st, hits);
            break;
        case Page::Boost:
            PaintGameBooster(dc, cx, cw, height, st, hits);
            break;
        case Page::Memory:
            PaintMemory(dc, cx, cw, height, st, hits);
            break;
        case Page::LargeFiles:
            PaintLargeFiles(dc, cx, cw, height, st, hits);
            break;
        case Page::Startup:
            PaintStartup(dc, cx, cw, height, st, hits);
            break;
        case Page::Deactivate:
            PaintDeactivate(dc, cx, cw, height, st, hits);
            break;
        case Page::System:
            PaintSystem(dc, cx, cw, height, st, hits);
            break;
        case Page::Storage:
            PaintStorage(dc, cx, cw, height, st, hits);
            break;
        case Page::Services:
            PaintServices(dc, cx, cw, height, st, hits);
            break;
        case Page::Tasks:
            PaintTasks(dc, cx, cw, height, st, hits);
            break;
        case Page::Settings:
            PaintSettings(dc, cx, cw, height, st, hits);
            break;
    }

    // Drawn last and over everything: while the agreement is open it is the only thing
    // that can be clicked, which is what makes it read-only in practice.
    if (st.showLicense) PaintLicenseView(dc, width, height, st, hits);
}
