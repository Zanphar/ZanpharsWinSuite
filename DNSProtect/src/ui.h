// ============================================================================
//  ui.h — main window (custom-drawn dashboard UI, tray integration).
// ============================================================================

#pragma once
#include "common.h"

namespace dzp::ui {

// Shared identity so the process entry can coordinate single-instance runs.
inline constexpr wchar_t kWindowClass[] = L"DNSProtectMainWnd";
inline constexpr wchar_t kMutexName[]   = L"Local\\DNSProtect.SingleInstance";
inline constexpr wchar_t kAppTitle[]    = L"DNS Protect";

// Creates and pumps the main window until the user exits.
// The engine is prepared by the caller; its notification window is attached
// here once the window exists.
int Run(HINSTANCE instance, int cmdShow);

}  // namespace dzp::ui
