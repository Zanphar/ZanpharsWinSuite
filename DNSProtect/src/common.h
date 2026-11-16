// ============================================================================
//  common.h — shared includes, types and small utilities for DNS Protect.
// ============================================================================

#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <iphlpapi.h>

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <filesystem>
#include <cwctype>
#include <cwchar>

#ifdef _MSC_VER
#  pragma comment(lib, "advapi32")
#  pragma comment(lib, "shell32")
#  pragma comment(lib, "ole32")
#  pragma comment(lib, "uuid")
#  pragma comment(lib, "iphlpapi")
#  pragma comment(lib, "ws2_32")
#  pragma comment(lib, "comctl32")
#  pragma comment(lib, "gdi32")
#  pragma comment(lib, "user32")
#  pragma comment(lib, "dwmapi")
#endif

namespace dzp {

// ---------------------------------------------------------------------------
// Messaging contract between the protection engine and the UI.
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

inline std::wstring StripBraces(std::wstring s) {
    if (!s.empty() && s.front() == L'{') s.erase(0, 1);
    if (!s.empty() && s.back()  == L'}') s.pop_back();
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

// Compares two comma-separated DNS address lists ignoring order and spacing.
inline bool SameDnsList(const std::wstring& a, const std::wstring& b) {
    std::vector<std::wstring> va, vb;
    auto split = [](const std::wstring& in, std::vector<std::wstring>& out) {
        std::wstring cur;
        for (wchar_t c : in) {
            if (c == L',' || c == L';' || c == L' ') {
                if (!cur.empty()) { out.push_back(Trim(cur)); cur.clear(); }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) out.push_back(Trim(cur));
    };
    split(a, va);
    split(b, vb);
    std::sort(va.begin(), va.end());
    std::sort(vb.begin(), vb.end());
    return va == vb;
}

inline bool IsValidIpv4(const std::wstring& s) {
    in_addr addr;
    const std::string narrow(s.begin(), s.end());
    return !s.empty() && inet_pton(AF_INET, narrow.c_str(), &addr) == 1;
}

// Unique id of an interface registry key ("{GUID}" -> lowercase guid).
inline std::wstring IfaceId(const std::wstring& key) {
    return ToLower(StripBraces(key));
}

}  // namespace dzp
