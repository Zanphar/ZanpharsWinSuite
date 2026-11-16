// ============================================================================
//  dns.h — the DNS protection engine.
//
//  Monitors the per-adapter DNS registry values
//    HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{GUID}
//      NameServer      (static address list)
//      DhcpNameServer  (DHCP-assigned addresses)
//  and snaps them back to the approved configuration whenever something else
//  modifies them (typically DNS-hijacking malware).
// ============================================================================

#pragma once
#include "common.h"
#include "config.h"

namespace dzp {

// A network adapter with IP-over-TCP configuration present.
struct Iface {
    std::wstring Key;                // registry subkey name, e.g. "{8A5F...}"
    std::wstring Name;               // adapter friendly name (best effort)
    bool           Dhcp    = false;  // addresses (and DNS) received via DHCP
    bool           Up      = false;  // adapter is currently operational
    std::wstring   NameServer;       // static list, comma-separated
    std::wstring   DhcpNameServer;   // DHCP-assigned list
};

enum class AdapterState { Ok, Mismatch, Untracked };

struct IfaceView {
    Iface          adapter;
    AdapterState   State = AdapterState::Untracked;
    std::wstring   Expected;   // approved NameServer list (display text)
};

struct Status {
    bool                     Enabled   = false;
    std::wstring             Provider;              // provider id
    std::wstring             CustomPrimary;
    std::wstring             CustomSecondary;
    unsigned long long       HijacksReverted = 0;
    bool                     Elevated = false;
    std::vector<IfaceView>   Adapters;
};

class Engine {
 public:
    static Engine& Instance();

    // Assigns the data directory, loads settings and the DNS baseline.
    // Call once, before any other method. `notify` receives WM_APP_REFRESH
    // whenever the engine changes state.
    bool Prepare(const DataDirs& dirs, HWND notify, bool elevated);
    void Shutdown();

    // Re-attaches the refresh-notification window (after the UI exists).
    void SetNotify(HWND window);

    // Thread-safe point-in-time view for the UI.
    Status Snapshot();

    // Enables protection for `providerId`. When providerId == "custom",
    // `primary`/`secondary` are used (and validated). Returns an empty
    // string on success, otherwise a human-readable error message.
    std::wstring Enable(const std::wstring& providerId,
                        const std::wstring& primary = L"",
                        const std::wstring& secondary = L"");
    void Disable();

    // Writes the stored "original DNS" baseline back to every adapter.
    // Returns an empty string on success, otherwise a user-facing message.
    std::wstring RestoreOriginal();

 private:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace dzp
