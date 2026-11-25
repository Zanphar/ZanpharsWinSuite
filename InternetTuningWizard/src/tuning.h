// ============================================================================
//  tuning.h — the Internet Tuning Wizard's model and registry engine.
//
//  Four changes, all of them plain policy-style values Windows already reads:
//
//    * network throttling      HKLM\...\Multimedia\SystemProfile
//                              NetworkThrottlingIndex = 0xFFFFFFFF
//    * IPv6                    HKLM\SYSTEM\...\Tcpip6\Parameters
//                              DisabledComponents = 0xFF
//    * Nagle / delayed ACK     HKLM\SYSTEM\...\Tcpip\Parameters\Interfaces\{GUID}
//                              TcpAckFrequency = 1, TCPNoDelay = 1
//    * DNS resolver            the same interface key, NameServer = "1.1.1.1,1.0.0.1"
//
//  Every change is undone by removing the value (or writing 0 for IPv6), which
//  is what Restore Windows defaults does. No service, no netsh, no reboot.
// ============================================================================

#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <string>
#include <vector>

namespace itw {

// ---------------------------------------------------------------------------
//  Adapters
// ---------------------------------------------------------------------------

struct Adapter {
    std::wstring guid;    // "{GUID}", the registry subkey under Tcpip\Interfaces
    std::wstring name;    // friendly name: "Ethernet", "Wi-Fi"
    std::wstring model;   // driver description, e.g. "Realtek PCIe GbE Family Controller"
    std::wstring kind;    // "Ethernet" or "Wi-Fi"
    std::wstring address; // first IPv4 address, may be empty
    std::vector<std::wstring> dns;   // configured resolvers, may be empty
    bool wifi = false;
};

// Connected physical adapters, wired first, then wireless, each by name.
std::vector<Adapter> Adapters();

const Adapter* FindAdapter(const std::vector<Adapter>& list, const std::wstring& guid);

// ---------------------------------------------------------------------------
//  DNS providers
// ---------------------------------------------------------------------------

struct DnsProvider {
    const wchar_t* label;   // "Cloudflare"
    const wchar_t* servers; // "1.1.1.1,1.0.0.1"; empty means automatic
    const wchar_t* detail;  // "1.1.1.1 and 1.0.0.1"
};

const std::vector<DnsProvider>& DnsProviders();

// ---------------------------------------------------------------------------
//  A plan and its report
// ---------------------------------------------------------------------------

struct Plan {
    std::wstring adapter; // empty: adapter-scoped changes are skipped
    int  dns = 0;         // index into DnsProviders(); 0 is automatic
    bool nagle = false;   // Nagle and delayed ACK off on that adapter
    bool throttle = false;
    bool ipv6 = false;
    bool scheduler = false; // multimedia CPU reserve off (SystemResponsiveness = 0)
    bool timeWait = false;  // TcpTimedWaitDelay = 30 seconds
    bool restoreDefaults = false;  // Restore(): put every saved value back
};

struct Line {
    bool         ok = false;
    std::wstring text;
};

struct Report {
    std::vector<Line> lines;
    int  changed = 0;
    int  failed = 0;
};

// Writes exactly what the plan says: selected values are written, unselected
// ones are removed so an apply is also a "put back" for anything unchecked.
Report Apply(const Plan& plan);

// Removes every value this program owns, for the named adapter when given.
Report Restore(const std::wstring& adapter);

struct Current {
    bool throttle = false;
    bool ipv6 = false;
    bool scheduler = false;
    bool timeWait = false;
    bool nagle = false;
    int  dns = 0;
    std::vector<std::wstring> servers;
};

Current ReadState(const std::wstring& adapter);

bool FlushDns();

// ---------------------------------------------------------------------------
//  Moving a plan between processes
// ---------------------------------------------------------------------------

// "adapter|dns|nagle|throttle|ipv6" - what the elevated copy receives.
std::wstring EncodePlan(const Plan& plan);
bool DecodePlan(const std::wstring& text, Plan* plan);

// One sentence per change the plan would make, in a fixed order.
std::vector<std::wstring> Describe(const Plan& plan);

// ---------------------------------------------------------------------------
//  Result file exchanged with the elevated copy
// ---------------------------------------------------------------------------

std::wstring DefaultResultPath();
bool WriteResultFile(const std::wstring& path, const Report& report);
bool ReadResultFile(const std::wstring& path, Report* report);

// ---------------------------------------------------------------------------
//  Console and command-line support
// ---------------------------------------------------------------------------

struct TestResult {
    const wchar_t* name;
    bool           passed;
    std::wstring   detail;
};

std::vector<TestResult> RunSelfTest();

bool IsElevated();

const wchar_t* CopyrightText();
const wchar_t* VersionText();

}  // namespace itw
