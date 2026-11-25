// Internet Connection Utility
// Copyright (c) 2026-2027 Charles McDonald. BSD 3-Clause License.
// Part of Zanphar's WinSuite; see LICENSE.md in the suite root.

// Internet Connection Utility - network primitives (Win32, no third-party dependencies)
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <wlanapi.h>
#include <winhttp.h>

#include <string>
#include <vector>
#include <cstdint>

namespace net {

bool init();          // idempotent WSAStartup; false on failure
double nowMs();       // monotonic milliseconds
std::string wstr2utf8(const std::wstring& s);
std::wstring utf82wstr(const std::string& s);
std::string trim(const std::string& s);

// ---------------------------------------------------------------- latency
struct PingStats {
    int sent = 0, recv = 0, lossPct = 0;
    double minMs = 0, maxMs = 0, avgMs = 0, jitterMs = 0;
    std::vector<double> samples;
    bool reachable = false;
};

bool pingOnce(const std::string& host, int timeoutMs, double& rttOut);
PingStats pingSeries(const std::string& host, int count, int intervalMs, int timeoutMs);

// ---------------------------------------------------------------- dns
struct DnsProbe {
    std::string server;
    bool responded = false;
    double ms = 0;
    int rcode = -1;
};
DnsProbe probeDns(const std::string& server, const char* qname, int timeoutMs = 1600);

struct ResolveRes {
    bool ok = false;
    double ms = 0;
    std::vector<std::string> v4;
    int err = 0;
};
ResolveRes resolve(const char* name, int timeoutMs = 2500);

// ---------------------------------------------------------------- http / tcp
struct HttpRes {
    bool ok = false;
    int status = 0;
    double ms = 0;
    std::string body;
    long long bytes = 0;
    std::string err;
    double mbps = 0;
};
HttpRes httpGet(const std::wstring& host, const std::wstring& path, bool secure,
                int timeoutMs, int maxBody = 1024);
HttpRes httpDownload(const std::wstring& host, const std::wstring& path, bool secure,
                     int timeoutMs, int maxBytes);

struct TcpRes {
    bool ok = false;
    double ms = 0;
    std::string err;
};
TcpRes tcpConnect(const char* host, int port, int timeoutMs = 2500);

// ---------------------------------------------------------------- adapters
struct Adapter {
    std::wstring name, desc, guid, mac, driverProvider, driverDate;
    std::string ipv4, mask, gateway;
    std::vector<std::string> dns;
    bool valid = false, up = false, dhcp = false, isWifi = false, isVirtual = false;
    bool apipa = false, autoMetric = true;
    ULONG ifIndex = 0;
    ULONG metric = 0;
    ULONG mtu = 0;
    ULONG64 speedBps = 0;
    double leaseHoursLeft = -1;
    int ifType = 0;
};

bool detectAdapter(Adapter& a);
bool interfaceStats(ULONG ifIndex, uint64_t& inErr, uint64_t& outErr,
                   uint64_t& inDisc, uint64_t& outDisc);
int defaultRouteCount();
std::vector<std::string> duplicateIps();

struct WifiNeighbor {
    std::wstring ssid;
    int rssi = -999;
    int channel = -1;
    bool band5 = false, band6 = false;
    int quality = 0;
    std::wstring auth;
};

struct Wifi {
    bool supported = false, connected = false, radioOn = true;
    std::wstring ssid, bssid, phy, auth, cipher, profile;
    int signalPct = -1, rssi = -999, channel = -1;
    bool band5 = false, band6 = false;
    ULONG rxKbps = 0, txKbps = 0;
    std::wstring stateText;
};

bool wifiCurrent(Wifi& w);
bool wifiNeighbors(std::vector<WifiNeighbor>& out);
void channelInfo(double centerMhz, int& channel, bool& is5, bool& is6);

// ---------------------------------------------------------------- services
struct SvcState {
    bool exists = false;
    DWORD state = 0;
    DWORD startType = 0;
};
SvcState serviceState(const wchar_t* name);
bool serviceAuto(const wchar_t* name);
bool serviceStart(const wchar_t* name);

// ---------------------------------------------------------------- registry
bool regGetDword(HKEY root, const std::wstring& path, const wchar_t* name, DWORD& out);
bool regGetStr(HKEY root, const std::wstring& path, const wchar_t* name, std::wstring& out);
bool regSetDword(HKEY root, const std::wstring& path, const wchar_t* name, DWORD v);
bool regDeleteValue(HKEY root, const std::wstring& path, const wchar_t* name);
bool regDeleteKeyTree(HKEY root, const std::wstring& path);

// ---------------------------------------------------------------- process
struct CmdRes {
    bool started = false;
    int code = -1;
    std::string out;
};
CmdRes run(const std::wstring& cmdline, int timeoutMs = 25000);
bool elevated();
bool relaunchAsAdmin();
std::wstring activeAdapterRegKey();   // class subkey of the active NIC, empty if unknown
std::wstring nicClassKeyForGuid(const std::wstring& guid);

// ---------------------------------------------------------------- misc
struct ProxyInfo {
    bool enabled = false;
    std::wstring server, pac;
};
ProxyInfo proxyInfo();
bool disableUserProxy();

uint64_t tcpRetrans(uint64_t& outSegs);
std::wstring hostsFile();

// Repairs queued before a UAC relaunch, applied by the elevated instance.
std::wstring pendingRepairsPath();
bool savePendingRepairs(const std::vector<std::string>& ids);
std::vector<std::string> loadPendingRepairs();
void clearPendingRepairs();
std::wstring computerName();
std::wstring osVersion();
bool isIpLiteral(const std::string& s);
std::wstring fmt1(double v);
std::wstring fmt0(double v);

} // namespace net
