// ============================================================================
//  ui.h — main window (custom-drawn dashboard UI, tray integration).
// ============================================================================

#pragma once
#include "common.h"

namespace stc::ui {

// Shared identity so the process entry can coordinate single-instance runs.
inline constexpr wchar_t kWindowClass[] = L"SystemTurboMainWnd";
inline constexpr wchar_t kMutexName[]   = L"Local\\SystemTurbo.SingleInstance";
inline constexpr wchar_t kAppTitle[]    = L"System Turbo";

// Creates and pumps the main window until the user exits.
// The engine is prepared by the caller; its notification window is attached
// here once the window exists.
int Run(HINSTANCE instance, int cmdShow);

}  // namespace stc::ui
