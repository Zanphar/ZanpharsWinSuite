// tray.h — notification area icon.
#pragma once
#include "util.h"

class TrayIcon {
public:
    ~TrayIcon();
    bool Add(HWND owner, UINT callbackMsg, HICON icon, const std::wstring& tip);
    void ReplaceIcon(HICON newIcon);            // takes ownership, destroys old
    HICON CurrentIcon() const { return icon_; }
    void UpdateTip(const std::wstring& tip);
    void Remove();

private:
    HWND owner_ = nullptr;
    UINT cbMsg_ = 0;
    HICON icon_ = nullptr;
    bool present_ = false;
};
