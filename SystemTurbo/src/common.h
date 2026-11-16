// ============================================================================
//  common.h — shared includes, types and small utilities for System Turbo.
// ============================================================================

#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <powrprof.h>
#include <psapi.h>

#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cwctype>
#include <cwchar>

#ifdef _MSC_VER
#  pragma comment(lib, "advapi32")
#  pragma comment(lib, "shell32")
#  pragma comment(lib, "ole32")
#  pragma comment(lib, "uuid")
#  pragma comment(lib, "comctl32")
#  pragma comment(lib, "gdi32")
#  pragma comment(lib, "user32")
#  pragma comment(lib, "dwmapi")
#  pragma comment(lib, "psapi")
#  pragma comment(lib, "powrprof")
#endif

namespace stc {

// ---------------------------------------------------------------------------
// Messaging contract between the boost engine and the UI.
// ---------------------------------------------------------------------------

enum : UINT {
    WM_APP_TRAY    = WM_APP + 1,   // tray icon callback (lParam = mouse msg)
    WM_APP_REFRESH = WM_APP + 2,   // engine -> UI: state changed, re-snapshot
};

// ---------------------------------------------------------------------------
// Small string utilities.
// ---------------------------------------------------------------------------

inline std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

inline std::wstring Trim(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    const size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

template <typename... Args>
inline std::wstring Fmt(const wchar_t* fmt, Args... args) {
    wchar_t buf[2048];
    _snwprintf(buf, 2047, fmt, args...);
    buf[2047] = 0;
    return buf;
}

// Renders a byte count with a human-friendly unit.
inline std::wstring FormatBytes(unsigned long long bytes) {
    const double mb = bytes / (1024.0 * 1024.0);
    if (mb >= 1024) return Fmt(L"%.1f GB", mb / 1024.0);
    if (mb >= 1)    return Fmt(L"%d MB", static_cast<int>(mb + 0.5));
    if (bytes >= 1024) return Fmt(L"%d KB", static_cast<int>(bytes / 1024));
    return Fmt(L"%d B", static_cast<int>(bytes));
}

// Prompt threshold of the automatic memory-release pass, in load percent.
inline constexpr double kAutoReleaseLoad = 80.0;

}  // namespace stc
