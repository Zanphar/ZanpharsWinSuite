// tray.cpp — notification area icon.
#include "tray.h"
#include <shellapi.h>

TrayIcon::~TrayIcon() {
    Remove();
}

static NOTIFYICONDATAW MakeNid(HWND owner, UINT cbMsg) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner;
    nid.uID = 1;
    nid.uCallbackMessage = cbMsg;

    static const wchar_t kTipBase[] = L"Smart RAM";
    wcscpy_s(nid.szTip, kTipBase);
    return nid;
}

bool TrayIcon::Add(HWND owner, UINT cbMsg, HICON icon, const std::wstring& tip) {
    owner_ = owner;
    cbMsg_ = cbMsg;
    NOTIFYICONDATAW nid = MakeNid(owner, cbMsg);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.hIcon = icon;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    present_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (present_) icon_ = icon;
    else DestroyIcon(icon);
    return present_;
}

void TrayIcon::ReplaceIcon(HICON newIcon) {
    if (!present_ || !newIcon) { DestroyIcon(newIcon); return; }
    NOTIFYICONDATAW nid = MakeNid(owner_, cbMsg_);
    nid.uFlags = NIF_ICON;
    nid.hIcon = newIcon;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (icon_) DestroyIcon(icon_);
    icon_ = newIcon;
}

void TrayIcon::UpdateTip(const std::wstring& tip) {
    if (!present_) return;
    NOTIFYICONDATAW nid = MakeNid(owner_, cbMsg_);
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::Remove() {
    if (!present_) return;
    NOTIFYICONDATAW nid = MakeNid(owner_, cbMsg_);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    present_ = false;
    if (icon_) { DestroyIcon(icon_); icon_ = nullptr; }
}
