// ============================================================================
//  pcspeedtest.h — the measurement engine PCSpeedTest's window and its console
//  modes share. One catalog of fifteen metrics in five groups, one entry point
//  per metric, and the machine description and CSV writer that make a run
//  comparable with the last one.
// ============================================================================

#pragma once

// Lean and mean keeps windows.h from pulling winsock.h in behind the winsock2.h
// that the loopback and ping measurements need.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace pt {

// The four groups the window draws headings for, in catalog order.
enum class Group { Cpu = 0, Memory, Disk, Network, Graphics };

struct Metric {
    const wchar_t* name;    // what is measured
    const wchar_t* detail;  // how it is measured, in the row's second line
    const wchar_t* unit;    // printed beside the value
    Group          group;
    const char*    key;     // CSV column name, stable across releases
};

// Fifteen metrics: two CPU, two memory, two disk, six network, three graphics.
const std::vector<Metric>& Metrics();
const wchar_t*             GroupName(Group group);

// Which clock produced a timed figure. The graphics rows use the GPU's own
// timestamp queries; a build that had to fall back to elapsed CPU-side time
// must say so, because the two clocks do not measure the same thing.
enum class TimeSource { Unknown = 0, GpuTimestamp, WallClock };

struct Reading {
    bool         ok = false;
    double       value = 0.0;
    std::wstring note;  // why it could not be measured, or a caveat when it was
    TimeSource   timing = TimeSource::Unknown;  // set by the graphics rows
};

// What the machine is, gathered once per run: the CSV row needs it to make two
// readings comparable, and the window prints it under the title.
struct Machine {
    std::wstring cpu;
    int          cores = 1;          // logical processors: the CSV's `cores`
    int          physicalCores = 1;  // cores, from RelationProcessorCore
    double       ramGiB = 0.0;
    std::wstring os;      // corrected name, edition, version and build
    std::wstring arch;    // x64 / ARM64 / x86
    std::wstring gpu;     // the adapter attached to the desktop
    std::wstring driver;  // its driver version, empty when the key has none
    std::wstring ramSpeed;  // "3200 MT/s" from SMBIOS, empty when unreadable
    std::wstring ramMaker;  // the module maker, "mixed" when they differ
    std::wstring link;    // the interface carrying the default route
    double       linkGbps = 0.0;
    std::string  hostUtf8;  // machine name, UTF-8 for the CSV
};

Machine Describe();
// The adapters Windows reports for the default route, refreshed per run so a
// Wi-Fi to Ethernet switch is not reported with the previous link speed.
void RefreshLink(Machine* machine);

struct Update {
    int     index = -1;
    Reading reading;
    bool    finished = false;  // the last update of a run
};

using Progress = std::function<void(const Update&)>;
using Tick     = std::function<bool()>;  // false cancels the run between blocks

// One metric by catalog index. Never throws: a metric that cannot be measured
// comes back ok=false with the reason in note, so one blocked test (ICMP
// filtered, no disk space) does not lose the other eleven.
Reading Measure(int index, const Machine& machine, const Tick& tick);

// Every metric in catalog order, reporting each as it lands.
void MeasureAll(const Machine& machine, const Progress& progress, const Tick& tick);

// "812.4" and the metric's unit: the value as the window and the console print
// it. Kept in one place so the two never drift.
std::wstring FormatValue(const Reading& reading, const Metric& metric);

// %LOCALAPPDATA%\PCSpeedTest\results.csv
std::wstring ResultsPath();
bool         AppendCsv(const Machine& machine, const std::vector<Reading>& readings,
                       std::wstring* error);

// The same append over a path the caller names. A file written under an older
// header is migrated in place first - one line per run, so the rewrite is cheap
// and keeps every reading that was already there.
bool AppendCsvTo(const std::wstring& path, const Machine& machine,
                 const std::vector<Reading>& readings, std::wstring* error);

// Throws away every run and leaves the header, so the file is empty without
// losing the schema the next append writes against.
bool EmptyCsv(std::wstring* error);
bool EmptyCsvTo(const std::wstring& path, std::wstring* error);

// One line of results.csv, read back. The catalogue columns are matched by
// their key, not by position, so a file written by an older build still loads.
struct HistoryRow {
    std::string         timestamp;
    std::string         host;
    std::string         cpu;
    std::string         os;
    int                 cores = 0;    // physical, 0 when an older row did not say
    int                 threads = 0;  // logical
    double              ramGiB = 0.0;
    std::vector<double> values;   // catalog order
    std::vector<bool>   present;  // false where the column was left empty
};

// The whole file, oldest first. A missing file is not an error: it comes back
// empty with the reason in message, which is what the viewer shows.
std::vector<HistoryRow> LoadHistory(std::wstring* message);

// The same reader over a path the caller names, so the self-test can prove the
// parsing against a file of its own instead of the one the user keeps.
std::vector<HistoryRow> LoadHistoryFrom(const std::wstring& path, std::wstring* message);

struct TestResult {
    std::wstring name;
    bool         pass = false;
    std::wstring detail;
};

// The engine's own checks, run by --selftest. No network traffic and no file
// left behind: each check either proves a measurement is plausible or proves a
// helper that the CSV depends on.
std::vector<TestResult> SelfTests();

}  // namespace pt
