// Internet Connection Utility
// Copyright (c) 2026-2027 Charles McDonald. BSD 3-Clause License.
// Part of Zanphar's WinSuite; see LICENSE.md in the suite root.

// Internet Connection Utility - diagnostic engine and remediation catalogue
#pragma once

#include "net.h"
#include <mutex>
#include <string>
#include <vector>

enum class Sev { Pass = 0, Info = 1, Warn = 2, Critical = 3, Running = 4, Skip = 5 };

const wchar_t* sevLabel(Sev s);
int sevColor(Sev s);

struct Issue {
    std::string id;
    std::wstring title;      // short name
    std::wstring detail;     // what was measured
    std::wstring advice;     // what it means / what to do
    std::vector<std::wstring> steps; // manual remediation steps
    std::wstring metric;     // headline value shown right-aligned
    Sev sev = Sev::Skip;
    std::string fixId;       // empty when nothing can be applied
    std::wstring fixLabel;
    bool admin = false;
    bool risky = false;
    bool reboot = false;
    bool tested = false;     // check actually ran
    bool fixedDuringRun = false;
};

struct FixDef {
    std::string id;
    std::wstring name;
    std::wstring desc;
    std::wstring commandSummary;
    bool admin = false;
    bool risky = false;
    bool reboot = false;
};

struct Metric {
    std::wstring label, value, unit;
    Sev sev = Sev::Info;
    double raw = 0;
};

struct LogEntry {
    std::wstring time, text;
    int level = 0; // 0 info, 1 good, 2 warn, 3 bad
};

struct AppState {
    bool scanning = false, fixing = false, cancelled = false;
    float progress = 0.f;
    std::wstring phase;
    std::wstring headline;
    int score = 0;
    int nPass = 0, nWarn = 0, nCrit = 0;
    std::vector<Issue> issues;
    std::vector<Metric> metrics;
    std::vector<LogEntry> logs;
    std::vector<double> spark;       // latency samples ms, 0 = lost
    net::Adapter adapter;
    net::Wifi wifi;
    std::vector<net::WifiNeighbor> neighbors;
    std::vector<FixDef> fixes;
    bool adminMode = false;
    std::wstring scannedAt;
    bool hasResult = false;
    ULONG pathMtu = 0;              // largest unfragmented packet measured this run
    std::wstring fixNote;           // outcome of the last repair batch, for a toast
    int fixLevel = 1;
    unsigned long long fixCounter = 0;
};

class Doctor {
public:
    Doctor();
    void startScan(HWND notify);
    void runFix(const std::string& fixId, HWND notify);
    // Applies an explicit queue (used to resume repairs after an elevated relaunch).
    void runFixList(const std::vector<std::string>& ids, HWND notify);
    // skipAdminFixes applies only repairs that work without elevation.
    void fixAll(HWND notify, bool skipAdminFixes = false);
    void cancel();
    AppState state();
    bool busy();
    void log(const std::wstring& text, int level);

private:
    std::mutex mu_;
    AppState st_;
    bool scanRunning_ = false;
    bool fixRunning_ = false;

    void scanWorker();
    void fixWorker(std::vector<std::string> ids, HWND notify);
    void emit(HWND h);
    void addLocked(const Issue& i);
    void setPhase(const std::wstring& p, float progress);
    void postMetrics();
    void finishScan(int scoreBase);
    std::wstring summarize();
};
