// ============================================================================
//  pcspeedtest.cpp — the measurements behind PCSpeedTest.
//
//  Every metric here is measured the same way twice: a fixed amount of work, a
//  monotonic clock around it, and the rate reported in the unit the catalog
//  names. Nothing is read from a previous run and nothing is scaled, so two
//  runs on the same machine differ only by what the machine did.
//
//  A metric that cannot be measured - no reply to ICMP, no disk room, a socket
//  refused, no Direct3D device - comes back ok=false with the reason in note.
//  One blocked test never costs the rest.
// ============================================================================

#include <winsock2.h>
#include <ws2tcpip.h>

#include "pcspeedtest.h"

// icmpapi.h leans on the winsock and IP helper types, so it comes last.
#include <ipexport.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <icmpapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <shlobj.h>
#include <winhttp.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace pt {

namespace {

// ---------------------------------------------------------------------------
//  Budgets. The whole run is a handful of seconds; each figure is a rate over
//  a fixed slice, not over the wall time of the run.
// ---------------------------------------------------------------------------
constexpr double kCpuWarmupSeconds = 0.05;
constexpr double kCpuSeconds = 0.35;
constexpr double kMemorySeconds = 0.30;
constexpr double kChaseSeconds = 0.25;

constexpr double kGpuSeconds = 0.5;           // each graphics rate runs this long
constexpr UINT   kGpuLanes = 1u << 18;        // 262144 shader invocations per dispatch
constexpr UINT   kGpuIterations = 256;        // four multiply-adds each
constexpr UINT   kGpuSide = 4096;             // 4096 x 4096 RGBA8 = 64 MiB
constexpr UINT   kGpuFrameWidth = 1920;       // offscreen scene target
constexpr UINT   kGpuFrameHeight = 1080;
constexpr UINT   kGpuFrameQuads = 4096;       // blended quads in every frame

constexpr size_t kCopyBytes = 64ull << 20;     // 64 MiB source and destination
constexpr size_t kChaseBytes = 32ull << 20;    // 8 M random hops
constexpr size_t kDiskBytes = 192ull << 20;    // 192 MiB written and read back
constexpr size_t kDiskChunk = 1ull << 20;
constexpr size_t kDiskSelfTestBytes = 48ull << 20;

constexpr size_t kLoopbackBytes = 32ull << 20;
constexpr size_t kLoopbackChunk = 64ull << 10;

constexpr size_t kWanDownBytes = 40ull << 20;
constexpr double kWanDownSeconds = 12.0;
constexpr size_t kWanUpBytes = 12ull << 20;
constexpr double kWanUpSeconds = 12.0;

constexpr double kNetTimeoutSeconds = 5.0;

// The nearest thing to a target this tool can assume: Cloudflare's open
// speed endpoints, and a public resolver for the WAN latency.
constexpr wchar_t kSpeedHost[] = L"speed.cloudflare.com";
constexpr wchar_t kLatencyHost[] = L"1.1.1.1";

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------

double Now() {
    static const double frequency = [] {
        LARGE_INTEGER value = {};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER value = {};
    QueryPerformanceCounter(&value);
    return static_cast<double>(value.QuadPart) / frequency;
}

std::wstring FormatDouble(double value, int decimals) {
    if (!std::isfinite(value)) return L"-";
    wchar_t buffer[64] = {};
    _snwprintf_s(buffer, _TRUNCATE, L"%.*f", decimals, value);
    return buffer;
}

std::wstring Wide(const char* utf8) {
    if (!utf8 || !*utf8) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (length <= 1) return std::wstring();
    std::wstring text(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, text.data(), length);
    return text;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr,
                                           nullptr);
    if (length <= 1) return std::string();
    std::string utf8(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), length, nullptr, nullptr);
    return utf8;
}

// Firmware string sets are usually ASCII and occasionally UTF-8; anything that
// is neither is still better read through the ANSI code page than dropped.
std::wstring Wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    UINT codePage = CP_UTF8;
    int  length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(),
                                      static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        codePage = CP_ACP;
        length = MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()),
                                     nullptr, 0);
    }
    if (length <= 0) return std::wstring();
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codePage, 0, text.c_str(), static_cast<int>(text.size()), result.data(),
                        length);
    return result;
}

std::wstring RegistryString(HKEY root, const wchar_t* path, const wchar_t* value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return std::wstring();
    wchar_t buffer[512] = {};
    DWORD   bytes = sizeof buffer;
    DWORD   type = 0;
    std::wstring result;
    if (RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &bytes) ==
            ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        result.assign(buffer, bytes / sizeof(wchar_t));
        while (!result.empty() && result.back() == L'\0') result.pop_back();
    }
    RegCloseKey(key);
    return result;
}

std::wstring Trim(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) ++begin;
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t')) --end;
    return text.substr(begin, end - begin);
}

unsigned CpuCount() {
    const unsigned reported = std::thread::hardware_concurrency();
    if (reported == 0) return 1;
    return reported > 64 ? 64 : reported;
}

DWORD RegistryDword(HKEY root, const wchar_t* path, const wchar_t* value, DWORD fallback) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return fallback;
    DWORD data = 0;
    DWORD bytes = sizeof data;
    DWORD type = 0;
    const bool ok = RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(&data),
                                     &bytes) == ERROR_SUCCESS &&
                    type == REG_DWORD && bytes == sizeof data;
    RegCloseKey(key);
    return ok ? data : fallback;
}

// Physical cores as Windows groups them, which is not the logical processor
// count: an i7-6700K is four cores and eight threads, and reporting "8 cores"
// for it is the same kind of wrong answer as calling Windows 11 "Windows 10".
int PhysicalCoreCount() {
    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return 0;
    std::vector<BYTE> buffer(length);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                                              buffer.data()),
                                          &length))
        return 0;
    int    cores = 0;
    size_t offset = 0;
    // Only the two DWORDs at the start of each entry are a fixed size; the
    // entry itself grows with its group mask, and comparing the offset against
    // sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) skips the last core.
    while (offset + sizeof(DWORD) * 2 <= length) {
        auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            buffer.data() + offset);
        if (entry->Relationship == RelationProcessorCore) ++cores;
        if (entry->Size == 0) break;
        offset += entry->Size;
    }
    return cores;
}

// The name Windows reports and the name the machine actually runs are not the
// same string: Windows 11 keeps "Windows 10" in ProductName for compatibility,
// so only the build number separates the two. 22000 is the first Windows 11
// build; anything below it, including the whole 19041-19045 range, is 10.
std::wstring DescribeWindows() {
    const wchar_t*     key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const std::wstring product = Trim(RegistryString(HKEY_LOCAL_MACHINE, key, L"ProductName"));
    const std::wstring buildText =
        Trim(RegistryString(HKEY_LOCAL_MACHINE, key, L"CurrentBuildNumber"));
    const int   build = buildText.empty() ? 0 : _wtoi(buildText.c_str());
    const DWORD ubr = RegistryDword(HKEY_LOCAL_MACHINE, key, L"UBR", 0);
    const std::wstring display = Trim(RegistryString(HKEY_LOCAL_MACHINE, key, L"DisplayVersion"));

    const bool server = product.find(L"Server") != std::wstring::npos;

    std::wstring name;
    if (product.empty()) {
        name = build >= 22000 ? L"Windows 11" : L"Windows 10";
    } else if (server) {
        name = product;  // "Windows Server 2022" already names its version
    } else {
        std::wstring edition = product;
        for (const wchar_t* prefix : {L"Windows 11 ", L"Windows 10 "}) {
            const std::wstring text(prefix);
            if (edition.rfind(text, 0) == 0) {
                edition = edition.substr(text.size());
                break;
            }
        }
        if (edition == L"Windows 11" || edition == L"Windows 10") edition.clear();
        name = build >= 22000 ? L"Windows 11" : L"Windows 10";
        if (!edition.empty()) name += L" " + edition;
    }

    if (!display.empty()) name += L" " + display;
    if (build > 0) {
        name += L" (build " + std::to_wstring(build);
        if (ubr > 0) name += L"." + std::to_wstring(ubr);
        name += L")";
    }
    return name;
}

std::wstring ProcessorArchitecture() {
    SYSTEM_INFO info = {};
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return L"x64";
        case PROCESSOR_ARCHITECTURE_ARM64: return L"ARM64";
        case PROCESSOR_ARCHITECTURE_INTEL: return L"x86";
        case PROCESSOR_ARCHITECTURE_ARM: return L"ARM";
        default: return L"unknown architecture";
    }
}

// ---------------------------------------------------------------------------
//  The rest of the hardware: the adapter Windows draws with, its driver, and the
//  memory modules' speed and maker. All of it is optional - a machine can refuse
//  the firmware table, a session can be remote, a driver key can be absent - so a
//  figure Windows does not report stays empty rather than becoming a guess.
// ---------------------------------------------------------------------------

std::wstring DriverVersionForAdapter(const std::wstring& name) {
    // One subkey per installed adapter; the one whose DriverDesc is the adapter
    // being drawn on is the one whose driver is loaded.
    const wchar_t* base =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    std::wstring firstVersion;
    for (int i = 0; i < 16; ++i) {
        wchar_t sub[8] = {};
        swprintf(sub, 8, L"\\%04d", i);
        const std::wstring key = std::wstring(base) + sub;
        const std::wstring desc =
            Trim(RegistryString(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverDesc"));
        const std::wstring version =
            Trim(RegistryString(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverVersion"));
        if (desc.empty() || version.empty()) continue;
        if (desc == name) return version;
        if (firstVersion.empty()) firstVersion = version;
    }
    return firstVersion;
}

// The adapter attached to the desktop, which is the one the machine draws on.
std::wstring GraphicsAdapter(std::wstring* driver) {
    for (DWORD i = 0; i < 16; ++i) {
        DISPLAY_DEVICEW device = {};
        device.cb = sizeof device;
        if (!EnumDisplayDevicesW(nullptr, i, &device, 0)) break;
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        const std::wstring name = Trim(device.DeviceString);
        if (name.empty()) continue;
        *driver = DriverVersionForAdapter(name);
        return name;
    }
    return std::wstring();
}

struct MemoryReport {
    std::wstring speed;  // "3200 MT/s", or "2666-3200 MT/s" when they differ
    std::wstring maker;  // the module maker, "mixed" when they differ
};

// Type 17 of the SMBIOS table is one memory device: its size says whether the
// slot is populated, its configured speed is the rate the platform runs it at,
// and its manufacturer is an index into the structure's string set rather than
// text. Taking the table as bytes keeps the parse testable without hardware.
MemoryReport ParseMemoryDevices(const std::vector<BYTE>& table) {
    MemoryReport             report;
    std::vector<unsigned>    speeds;
    std::vector<std::string> makers;
    if (table.size() < 8) return report;  // the raw table's own header comes first

    size_t at = 8;
    while (at + 4 <= table.size()) {
        const BYTE type = table[at];
        const BYTE length = table[at + 1];
        if (type == 127 || length < 4 || at + length > table.size()) break;

        if (type == 17 && length >= 0x18) {
            const BYTE* field = table.data() + at;
            const WORD  size = static_cast<WORD>(field[0x0C] | (field[0x0D] << 8));
            const WORD  rated = static_cast<WORD>(field[0x15] | (field[0x16] << 8));
            const BYTE  makerIndex = field[0x17];
            // SMBIOS 2.7 added the configured speed, which is what the module
            // actually runs at; the rated figure is the fallback.
            const WORD configured = length >= 0x22
                                        ? static_cast<WORD>(field[0x20] | (field[0x21] << 8))
                                        : 0;
            const WORD speed = configured >= 100 ? configured : rated;

            std::vector<std::string> strings;
            size_t                   stop = at + length;
            while (stop < table.size() && table[stop] != 0) {
                size_t end = stop;
                while (end < table.size() && table[end] != 0) ++end;
                strings.push_back(std::string(table.begin() + stop, table.begin() + end));
                stop = end + 1;
            }

            if (size != 0 && size != 0xFFFF && speed >= 100) {
                speeds.push_back(speed);
                if (makerIndex >= 1 && makerIndex <= strings.size())
                    makers.push_back(strings[makerIndex - 1]);
            }
        }

        // The formatted area, then the string set, which ends with a double NUL.
        size_t next = at + length;
        while (next + 1 < table.size() && !(table[next] == 0 && table[next + 1] == 0)) ++next;
        at = next + 2;
    }

    if (speeds.empty()) return report;
    unsigned lowest = speeds[0];
    unsigned highest = speeds[0];
    for (unsigned value : speeds) {
        if (value < lowest) lowest = value;
        if (value > highest) highest = value;
    }
    report.speed = lowest == highest
                       ? std::to_wstring(highest) + L" MT/s"
                       : std::to_wstring(lowest) + L"-" + std::to_wstring(highest) + L" MT/s";

    bool sameMaker = !makers.empty();
    for (const std::string& maker : makers)
        if (maker != makers[0]) sameMaker = false;
    if (!makers.empty()) report.maker = sameMaker ? Wide(makers[0]) : L"mixed";
    return report;
}

// Windows hands the raw SMBIOS table to any caller that asks; a build that
// refuses simply leaves the fields empty.
MemoryReport ReadMemoryReport() {
    // 'RSMB' as the API spells it, written out because a multi-character literal
    // is an implementation-defined int in C++ and gcc rightly warns about it.
    constexpr DWORD kRawSmbios = 0x52534D42;
    const DWORD     size = GetSystemFirmwareTable(kRawSmbios, 0, nullptr, 0);
    if (size < 8) return MemoryReport{};
    std::vector<BYTE> table(size);
    const DWORD       got = GetSystemFirmwareTable(kRawSmbios, 0, table.data(), size);
    if (got < 8) return MemoryReport{};
    table.resize(got);
    return ParseMemoryDevices(table);
}

// ---------------------------------------------------------------------------
//  CPU: one churn loop, run on one thread or on every thread.
//
//  The loop carries a dependency from one iteration to the next in both the
//  integer and the floating point path, so the compiler cannot vectorise it
//  away or overlap it with itself: the count is work actually done.
// ---------------------------------------------------------------------------

volatile uint64_t g_cpuSink = 0;

// Returns the number of churn steps completed, not the state: the rate is the
// count over the clock, and the state only exists to keep the loop honest.
uint64_t ChurnFor(double seconds, const Tick& tick) {
    constexpr uint64_t kBlock = 1ull << 16;
    uint64_t           x = 0x9E3779B97F4A7C15ull;
    double             acc = 0.5;
    uint64_t           total = 0;
    const double       start = Now();
    for (;;) {
        for (uint64_t i = 0; i < kBlock; ++i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            acc = acc * 1.00000011920928955 + static_cast<double>(x & 0xFFFFu);
        }
        total += kBlock;
        g_cpuSink = x ^ static_cast<uint64_t>(acc);
        if (Now() - start >= seconds) break;
        if (tick && !tick()) break;
    }
    return total;
}

Reading MeasureCpuSingle(const Tick& tick, double seconds) {
    Reading reading;
    g_cpuSink ^= ChurnFor(kCpuWarmupSeconds, tick);

    const double   start = Now();
    const uint64_t ops = ChurnFor(seconds, tick);
    const double   elapsed = Now() - start;
    g_cpuSink ^= ops;

    if (elapsed <= 0.0 || ops == 0) {
        reading.note = L"the timing loop returned no work";
        return reading;
    }
    reading.ok = true;
    reading.value = static_cast<double>(ops) / 1e6 / elapsed;
    return reading;
}

Reading MeasureCpuMulti(const Tick& tick, double seconds) {
    Reading reading;
    const unsigned cores = CpuCount();
    if (cores <= 1) {
        reading.note = L"this machine reports a single logical processor";
        return reading;
    }

    std::vector<uint64_t> perThread(cores, 0);
    std::vector<std::thread> threads;
    threads.reserve(cores);

    const double start = Now();
    for (unsigned i = 0; i < cores; ++i)
        threads.emplace_back([&perThread, i, seconds, tick] {
            perThread[i] = ChurnFor(seconds, tick);
        });
    for (std::thread& thread : threads) thread.join();
    const double elapsed = Now() - start;

    uint64_t ops = 0;
    for (uint64_t value : perThread) {
        ops += value;
        g_cpuSink ^= value;
    }

    if (elapsed <= 0.0 || ops == 0) {
        reading.note = L"the timing loop returned no work";
        return reading;
    }
    reading.ok = true;
    reading.value = static_cast<double>(ops) / 1e6 / elapsed;
    reading.note = std::to_wstring(cores) + L" threads";
    return reading;
}

// ---------------------------------------------------------------------------
//  Memory: copy bandwidth, and the latency of one dependent load.
// ---------------------------------------------------------------------------

Reading MeasureMemoryCopy(const Tick& tick) {
    Reading reading;
    void*   source = VirtualAlloc(nullptr, kCopyBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void*   target = VirtualAlloc(nullptr, kCopyBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!source || !target) {
        if (source) VirtualFree(source, 0, MEM_RELEASE);
        if (target) VirtualFree(target, 0, MEM_RELEASE);
        reading.note = L"could not reserve 128 MiB";
        return reading;
    }
    std::memset(source, 0x5A, kCopyBytes);
    std::memset(target, 0xA5, kCopyBytes);

    // One untimed pass, so the measurement is not the first touch of either
    // buffer (page faults would read as slow memory).
    std::memcpy(target, source, kCopyBytes);

    const double start = Now();
    double       elapsed = 0.0;
    uint64_t     passes = 0;
    for (;;) {
        std::memcpy(target, source, kCopyBytes);
        ++passes;
        elapsed = Now() - start;
        if (elapsed >= kMemorySeconds) break;
        if (tick && !tick()) break;
    }

    VirtualFree(source, 0, MEM_RELEASE);
    VirtualFree(target, 0, MEM_RELEASE);

    if (passes == 0 || elapsed <= 0.0) {
        reading.note = L"no pass completed";
        return reading;
    }
    reading.ok = true;
    reading.value = static_cast<double>(passes) * static_cast<double>(kCopyBytes) / 1e9 / elapsed;
    return reading;
}

Reading MeasureMemoryLatency(const Tick& tick) {
    Reading          reading;
    const size_t     slots = kChaseBytes / sizeof(uint32_t);
    auto*            table = static_cast<uint32_t*>(
        VirtualAlloc(nullptr, slots * sizeof(uint32_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!table) {
        reading.note = L"could not reserve 32 MiB";
        return reading;
    }

    // A shuffled single-cycle permutation: the chase visits every slot once and
    // each hop depends on the one before it, so the clock measures memory, not
    // the loop.
    for (size_t i = 0; i < slots; ++i) table[i] = static_cast<uint32_t>((i + 1) % slots);
    uint64_t state = 0x2545F4914F6CDD1Dull;
    for (size_t i = slots - 1; i > 0; --i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const size_t j = static_cast<size_t>(state % (i + 1));
        const uint32_t swap = table[i];
        table[i] = table[j];
        table[j] = swap;
    }

    const double  start = Now();
    uint32_t      index = 0;
    uint64_t      hops = 0;
    const double  budget = kChaseSeconds;
    double        elapsed = 0.0;
    while (elapsed < budget) {
        for (int i = 0; i < 1 << 16; ++i) index = table[index];
        hops += 1u << 16;
        elapsed = Now() - start;
        if (tick && !tick()) break;
    }
    g_cpuSink ^= index;

    VirtualFree(table, 0, MEM_RELEASE);

    if (hops == 0 || elapsed <= 0.0) {
        reading.note = L"no hop completed";
        return reading;
    }
    reading.ok = true;
    reading.value = elapsed * 1e9 / static_cast<double>(hops);
    return reading;
}

// ---------------------------------------------------------------------------
//  Disk: sequential write with a flush, then a read that bypasses the cache.
// ---------------------------------------------------------------------------

bool EnsureDirectory(const std::wstring& path) {
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS)
        return true;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring WorkDirectory() {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) return std::wstring();
    std::wstring path = base;
    CoTaskMemFree(base);
    path += L"\\PCSpeedTest";
    return EnsureDirectory(path) ? path : std::wstring();
}

Reading MeasureDiskWrite(const Tick& tick, size_t bytes) {
    Reading          reading;
    const std::wstring directory = WorkDirectory();
    if (directory.empty()) {
        reading.note = L"no writable %LOCALAPPDATA%";
        return reading;
    }
    const std::wstring path = directory + L"\\speedtest.tmp";

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        reading.note = L"could not create " + path;
        return reading;
    }

    std::vector<char> chunk(kDiskChunk, 0x5A);
    const double      start = Now();
    size_t            written = 0;
    bool              failed = false;
    while (written < bytes) {
        DWORD wrote = 0;
        if (!WriteFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &wrote, nullptr) ||
            wrote == 0) {
            failed = true;
            break;
        }
        written += wrote;
        if (tick && !tick()) break;
    }
    // The flush is part of the figure: a write the drive has not been told to
    // keep is not a write the machine has done yet.
    if (!failed) FlushFileBuffers(file);
    const double elapsed = Now() - start;
    CloseHandle(file);

    if (failed || written == 0 || elapsed <= 0.0) {
        DeleteFileW(path.c_str());
        reading.note = failed ? L"the write failed" : L"no byte was written";
        return reading;
    }
    reading.ok = true;
    reading.value = static_cast<double>(written) / 1e6 / elapsed;
    reading.note = L"192 MiB to " + path;
    return reading;
}

Reading MeasureDiskRead(const Tick& tick, size_t bytes) {
    Reading          reading;
    const std::wstring directory = WorkDirectory();
    if (directory.empty()) {
        reading.note = L"no writable %LOCALAPPDATA%";
        return reading;
    }
    const std::wstring path = directory + L"\\speedtest-read.tmp";

    // Prepare the file untimed. The read below uses FILE_FLAG_NO_BUFFERING, so
    // the cache cannot answer it and the figure is the drive.
    {
        HANDLE prep = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (prep == INVALID_HANDLE_VALUE) {
            reading.note = L"could not create " + path;
            return reading;
        }
        std::vector<char> chunk(kDiskChunk, 0xA5);
        size_t            written = 0;
        while (written < bytes) {
            DWORD wrote = 0;
            if (!WriteFile(prep, chunk.data(), static_cast<DWORD>(chunk.size()), &wrote, nullptr) ||
                wrote == 0)
                break;
            written += wrote;
        }
        FlushFileBuffers(prep);
        CloseHandle(prep);
        if (written < bytes) {
            DeleteFileW(path.c_str());
            reading.note = L"could not prepare the test file";
            return reading;
        }
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_NO_BUFFERING, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DeleteFileW(path.c_str());
        reading.note = L"could not open the test file for reading";
        return reading;
    }

    void* buffer = VirtualAlloc(nullptr, static_cast<SIZE_T>(kDiskChunk), MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
    if (!buffer) {
        CloseHandle(file);
        DeleteFileW(path.c_str());
        reading.note = L"could not reserve a read buffer";
        return reading;
    }

    const double start = Now();
    size_t       read = 0;
    bool         failed = false;
    while (read < bytes) {
        DWORD got = 0;
        if (!ReadFile(file, buffer, static_cast<DWORD>(kDiskChunk), &got, nullptr)) {
            failed = true;
            break;
        }
        if (got == 0) break;
        read += got;
        if (tick && !tick()) break;
    }
    const double elapsed = Now() - start;

    VirtualFree(buffer, 0, MEM_RELEASE);
    CloseHandle(file);
    DeleteFileW(path.c_str());

    if (failed || read == 0 || elapsed <= 0.0) {
        reading.note = failed ? L"the read failed" : L"no byte was read";
        return reading;
    }
    reading.ok = true;
    reading.value = static_cast<double>(read) / 1e6 / elapsed;
    reading.note = L"192 MiB unbuffered";
    return reading;
}

// ---------------------------------------------------------------------------
//  Network: the link Windows reports, loopback throughput, and the two
//  latencies. Then the WAN rates over HTTPS.
// ---------------------------------------------------------------------------

bool NetInit() {
    static const bool ready = [] {
        WSADATA data = {};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

DWORD DefaultInterfaceIndex() {
    DWORD index = 0;
    const DWORD address = inet_addr("8.8.8.8");
    if (GetBestInterface(address, &index) != NO_ERROR) return 0;
    return index;
}

Reading MeasureLink(const Machine& machine) {
    Reading reading;
    if (machine.linkGbps > 0.0) {
        reading.ok = true;
        reading.value = machine.linkGbps;
        reading.note = machine.link;
        return reading;
    }
    reading.note = L"Windows reported no link speed for the default route";
    return reading;
}

std::wstring DefaultGatewayText() {
    const DWORD wanted = DefaultInterfaceIndex();
    if (wanted == 0) return std::wstring();

    ULONG size = 16 * 1024;
    std::vector<BYTE> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    // INCLUDE_GATEWAYS is what fills FirstGatewayAddress; without it every
    // adapter comes back with an empty gateway list and LAN latency would look
    // like a machine with no router.
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    if (result != NO_ERROR) return std::wstring();

    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->IfIndex != wanted) continue;
        for (auto* gateway = adapter->FirstGatewayAddress; gateway; gateway = gateway->Next) {
            if (gateway->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* address = reinterpret_cast<sockaddr_in*>(gateway->Address.lpSockaddr);
            wchar_t text[INET_ADDRSTRLEN] = {};
            if (InetNtopW(AF_INET, &address->sin_addr, text, INET_ADDRSTRLEN)) return text;
        }
    }
    return std::wstring();
}

Reading MeasureLoopback(const Tick& tick) {
    Reading reading;
    if (!NetInit()) {
        reading.note = L"Winsock did not start";
        return reading;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        reading.note = L"no TCP socket";
        return reading;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int length = sizeof address;
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 ||
        listen(listener, 1) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        closesocket(listener);
        reading.note = L"could not open a loopback listener";
        return reading;
    }

    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET ||
        connect(client, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        if (client != INVALID_SOCKET) closesocket(client);
        closesocket(listener);
        reading.note = L"could not connect to the loopback listener";
        return reading;
    }
    SOCKET server = accept(listener, nullptr, nullptr);
    closesocket(listener);
    if (server == INVALID_SOCKET) {
        closesocket(client);
        reading.note = L"the loopback connection was not accepted";
        return reading;
    }

    const DWORD timeoutMs = static_cast<DWORD>(kNetTimeoutSeconds * 1000);
    setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs),
               sizeof timeoutMs);
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs),
               sizeof timeoutMs);

    std::atomic<size_t> received{0};
    std::thread reader([&received, server] {
        std::vector<char> buffer(kLoopbackChunk);
        size_t            total = 0;
        while (total < kLoopbackBytes) {
            const int got = recv(server, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (got <= 0) break;
            total += static_cast<size_t>(got);
        }
        received = total;
    });

    std::vector<char> chunk(kLoopbackChunk, 0x5A);
    size_t            sent = 0;
    const double      start = Now();
    while (sent < kLoopbackBytes) {
        const int put = send(client, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (put <= 0) break;
        sent += static_cast<size_t>(put);
        if (Now() - start > kNetTimeoutSeconds) break;
        if (tick && !tick()) break;
    }
    shutdown(client, SD_SEND);
    reader.join();
    const double elapsed = Now() - start;

    closesocket(server);
    closesocket(client);

    if (received == 0 || elapsed <= 0.0) {
        reading.note = L"no byte crossed the loopback connection";
        return reading;
    }
    reading.ok = true;
    reading.value = static_cast<double>(received) / 1e6 / elapsed;
    reading.note = L"64 KiB blocks over 127.0.0.1";
    return reading;
}

Reading PingHost(const wchar_t* host, int echoes, const wchar_t* label) {
    Reading reading;
    if (!NetInit()) {
        reading.note = L"Winsock did not start";
        return reading;
    }

    IN_ADDR address = {};
    if (InetPtonW(AF_INET, host, &address) != 1) {
        reading.note = L"could not read the address " + std::wstring(host);
        return reading;
    }

    HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) {
        reading.note = L"ICMP is not available to this process";
        return reading;
    }

    char                     payload[32] = {};
    std::vector<unsigned char> reply(sizeof(ICMP_ECHO_REPLY) + sizeof payload + 8);
    std::vector<double>      samples;
    std::wstring             reason;
    for (int i = 0; i < echoes; ++i) {
        const DWORD count = IcmpSendEcho(icmp, address.S_un.S_addr, payload, sizeof payload,
                                         nullptr, reply.data(),
                                         static_cast<DWORD>(reply.size()), 700);
        if (count == 0) {
            const DWORD error = GetLastError();
            if (reason.empty())
                reason = L"no reply from " + std::wstring(host) + L" (error " +
                         std::to_wstring(error) + L", ICMP may be filtered)";
            continue;
        }
        auto* echo = reinterpret_cast<ICMP_ECHO_REPLY*>(reply.data());
        if (echo->Status == IP_SUCCESS) samples.push_back(static_cast<double>(echo->RoundTripTime));
    }
    IcmpCloseHandle(icmp);

    if (samples.empty()) {
        reading.note = reason.empty() ? L"no reply to any echo" : reason;
        return reading;
    }

    double total = 0.0;
    for (double sample : samples) total += sample;
    const double average = total / static_cast<double>(samples.size());

    double jitter = 0.0;
    if (samples.size() > 1) {
        double swing = 0.0;
        for (size_t i = 1; i < samples.size(); ++i) swing += std::fabs(samples[i] - samples[i - 1]);
        jitter = swing / static_cast<double>(samples.size() - 1);
    }

    const int lost = echoes - static_cast<int>(samples.size());
    reading.ok = true;
    reading.value = average;
    reading.note = std::wstring(label) + L" - jitter " + FormatDouble(jitter, 1) + L" ms";
    // IcmpSendEcho reports whole milliseconds, so a LAN reply inside the same
    // millisecond reads as zero. Say so rather than implying an exact 0 ms.
    if (average == 0.0) reading.note += L", under ICMP's 1 ms resolution";
    if (lost > 0)
        reading.note += L", " + std::to_wstring(lost) + L" of " + std::to_wstring(echoes) +
                        L" lost";
    return reading;
}

// One HTTPS exchange with a byte cap and a time cap; the caller decides whether
// the bytes travel up or down.
bool OpenSpeedRequest(const wchar_t* verb, const wchar_t* path, size_t offer, HINTERNET* session,
                      HINTERNET* connection, HINTERNET* request) {
    *session = WinHttpOpen(L"PCSpeedTest/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!*session) return false;
    WinHttpSetTimeouts(*session, 5000, 5000, 8000, 15000);

    *connection = WinHttpConnect(*session, kSpeedHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!*connection) {
        WinHttpCloseHandle(*session);
        *session = nullptr;
        return false;
    }

    *request = WinHttpOpenRequest(*connection, verb, path, nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!*request) {
        WinHttpCloseHandle(*connection);
        WinHttpCloseHandle(*session);
        *connection = nullptr;
        *session = nullptr;
        return false;
    }

    const wchar_t* headers = verb && verb[0] == L'P' ? L"Content-Type: application/octet-stream\r\n"
                                                    : WINHTTP_NO_ADDITIONAL_HEADERS;
    const BOOL sent = WinHttpSendRequest(*request, headers,
                                         headers == WINHTTP_NO_ADDITIONAL_HEADERS ? 0 : (DWORD)-1L,
                                         WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(offer), 0);
    if (!sent) {
        WinHttpCloseHandle(*request);
        WinHttpCloseHandle(*connection);
        WinHttpCloseHandle(*session);
        *request = nullptr;
        *connection = nullptr;
        *session = nullptr;
        return false;
    }
    return true;
}

void CloseSpeedRequest(HINTERNET session, HINTERNET connection, HINTERNET request) {
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
}

Reading MeasureDownload(const Tick& tick) {
    Reading   reading;
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;

    wchar_t path[128] = {};
    _snwprintf_s(path, _TRUNCATE, L"/__down?bytes=%zu", kWanDownBytes);

    if (!OpenSpeedRequest(L"GET", path, 0, &session, &connection, &request)) {
        reading.note = L"could not reach " + std::wstring(kSpeedHost);
        return reading;
    }

    // The response has to be received before the first read; reading a request
    // whose headers are still in flight fails with a connection error.
    if (!WinHttpReceiveResponse(request, nullptr)) {
        CloseSpeedRequest(session, connection, request);
        reading.note = L"no response from " + std::wstring(kSpeedHost);
        return reading;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof status;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 0 && status != 200 && status != 206) {
        CloseSpeedRequest(session, connection, request);
        reading.note = L"the server answered HTTP " + std::to_wstring(status);
        return reading;
    }

    std::vector<char> buffer(64 * 1024);
    size_t            total = 0;
    const double      start = Now();
    double            elapsed = 0.0;
    bool              failed = false;
    bool              capped = false;
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) {
            failed = true;
            break;
        }
        if (got == 0) break;
        total += got;
        elapsed = Now() - start;
        if (total >= kWanDownBytes) break;
        if (elapsed >= kWanDownSeconds) {
            capped = true;
            break;
        }
        if (tick && !tick()) break;
    }
    if (elapsed <= 0.0) elapsed = Now() - start;
    CloseSpeedRequest(session, connection, request);

    if (total == 0 || elapsed <= 0.0) {
        reading.note = failed ? L"the download failed part way" : L"no byte arrived";
        return reading;
    }
    reading.ok = true;
    reading.value = static_cast<double>(total) * 8.0 / 1e6 / elapsed;
    reading.note = L"Cloudflare, " + FormatDouble(static_cast<double>(total) / (1024.0 * 1024.0), 1) +
                   L" MiB" + (capped ? L", capped at 12 s" : L"");
    return reading;
}

Reading MeasureUpload(const Tick& tick) {
    Reading   reading;
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;

    if (!OpenSpeedRequest(L"POST", L"/__up", kWanUpBytes, &session, &connection, &request)) {
        reading.note = L"could not reach " + std::wstring(kSpeedHost);
        return reading;
    }

    std::vector<char> chunk(64 * 1024, 0x5A);
    size_t            sent = 0;
    const double      start = Now();
    double            elapsed = 0.0;
    bool              failed = false;
    while (sent < kWanUpBytes) {
        DWORD put = 0;
        if (!WinHttpWriteData(request, chunk.data(), static_cast<DWORD>(chunk.size()), &put) ||
            put == 0) {
            failed = true;
            break;
        }
        sent += put;
        elapsed = Now() - start;
        if (elapsed >= kWanUpSeconds) break;
        if (tick && !tick()) break;
    }
    // The rate is the rate the body left this machine; waiting for the response
    // here only confirms the server took it.
    const bool responded = WinHttpReceiveResponse(request, nullptr) != FALSE;
    if (elapsed <= 0.0) elapsed = Now() - start;
    CloseSpeedRequest(session, connection, request);

    if (sent == 0 || elapsed <= 0.0 || !responded) {
        if (sent == 0)
            reading.note = failed ? L"the first write was refused" : L"no byte was sent";
        else if (failed)
            reading.note = L"the upload failed part way";
        else if (!responded)
            reading.note = L"the server did not confirm the upload";
        else
            reading.note = L"no time passed during the upload";
        return reading;
    }
    reading.ok = true;
    reading.value = static_cast<double>(sent) * 8.0 / 1e6 / elapsed;
    reading.note = L"Cloudflare, " + FormatDouble(static_cast<double>(sent) / (1024.0 * 1024.0), 1) +
                   L" MiB" + (elapsed < kWanUpSeconds - 0.5 ? L"" : L", capped at 12 s");
    return reading;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Graphics: a compute shader and a texture copy, timed on the GPU itself.
//
//  Both need a Direct3D 11 device. A session without one - a headless VM, a
//  remote desktop with no hardware adapter - reports the reason rather than a
//  figure, the same way a blocked network row does.
// ---------------------------------------------------------------------------

struct GpuDevice {
    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    ~GpuDevice() {
        if (context) context->Release();
        if (device) device->Release();
    }

    bool Create(std::wstring* why) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL       achieved = D3D_FEATURE_LEVEL_10_0;
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                             2, D3D11_SDK_VERSION, &device, &achieved, &context);
        if (FAILED(hr) || !device || !context) {
            if (context) { context->Release(); context = nullptr; }
            if (device) { device->Release(); device = nullptr; }
            *why = L"no Direct3D 11 device on this machine";
            return false;
        }
        return true;
    }
};

// Four independent chains of multiply-adds: independent so the rate is
// throughput rather than add latency, and with the result read at the end so the
// compiler cannot drop the loop.
const char kComputeShader[] = R"(
RWStructuredBuffer<uint> gOutput : register(u0);
cbuffer Constants : register(b0) { uint gIterations; uint gPad0; uint gPad1; uint gPad2; };

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    float a = (float)id.x * 0.0001f + 0.5f;
    float b = a + 0.25f;
    float c = a + 0.50f;
    float d = a + 0.75f;
    const float k = 1.0000001f;
    [loop] for (uint i = 0; i < gIterations; ++i) {
        a = mad(a, k, 0.0000001f);
        b = mad(b, k, 0.0000002f);
        c = mad(c, k, 0.0000003f);
        d = mad(d, k, 0.0000004f);
    }
    const float sum = a + b + c + d;
    if (sum == 123456.0f) gOutput[id.x] = 1u;
}
)";

bool CompileComputeShader(ID3D11Device* device, ID3D11ComputeShader** shader) {
    ID3DBlob*     code = nullptr;
    ID3DBlob*     errors = nullptr;
    const HRESULT hr = D3DCompile(kComputeShader, sizeof(kComputeShader) - 1, "pcspeedtest_gpu",
                                  nullptr, nullptr, "CSMain", "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) errors->Release();
    if (FAILED(hr) || !code) return false;
    const HRESULT made = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                     nullptr, shader);
    code->Release();
    return SUCCEEDED(made) && *shader != nullptr;
}

// Times a batch on the GPU's own clock. Wall time around a batch is not the
// measurement: it includes the gaps where the queue was empty waiting for the
// CPU to submit the next command, which on a batch of copies is most of the
// time. A timestamp pair and a disjoint query report the work itself, and the
// Stop() call also drains the queue before returning.
struct GpuTimer {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* begin = nullptr;
    ID3D11Query* finish = nullptr;

    ~GpuTimer() {
        if (disjoint) disjoint->Release();
        if (begin) begin->Release();
        if (finish) finish->Release();
    }

    bool Create(ID3D11Device* device) {
        D3D11_QUERY_DESC desc = {};
        desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(device->CreateQuery(&desc, &disjoint))) return false;
        desc.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(device->CreateQuery(&desc, &begin))) return false;
        if (FAILED(device->CreateQuery(&desc, &finish))) return false;
        return true;
    }

    void Start(ID3D11DeviceContext* context) {
        context->Begin(disjoint);
        context->End(begin);
    }

    // Seconds of GPU time, or 0 when the driver would not report it.
    double Stop(ID3D11DeviceContext* context) {
        context->End(finish);
        context->End(disjoint);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT info = {};
        UINT64                                  from = 0;
        UINT64                                  to = 0;
        while (context->GetData(disjoint, &info, sizeof info, 0) == S_FALSE) {
        }
        while (context->GetData(begin, &from, sizeof from, 0) == S_FALSE) {
        }
        while (context->GetData(finish, &to, sizeof to, 0) == S_FALSE) {
        }
        if (info.Disjoint || info.Frequency == 0 || to <= from) return 0.0;
        return static_cast<double>(to - from) / static_cast<double>(info.Frequency);
    }
};

// Compiles one shader stage of the scene shader. The compute row builds its
// shader directly; the frame row needs a vertex and a pixel stage from the same
// source, so the compile is shared.
bool CompileStage(const char* source, const char* entry, const char* target, ID3DBlob** code) {
    ID3DBlob*     errors = nullptr;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "pcspeedtest_scene", nullptr,
                                  nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code,
                                  &errors);
    if (errors) errors->Release();
    return SUCCEEDED(hr) && *code != nullptr;
}

Reading MeasureGpuCompute(const Tick& tick) {
    Reading    reading;
    GpuDevice  gpu;
    std::wstring why;
    if (!gpu.Create(&why)) {
        reading.note = why;
        return reading;
    }

    ID3D11ComputeShader* shader = nullptr;
    if (!CompileComputeShader(gpu.device, &shader)) {
        reading.note = L"the compute shader would not compile";
        return reading;
    }

    D3D11_BUFFER_DESC outputDesc = {};
    outputDesc.ByteWidth = kGpuLanes * sizeof(UINT);
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    outputDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    outputDesc.StructureByteStride = sizeof(UINT);
    ID3D11Buffer* output = nullptr;

    D3D11_UNORDERED_ACCESS_VIEW_DESC viewDesc = {};
    viewDesc.Format = DXGI_FORMAT_UNKNOWN;
    viewDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    viewDesc.Buffer.NumElements = kGpuLanes;
    ID3D11UnorderedAccessView* view = nullptr;

    D3D11_BUFFER_DESC constantDesc = {};
    constantDesc.ByteWidth = sizeof(UINT) * 4;
    constantDesc.Usage = D3D11_USAGE_DEFAULT;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const UINT                constants[4] = {kGpuIterations, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA    initial = {};
    initial.pSysMem = constants;
    ID3D11Buffer* cbuffer = nullptr;

    const bool built = SUCCEEDED(gpu.device->CreateBuffer(&outputDesc, nullptr, &output)) &&
                       SUCCEEDED(gpu.device->CreateUnorderedAccessView(output, &viewDesc, &view)) &&
                       SUCCEEDED(gpu.device->CreateBuffer(&constantDesc, &initial, &cbuffer));
    if (!built) {
        if (cbuffer) cbuffer->Release();
        if (view) view->Release();
        if (output) output->Release();
        shader->Release();
        reading.note = L"could not create the GPU work buffers";
        return reading;
    }

    gpu.context->CSSetShader(shader, nullptr, 0);
    gpu.context->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
    gpu.context->CSSetConstantBuffers(0, 1, &cbuffer);

    GpuTimer timer;
    if (!timer.Create(gpu.device)) {
        cbuffer->Release();
        view->Release();
        output->Release();
        shader->Release();
        reading.note = L"the adapter would not time GPU work";
        return reading;
    }

    auto dispatchBatch = [&gpu](uint64_t count) {
        for (uint64_t i = 0; i < count; ++i) gpu.context->Dispatch(kGpuLanes / 64, 1, 1);
    };

    // One dispatch is timed first, so the batch that follows is sized to fill the
    // budget instead of guessed at.
    timer.Start(gpu.context);
    dispatchBatch(1);
    const double single = timer.Stop(gpu.context);
    uint64_t     dispatches = single > 0.0 ? static_cast<uint64_t>(kGpuSeconds / single) : 64;
    if (dispatches < 16) dispatches = 16;
    if (dispatches > 100000) dispatches = 100000;
    if (tick && !tick()) {
        cbuffer->Release();
        view->Release();
        output->Release();
        shader->Release();
        reading.note = L"stopped before the timed batch";
        return reading;
    }

    timer.Start(gpu.context);
    dispatchBatch(dispatches);
    const double elapsed = timer.Stop(gpu.context);

    cbuffer->Release();
    view->Release();
    output->Release();
    shader->Release();

    if (dispatches == 0 || elapsed <= 0.0) {
        reading.note = L"the GPU clock reported no time for the dispatch";
        return reading;
    }
    // Four multiply-adds per iteration per lane, each counted as two FLOPs.
    const double flops = static_cast<double>(dispatches) * kGpuLanes * kGpuIterations * 4.0 * 2.0;
    reading.ok = true;
    reading.value = flops / elapsed / 1e9;
    reading.timing = TimeSource::GpuTimestamp;
    reading.note = std::to_wstring(dispatches) + L" dispatches of " +
                   std::to_wstring(kGpuLanes) + L" lanes";
    return reading;
}

Reading MeasureGpuBandwidth(const Tick& tick) {
    Reading     reading;
    GpuDevice   gpu;
    std::wstring why;
    if (!gpu.Create(&why)) {
        reading.note = why;
        return reading;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = kGpuSide;
    desc.Height = kGpuSide;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    ID3D11Texture2D* source = nullptr;
    ID3D11Texture2D* target = nullptr;
    if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, &source)) ||
        FAILED(gpu.device->CreateTexture2D(&desc, nullptr, &target))) {
        if (source) source->Release();
        if (target) target->Release();
        reading.note = L"could not create the 64 MiB textures";
        return reading;
    }

    GpuTimer timer;
    if (!timer.Create(gpu.device)) {
        source->Release();
        target->Release();
        reading.note = L"the adapter would not time GPU work";
        return reading;
    }

    auto copyBatch = [&gpu, target, source](uint64_t count) {
        for (uint64_t i = 0; i < count; ++i) gpu.context->CopyResource(target, source);
    };

    timer.Start(gpu.context);
    copyBatch(1);
    const double single = timer.Stop(gpu.context);
    uint64_t     copies = single > 0.0 ? static_cast<uint64_t>(kGpuSeconds / single) : 64;
    if (copies < 8) copies = 8;
    if (copies > 100000) copies = 100000;
    if (tick && !tick()) {
        source->Release();
        target->Release();
        reading.note = L"stopped before the timed batch";
        return reading;
    }

    timer.Start(gpu.context);
    copyBatch(copies);
    const double elapsed = timer.Stop(gpu.context);

    source->Release();
    target->Release();

    if (copies == 0 || elapsed <= 0.0) {
        reading.note = L"the GPU clock reported no time for the copies";
        return reading;
    }
    // A copy reads the source and writes the target, so the traffic is twice the
    // texture: the figure is what the memory did, not what the API was asked.
    const double bytes = static_cast<double>(copies) * kGpuSide * kGpuSide * 4.0 * 2.0;
    reading.ok = true;
    reading.value = bytes / elapsed / 1e9;
    reading.timing = TimeSource::GpuTimestamp;
    reading.note = std::to_wstring(copies) + L" copies of 64 MiB";
    return reading;
}

// A simple scene rendered offscreen and thrown away: a cleared 1080p target, a
// depth buffer and a few thousand blended quads spun by the vertex shader in one
// instanced draw, then cleared again for the next frame. Nothing is presented,
// so the figure is how many frames of a game-shaped load the GPU can produce -
// the same fixed slice on the GPU's own clock as the other two graphics rows,
// and the note says so rather than implying a measured refresh rate.
const char kSceneShader[] = R"(
cbuffer Frame : register(b0) { float gPhase; float3 gPad; };

struct VSOut {
    float4 pos  : SV_Position;
    float2 uv   : TEXCOORD0;
    float4 tint : COLOR0;
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    float2 corner = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);
    float  index  = (float)iid;
    float  radius = 0.10 + 0.80 * frac(index * 0.6180340);
    float  speed  = 0.30 + 1.10 * frac(index * 0.4142136);
    float  angle  = gPhase * speed + index * 2.3999632;
    // Large enough that the quads overlap into tens of screens of blended
    // fill per frame: that overdraw is what a game frame is made of, and a
    // handful of tiny sprites would report a frame rate nothing else can use.
    float  scale  = 0.045 + 0.155 * frac(index * 0.7548777);
    float2 centre = float2(cos(angle), sin(angle)) * radius;
    float  spin   = angle * 1.7;
    float  c = cos(spin);
    float  s = sin(spin);
    float2 local  = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c) * scale;
    VSOut result;
    result.pos  = float4(centre + local, 0.9 - index * 0.0001, 1.0);
    result.uv   = corner * 0.5 + 0.5;
    result.tint = float4(frac(index * 0.113), frac(index * 0.271), frac(index * 0.577), 0.55);
    return result;
}

float4 PSMain(VSOut input) : SV_Target {
    float2 d = input.uv - 0.5;
    float  r = dot(d, d);
    if (r > 0.25) discard;
    return float4(input.tint.rgb, input.tint.a * (1.0 - r * 4.0));
}
)";

Reading MeasureGpuFrames(const Tick& tick) {
    Reading     reading;
    GpuDevice   gpu;
    std::wstring why;
    if (!gpu.Create(&why)) {
        reading.note = why;
        return reading;
    }

    ID3DBlob* vsCode = nullptr;
    ID3DBlob* psCode = nullptr;
    if (!CompileStage(kSceneShader, "VSMain", "vs_5_0", &vsCode) ||
        !CompileStage(kSceneShader, "PSMain", "ps_5_0", &psCode)) {
        if (vsCode) vsCode->Release();
        if (psCode) psCode->Release();
        reading.note = L"the scene shaders would not compile";
        return reading;
    }

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader*  ps = nullptr;
    const bool shaders =
        SUCCEEDED(gpu.device->CreateVertexShader(vsCode->GetBufferPointer(),
                                                vsCode->GetBufferSize(), nullptr, &vs)) &&
        SUCCEEDED(gpu.device->CreatePixelShader(psCode->GetBufferPointer(),
                                               psCode->GetBufferSize(), nullptr, &ps));
    vsCode->Release();
    psCode->Release();
    if (!shaders) {
        if (vs) vs->Release();
        if (ps) ps->Release();
        reading.note = L"the scene shaders would not load";
        return reading;
    }

    D3D11_TEXTURE2D_DESC colourDesc = {};
    colourDesc.Width = kGpuFrameWidth;
    colourDesc.Height = kGpuFrameHeight;
    colourDesc.MipLevels = 1;
    colourDesc.ArraySize = 1;
    colourDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    colourDesc.SampleDesc.Count = 1;
    colourDesc.Usage = D3D11_USAGE_DEFAULT;
    colourDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

    D3D11_TEXTURE2D_DESC depthDesc = colourDesc;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ID3D11Texture2D* colour = nullptr;
    ID3D11Texture2D* depthTexture = nullptr;
    if (FAILED(gpu.device->CreateTexture2D(&colourDesc, nullptr, &colour)) ||
        FAILED(gpu.device->CreateTexture2D(&depthDesc, nullptr, &depthTexture))) {
        if (colour) colour->Release();
        if (depthTexture) depthTexture->Release();
        ps->Release();
        vs->Release();
        reading.note = L"could not create the offscreen frame buffers";
        return reading;
    }

    ID3D11RenderTargetView* target = nullptr;
    ID3D11DepthStencilView* depthView = nullptr;
    if (FAILED(gpu.device->CreateRenderTargetView(colour, nullptr, &target)) ||
        FAILED(gpu.device->CreateDepthStencilView(depthTexture, nullptr, &depthView))) {
        if (target) target->Release();
        if (depthView) depthView->Release();
        depthTexture->Release();
        colour->Release();
        ps->Release();
        vs->Release();
        reading.note = L"could not create the offscreen views";
        return reading;
    }

    const float              phase[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    D3D11_BUFFER_DESC        phaseDesc = {};
    phaseDesc.ByteWidth = sizeof phase;
    phaseDesc.Usage = D3D11_USAGE_DEFAULT;
    phaseDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA   phaseData = {};
    phaseData.pSysMem = phase;
    ID3D11Buffer* phaseBuffer = nullptr;

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState* blend = nullptr;

    D3D11_DEPTH_STENCIL_DESC depthState = {};
    depthState.DepthEnable = TRUE;
    depthState.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthState.DepthFunc = D3D11_COMPARISON_LESS;
    ID3D11DepthStencilState* depthOn = nullptr;

    // No culling: the strip is built from the vertex id, and choosing the
    // winding by driver default is how a whole frame ends up culled away.
    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    ID3D11RasterizerState* raster = nullptr;

    const bool state = SUCCEEDED(gpu.device->CreateBuffer(&phaseDesc, &phaseData, &phaseBuffer)) &&
                       SUCCEEDED(gpu.device->CreateBlendState(&blendDesc, &blend)) &&
                       SUCCEEDED(gpu.device->CreateDepthStencilState(&depthState, &depthOn)) &&
                       SUCCEEDED(gpu.device->CreateRasterizerState(&rasterDesc, &raster));

    GpuTimer timer;
    if (!state || !timer.Create(gpu.device)) {
        if (phaseBuffer) phaseBuffer->Release();
        if (blend) blend->Release();
        if (depthOn) depthOn->Release();
        if (raster) raster->Release();
        depthView->Release();
        target->Release();
        depthTexture->Release();
        colour->Release();
        ps->Release();
        vs->Release();
        reading.note = L"the adapter would not time GPU work";
        return reading;
    }

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(kGpuFrameWidth);
    viewport.Height = static_cast<float>(kGpuFrameHeight);
    viewport.MaxDepth = 1.0f;
    const float clear[4] = {0.035f, 0.043f, 0.063f, 1.0f};

    auto renderFrame = [&]() {
        gpu.context->ClearRenderTargetView(target, clear);
        gpu.context->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH, 1.0f, 0);
        gpu.context->OMSetRenderTargets(1, &target, depthView);
        gpu.context->RSSetViewports(1, &viewport);
        gpu.context->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
        gpu.context->OMSetDepthStencilState(depthOn, 0);
        gpu.context->RSSetState(raster);
        gpu.context->IASetInputLayout(nullptr);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        gpu.context->VSSetShader(vs, nullptr, 0);
        gpu.context->PSSetShader(ps, nullptr, 0);
        gpu.context->VSSetConstantBuffers(0, 1, &phaseBuffer);
        gpu.context->DrawInstanced(4, kGpuFrameQuads, 0, 0);
    };

    // One frame is timed first, so the slice that follows is sized to fill the
    // budget instead of guessed at.
    timer.Start(gpu.context);
    renderFrame();
    const double single = timer.Stop(gpu.context);
    uint64_t     frames = single > 0.0 ? static_cast<uint64_t>(kGpuSeconds / single) : 64;
    if (frames < 4) frames = 4;
    if (frames > 100000) frames = 100000;
    if (tick && !tick()) {
        phaseBuffer->Release();
        blend->Release();
        depthOn->Release();
        raster->Release();
        depthView->Release();
        target->Release();
        depthTexture->Release();
        colour->Release();
        ps->Release();
        vs->Release();
        reading.note = L"stopped before the timed batch";
        return reading;
    }

    timer.Start(gpu.context);
    for (uint64_t i = 0; i < frames; ++i) renderFrame();
    const double elapsed = timer.Stop(gpu.context);

    phaseBuffer->Release();
    blend->Release();
    depthOn->Release();
    raster->Release();
    depthView->Release();
    target->Release();
    depthTexture->Release();
    colour->Release();
    ps->Release();
    vs->Release();

    if (elapsed <= 0.0) {
        reading.note = L"the GPU clock reported no time for the frames";
        return reading;
    }
    reading.ok = true;
    reading.value = static_cast<double>(frames) / elapsed;
    reading.timing = TimeSource::GpuTimestamp;
    reading.note = std::to_wstring(frames) + L" frames of " +
                   std::to_wstring(kGpuFrameWidth) + L" x " +
                   std::to_wstring(kGpuFrameHeight) + L", GPU time only";
    return reading;
}

// ---------------------------------------------------------------------------
//  The catalog
// ---------------------------------------------------------------------------

const std::vector<Metric>& Metrics() {
    static const std::vector<Metric> catalog = {
        {L"CPU - one thread", L"integer and float mix, 0.35 s", L"Mops/s", Group::Cpu, "cpu_single_mops"},
        {L"CPU - every thread", L"the same loop on every logical CPU", L"Mops/s", Group::Cpu, "cpu_multi_mops"},
        {L"Memory - copy", L"64 MiB blocks, 0.3 s", L"GB/s", Group::Memory, "mem_copy_gbs"},
        {L"Memory - latency", L"dependent hops over 32 MiB", L"ns", Group::Memory, "mem_latency_ns"},
        {L"Disk - write", L"192 MiB written and flushed", L"MB/s", Group::Disk, "disk_write_mbs"},
        {L"Disk - read", L"192 MiB, unbuffered", L"MB/s", Group::Disk, "disk_read_mbs"},
        {L"Network - link", L"the adapter carrying the default route", L"Gbps", Group::Network, "lan_link_gbps"},
        {L"Network - loopback", L"32 MiB over 127.0.0.1", L"MB/s", Group::Network, "lan_loopback_mbs"},
        {L"LAN - latency", L"the default gateway, 8 echoes", L"ms", Group::Network, "lan_latency_ms"},
        {L"Internet - latency", L"1.1.1.1, 10 echoes", L"ms", Group::Network, "wan_latency_ms"},
        {L"Internet - download", L"Cloudflare, 40 MiB cap, 12 s cap", L"Mbps", Group::Network, "wan_down_mbps"},
        {L"Internet - upload", L"Cloudflare, 12 MiB cap, 12 s cap", L"Mbps", Group::Network, "wan_up_mbps"},
        {L"GPU - compute", L"independent FMA chains, 0.5 s", L"GFLOP/s", Group::Graphics, "gpu_gflops"},
        {L"GPU - memory", L"64 MiB texture copies, 0.5 s", L"GB/s", Group::Graphics, "gpu_bandwidth"},
        {L"GPU - frames", L"offscreen 1080p scene, 0.5 s", L"FPS", Group::Graphics, "gpu_fps"},
    };
    return catalog;
}

const wchar_t* GroupName(Group group) {
    switch (group) {
        case Group::Cpu: return L"Processor";
        case Group::Memory: return L"Memory";
        case Group::Disk: return L"Disk";
        case Group::Network: return L"Network and internet";
        case Group::Graphics: return L"Graphics";
    }
    return L"";
}

// ---------------------------------------------------------------------------
//  The machine
// ---------------------------------------------------------------------------

void RefreshLink(Machine* machine) {
    machine->linkGbps = 0.0;
    machine->link.clear();

    const DWORD wanted = DefaultInterfaceIndex();
    if (wanted == 0) return;

    MIB_IF_ROW2 row = {};
    row.InterfaceIndex = wanted;
    if (GetIfEntry2(&row) != NO_ERROR) return;
    if (row.TransmitLinkSpeed == 0) return;

    machine->linkGbps = static_cast<double>(row.TransmitLinkSpeed) / 1e9;
    machine->link = row.Alias;
    if (machine->link.empty()) machine->link = row.Description;
}

Machine Describe() {
    Machine machine;
    machine.cpu = Trim(RegistryString(HKEY_LOCAL_MACHINE,
                                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                      L"ProcessorNameString"));
    if (machine.cpu.empty()) machine.cpu = L"unknown processor";

    machine.cores = static_cast<int>(CpuCount());
    machine.physicalCores = PhysicalCoreCount();
    if (machine.physicalCores < 1 || machine.physicalCores > machine.cores)
        machine.physicalCores = machine.cores;

    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof memory;
    if (GlobalMemoryStatusEx(&memory))
        machine.ramGiB = static_cast<double>(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);

    machine.os = DescribeWindows();
    machine.arch = ProcessorArchitecture();

    machine.gpu = GraphicsAdapter(&machine.driver);
    const MemoryReport memoryReport = ReadMemoryReport();
    machine.ramSpeed = memoryReport.speed;
    machine.ramMaker = memoryReport.maker;

    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD   length = MAX_COMPUTERNAME_LENGTH + 1;
    machine.hostUtf8 = GetComputerNameW(name, &length) ? Utf8(name) : "unknown";

    RefreshLink(&machine);
    return machine;
}

// ---------------------------------------------------------------------------
//  Measuring
// ---------------------------------------------------------------------------

Reading Measure(int index, const Machine& machine, const Tick& tick) {
    const std::vector<Metric>& catalog = Metrics();
    if (index < 0 || index >= static_cast<int>(catalog.size())) return Reading{};
    switch (index) {
        case 0: return MeasureCpuSingle(tick, kCpuSeconds);
        case 1: return MeasureCpuMulti(tick, kCpuSeconds);
        case 2: return MeasureMemoryCopy(tick);
        case 3: return MeasureMemoryLatency(tick);
        case 4: return MeasureDiskWrite(tick, kDiskBytes);
        case 5: return MeasureDiskRead(tick, kDiskBytes);
        case 6: return MeasureLink(machine);
        case 7: return MeasureLoopback(tick);
        case 8: {
            const std::wstring gateway = DefaultGatewayText();
            if (gateway.empty()) {
                Reading reading;
                reading.note = L"this machine has no default gateway";
                return reading;
            }
            return PingHost(gateway.c_str(), 8, L"gateway");
        }
        case 9: return PingHost(kLatencyHost, 10, L"1.1.1.1");
        case 10: return MeasureDownload(tick);
        case 11: return MeasureUpload(tick);
        case 12: return MeasureGpuCompute(tick);
        case 13: return MeasureGpuBandwidth(tick);
        case 14: return MeasureGpuFrames(tick);
        default: return Reading{};
    }
}

void MeasureAll(const Machine& machine, const Progress& progress, const Tick& tick) {
    const std::vector<Metric>& catalog = Metrics();
    for (int index = 0; index < static_cast<int>(catalog.size()); ++index) {
        if (tick && !tick()) return;
        Update update;
        update.index = index;
        update.reading = Measure(index, machine, tick);
        if (progress) progress(update);
    }
    Update done;
    done.finished = true;
    if (progress) progress(done);
}

// ---------------------------------------------------------------------------
//  Formatting and the CSV
// ---------------------------------------------------------------------------

std::wstring FormatValue(const Reading& reading, const Metric& metric) {
    if (!reading.ok) return std::wstring();
    int decimals = 1;
    const std::wstring unit = metric.unit;
    if (unit == L"GB/s") decimals = 2;
    if (unit == L"Gbps") decimals = 2;
    return FormatDouble(reading.value, decimals) + L" " + unit;
}

std::wstring ResultsPath() {
    const std::wstring directory = WorkDirectory();
    if (directory.empty()) return std::wstring();
    return directory + L"\\results.csv";
}

namespace {

std::string CsvField(const std::string& text) {
    if (text.find_first_of(",\"\r\n") == std::string::npos) return text;
    std::string quoted = "\"";
    for (char character : text) {
        if (character == '"') quoted += '"';
        quoted += character;
    }
    quoted += '"';
    return quoted;
}

std::string CsvNumber(bool ok, double value, int decimals) {
    if (!ok) return std::string();
    char buffer[64] = {};
    std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
    return buffer;
}

// All three graphics rows share one timer, so one column describes the run.
// It is left empty when no graphics row produced a figure (nothing was timed)
// and says "mixed" rather than picking a clock if a future build ever times
// the rows differently from each other.
std::string GpuTimingColumn(const std::vector<Reading>& readings,
                            const std::vector<Metric>& catalog) {
    bool timestamp = false, wall = false;
    for (size_t i = 0; i < readings.size() && i < catalog.size(); ++i) {
        if (catalog[i].group != Group::Graphics || !readings[i].ok) continue;
        if (readings[i].timing == TimeSource::GpuTimestamp) timestamp = true;
        else if (readings[i].timing == TimeSource::WallClock) wall = true;
    }
    if (timestamp && wall) return "mixed";
    if (timestamp) return "timestamp";
    if (wall) return "wall";
    return std::string();
}

// The machine columns, in the order they are written. `cores` is physical and
// `threads` logical: one column called "cores" holding 8 for a 4-core processor
// was the same wrong answer as calling Windows 11 "Windows 10".
std::string BuildHeader() {
    std::string header =
        "timestamp_utc,host,cpu,cores,threads,arch,ram_gib,ram_speed,ram_maker,os,gpu,"
        "gpu_driver,gpu_timing";
    for (const Metric& metric : Metrics()) header += std::string(",") + metric.key;
    return header;
}

bool ReadAllText(const std::wstring& path, std::string* text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    GetFileSizeEx(file, &size);
    text->clear();
    if (size.QuadPart > 0 && size.QuadPart <= (64ll << 20)) {
        text->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(file, text->data(), static_cast<DWORD>(text->size()), &got, nullptr)) {
            CloseHandle(file);
            return false;
        }
        text->resize(got);
    }
    CloseHandle(file);
    return true;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t                   start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

std::string TimestampUtc() {
    SYSTEMTIME now = {};
    GetSystemTime(&now);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof buffer, "%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear, now.wMonth,
                  now.wDay, now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

}  // namespace

namespace {

// Declared here and defined with the reader below: the migration walks the same
// quoted CSV the reader parses, and one parser is what keeps them in step.
std::vector<std::string> SplitCsvLine(const std::string& line);
int                      FindColumn(const std::vector<std::string>& header, const char* key);
std::string              Column(const std::vector<std::string>& fields, int index);

// A row written by a build that trusted ProductName can claim Windows 10 on a
// machine that runs 11. The build number is in the same string, so the claim can
// be corrected where it stands rather than left standing.
bool OsClaimIsWrong(const std::string& os) {
    const size_t buildAt = os.find("build ");
    if (buildAt == std::string::npos) return false;
    const int build = std::atoi(os.c_str() + buildAt + 6);
    return build >= 22000 && os.compare(0, 10, "Windows 10") == 0;
}

std::string FixedOsClaim(const std::string& os) {
    if (!OsClaimIsWrong(os)) return os;
    return "Windows 11" + os.substr(10);
}

// Rewrite a file that was written under an older header, keeping every row. The
// old `cores` column held logical processors, so it moves to `threads` and the
// new `cores` stays empty for those rows rather than claiming a figure nobody
// measured.
bool MigrateCsv(const std::wstring& path, const std::string& header, std::wstring* error) {
    std::string existing;
    if (!ReadAllText(path, &existing)) {
        if (error) *error = L"could not read " + path;
        return false;
    }
    const std::vector<std::string> lines = SplitLines(existing);
    if (lines.empty()) return true;

    const std::vector<std::string> oldHeader = SplitCsvLine(lines[0]);
    const std::vector<std::string> newFields = SplitCsvLine(header);
    const bool   legacy = FindColumn(oldHeader, "threads") < 0;
    const int    oldCores = FindColumn(oldHeader, "cores");
    const int    threadsAt = FindColumn(newFields, "threads");
    const int    coresAt = FindColumn(newFields, "cores");

    std::string text = header + "\r\n";
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::vector<std::string> fields = SplitCsvLine(lines[i]);
        std::vector<std::string> row(newFields.size());
        for (size_t c = 0; c < newFields.size(); ++c) {
            const int old = FindColumn(oldHeader, newFields[c].c_str());
            if (old >= 0) row[c] = Column(fields, old);
        }
        if (legacy && oldCores >= 0) {
            if (threadsAt >= 0 && row[static_cast<size_t>(threadsAt)].empty())
                row[static_cast<size_t>(threadsAt)] = Column(fields, oldCores);
            if (coresAt >= 0) row[static_cast<size_t>(coresAt)].clear();
        }
        const int osAt = FindColumn(newFields, "os");
        if (osAt >= 0) row[static_cast<size_t>(osAt)] = FixedOsClaim(row[static_cast<size_t>(osAt)]);
        for (size_t c = 0; c < row.size(); ++c) {
            if (c) text += ',';
            text += CsvField(row[c]);
        }
        text += "\r\n";
    }

    const std::wstring temp = path + L".migrating";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = L"could not rewrite " + path;
        return false;
    }
    DWORD      written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                              nullptr);
    CloseHandle(file);
    if (!ok || written != text.size() ||
        !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        if (error) *error = L"could not replace " + path;
        return false;
    }
    return true;
}

}  // namespace

bool AppendCsv(const Machine& machine, const std::vector<Reading>& readings, std::wstring* error) {
    const std::wstring path = ResultsPath();
    if (path.empty()) {
        if (error) *error = L"no writable %LOCALAPPDATA%";
        return false;
    }
    return AppendCsvTo(path, machine, readings, error);
}

bool EmptyCsv(std::wstring* error) {
    const std::wstring path = ResultsPath();
    if (path.empty()) {
        if (error) *error = L"no writable %LOCALAPPDATA%";
        return false;
    }
    return EmptyCsvTo(path, error);
}

// The header stays: an empty file that still says what its columns mean is one
// the next append can write to, and one the reader understands as "no runs"
// rather than as damage.
bool EmptyCsvTo(const std::wstring& path, std::wstring* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = L"could not open " + path;
        return false;
    }
    const std::string header = BuildHeader() + "\r\n";
    DWORD             written = 0;
    const BOOL        ok = WriteFile(file, header.data(), static_cast<DWORD>(header.size()), &written,
                                     nullptr);
    CloseHandle(file);
    if (!ok || written != header.size()) {
        if (error) *error = L"could not write " + path;
        return false;
    }
    return true;
}

bool AppendCsvTo(const std::wstring& path, const Machine& machine,
                 const std::vector<Reading>& readings, std::wstring* error) {
    const std::string header = BuildHeader();

    std::string existing;
    if (ReadAllText(path, &existing)) {
        const std::vector<std::string> lines = SplitLines(existing);
        bool                          rewrite = !lines.empty() && lines[0] != header;
        // A row under the current header can still carry the old mistake, so a
        // mislabelled OS forces the rewrite as well as a header change does.
        for (size_t i = 1; i < lines.size() && !rewrite; ++i) {
            const std::vector<std::string> fields = SplitCsvLine(lines[i]);
            const int osAt = FindColumn(SplitCsvLine(lines[0]), "os");
            if (osAt >= 0 && OsClaimIsWrong(Column(fields, osAt))) rewrite = true;
        }
        if (rewrite && !MigrateCsv(path, header, error)) return false;
    }

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = L"could not open " + path;
        return false;
    }

    LARGE_INTEGER size = {};
    GetFileSizeEx(file, &size);

    const std::vector<Metric>& catalog = Metrics();
    std::string                text;
    if (size.QuadPart == 0) text += header + "\r\n";

    text += TimestampUtc();
    text += "," + CsvField(machine.hostUtf8);
    text += "," + CsvField(Utf8(machine.cpu));
    text += "," + std::to_string(machine.physicalCores);
    text += "," + std::to_string(machine.cores);
    text += "," + CsvField(Utf8(machine.arch));
    text += "," + CsvNumber(true, machine.ramGiB, 1);
    text += "," + CsvField(Utf8(machine.ramSpeed));
    text += "," + CsvField(Utf8(machine.ramMaker));
    text += "," + CsvField(Utf8(machine.os));
    text += "," + CsvField(Utf8(machine.gpu));
    text += "," + CsvField(Utf8(machine.driver));
    text += "," + CsvField(GpuTimingColumn(readings, catalog));
    for (size_t i = 0; i < catalog.size(); ++i) {
        const Reading reading = i < readings.size() ? readings[i] : Reading{};
        int           decimals = 1;
        const std::wstring unit = catalog[i].unit;
        if (unit == L"GB/s" || unit == L"Gbps") decimals = 2;
        text += "," + CsvNumber(reading.ok, reading.value, decimals);
    }
    text += "\r\n";

    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    if (!ok || written != text.size()) {
        if (error) *error = L"could not write " + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Reading the CSV back
// ---------------------------------------------------------------------------

namespace {

// RFC 4180 fields: a quoted field may hold a comma, and "" is one quote.
std::vector<std::string> SplitCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string              field;
    bool                     quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char character = line[i];
        if (quoted) {
            if (character == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += character;
            }
        } else if (character == '"') {
            quoted = true;
        } else if (character == ',') {
            fields.push_back(field);
            field.clear();
        } else {
            field += character;
        }
    }
    fields.push_back(field);
    return fields;
}

int FindColumn(const std::vector<std::string>& header, const char* key) {
    for (size_t i = 0; i < header.size(); ++i)
        if (header[i] == key) return static_cast<int>(i);
    return -1;
}

std::string Column(const std::vector<std::string>& fields, int index) {
    if (index < 0 || index >= static_cast<int>(fields.size())) return std::string();
    return fields[static_cast<size_t>(index)];
}

}  // namespace

std::vector<HistoryRow> LoadHistory(std::wstring* message) {
    return LoadHistoryFrom(ResultsPath(), message);
}

std::vector<HistoryRow> LoadHistoryFrom(const std::wstring& path, std::wstring* message) {
    std::vector<HistoryRow> rows;
    if (path.empty()) {
        if (message) *message = L"There is no %LOCALAPPDATA%, so no run has been recorded.";
        return rows;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (message) *message = L"No run has been recorded yet - results.csv does not exist.";
        return rows;
    }

    LARGE_INTEGER size = {};
    GetFileSizeEx(file, &size);
    if (size.QuadPart <= 0 || size.QuadPart > (64ll << 20)) {
        CloseHandle(file);
        if (message)
            *message = size.QuadPart == 0 ? L"results.csv is empty."
                                          : L"results.csv is too large to read.";
        return rows;
    }

    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD       got = 0;
    const BOOL  read = ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr);
    CloseHandle(file);
    if (!read) {
        if (message) *message = L"results.csv could not be read.";
        return rows;
    }
    text.resize(got);

    std::vector<std::string> lines;
    size_t                   start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) lines.push_back(line);
        start = end + 1;
    }
    if (lines.empty()) {
        if (message) *message = L"results.csv has no rows yet.";
        return rows;
    }

    const std::vector<Metric>& catalog = Metrics();
    const std::vector<std::string> header = SplitCsvLine(lines[0]);
    std::vector<int> keyColumn(catalog.size(), -1);
    for (size_t i = 0; i < catalog.size(); ++i)
        keyColumn[i] = FindColumn(header, catalog[i].key);

    const int timestampColumn = FindColumn(header, "timestamp_utc");
    const int hostColumn = FindColumn(header, "host");
    const int cpuColumn = FindColumn(header, "cpu");
    const int coresColumn = FindColumn(header, "cores");
    const int threadsColumn = FindColumn(header, "threads");
    const int ramColumn = FindColumn(header, "ram_gib");
    const int osColumn = FindColumn(header, "os");

    for (size_t i = 1; i < lines.size(); ++i) {
        const std::vector<std::string> fields = SplitCsvLine(lines[i]);
        HistoryRow                     row;
        row.timestamp = Column(fields, timestampColumn);
        row.host = Column(fields, hostColumn);
        row.cpu = Column(fields, cpuColumn);
        row.os = Column(fields, osColumn);
        row.cores = std::atoi(Column(fields, coresColumn).c_str());
        // A file written before the split has only `cores`, and that figure was
        // the logical count: read it as threads rather than lose it.
        row.threads = std::atoi(Column(fields, threadsColumn).c_str());
        if (row.threads < 1) row.threads = row.cores;
        row.ramGiB = std::atof(Column(fields, ramColumn).c_str());
        row.values.assign(catalog.size(), 0.0);
        row.present.assign(catalog.size(), false);
        for (size_t metric = 0; metric < catalog.size(); ++metric) {
            const std::string cell = Column(fields, keyColumn[metric]);
            if (cell.empty()) continue;
            row.values[metric] = std::atof(cell.c_str());
            row.present[metric] = true;
        }
        rows.push_back(row);
    }

    if (rows.empty() && message) *message = L"results.csv holds a header and no runs.";
    return rows;
}

// ---------------------------------------------------------------------------
//  Self-test: no network, no leftovers
// ---------------------------------------------------------------------------

namespace {

TestResult Check(const std::wstring& name, bool pass, const std::wstring& detail) {
    TestResult result;
    result.name = name;
    result.pass = pass;
    result.detail = detail;
    return result;
}

}  // namespace

std::vector<TestResult> SelfTests() {
    std::vector<TestResult> results;

    // 1. the catalog itself
    {
        const std::vector<Metric>& catalog = Metrics();
        bool uniqueKeys = true;
        bool everyField = true;
        for (size_t i = 0; i < catalog.size(); ++i) {
            if (!catalog[i].name || !*catalog[i].name || !catalog[i].unit || !catalog[i].key ||
                !*catalog[i].key)
                everyField = false;
            for (size_t j = i + 1; j < catalog.size(); ++j)
                if (std::strcmp(catalog[i].key, catalog[j].key) == 0) uniqueKeys = false;
        }
        results.push_back(Check(L"the catalog holds fifteen metrics with unique columns",
                                catalog.size() == 15 && uniqueKeys && everyField,
                                std::to_wstring(catalog.size()) + L" metrics"));
    }

    // 2. formatting
    {
        Reading  reading;
        reading.ok = true;
        reading.value = 1536.25;
        const std::vector<Metric>& catalog = Metrics();
        const std::wstring gbs = FormatValue(reading, catalog[2]);
        const std::wstring mbs = FormatValue(reading, catalog[4]);
        // 1536.25 prints as two decimals for GB/s and one for MB/s; the single
        // decimal is a tie, and printf rounds it to even.
        results.push_back(Check(L"values print with the metric's own precision", gbs == L"1536.25 GB/s" &&
                                                                                   mbs == L"1536.2 MB/s",
                                gbs + L" / " + mbs));
    }

    // 3. CSV escaping
    {
        const std::string plain = CsvField("workshop");
        const std::string comma = CsvField("workshop, upstairs");
        const std::string quote = CsvField("the \"big\" one");
        results.push_back(Check(L"CSV fields escape commas and quotes",
                                plain == "workshop" && comma == "\"workshop, upstairs\"" &&
                                    quote == "\"the \"\"big\"\" one\"",
                                Wide(comma.c_str())));
    }

    // 4. the CPU loop
    {
        const Reading reading = MeasureCpuSingle(nullptr, 0.12);
        results.push_back(Check(L"the CPU loop reports a plausible rate",
                                reading.ok && reading.value > 1.0 && reading.value < 1e6,
                                FormatDouble(reading.value, 1) + L" Mops/s"));
    }

    // 5. memory copy
    {
        const Reading reading = MeasureMemoryCopy(nullptr);
        results.push_back(Check(L"memory copy reports a plausible bandwidth",
                                reading.ok && reading.value > 0.5 && reading.value < 2000.0,
                                FormatDouble(reading.value, 2) + L" GB/s"));
    }

    // 6. memory latency
    {
        const Reading reading = MeasureMemoryLatency(nullptr);
        results.push_back(Check(L"memory latency lands between 0.1 and 2000 ns",
                                reading.ok && reading.value > 0.1 && reading.value < 2000.0,
                                FormatDouble(reading.value, 1) + L" ns"));
    }

    // 7. loopback
    {
        const Reading reading = MeasureLoopback(nullptr);
        results.push_back(Check(L"loopback TCP transfers bytes at a plausible rate",
                                reading.ok && reading.value > 50.0,
                                FormatDouble(reading.value, 1) + L" MB/s"));
    }

    // 8. disk, at a size the self-test can afford
    {
        const Reading wrote = MeasureDiskWrite(nullptr, kDiskSelfTestBytes);
        Reading       read;
        if (wrote.ok) read = MeasureDiskRead(nullptr, kDiskSelfTestBytes);
        results.push_back(Check(L"a small file is written, flushed and read back",
                                wrote.ok && read.ok && read.value > 1.0,
                                FormatDouble(wrote.value, 1) + L" MB/s out, " +
                                    FormatDouble(read.value, 1) + L" MB/s in"));
    }

    // 9. the CSV reader, over a file of the self-test's own making - the user's
    //    results.csv is never opened, written or deleted here.
    {
        wchar_t temp[MAX_PATH] = {};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        std::wstring path = length > 0 ? std::wstring(temp) : std::wstring();
        path += L"PCSpeedTest-selftest-" + std::to_wstring(GetCurrentProcessId()) + L".csv";

        // A quoted host with a comma in it, a quoted cpu with a quote in it, and
        // mem_latency_ns deliberately empty, all in catalogue column order.
        const std::string text =
            "timestamp_utc,host,cpu,cores,threads,arch,ram_gib,ram_speed,ram_maker,"
            "os,gpu,gpu_driver,gpu_timing,cpu_single_mops,cpu_multi_mops,"
            "mem_copy_gbs,mem_latency_ns,disk_write_mbs,disk_read_mbs,"
            "lan_link_gbps,lan_loopback_mbs,lan_latency_ms,wan_latency_ms,wan_down_mbps,"
            "wan_up_mbps,gpu_gflops,gpu_bandwidth,gpu_fps\r\n"
            "2026-11-18T06:26:43Z,\"workshop, upstairs\",\"the \"\"big\"\" one\",4,8,x64,31.9,"
            "3200 MT/s,Corsair,Windows 11 Pro,\"NVIDIA GeForce GTX 1070, 8 GiB\",32.0.15.6094,timestamp,"
            "481.6,2886.9,10.02,,262.3,510,1.00,2397.6,0,10.5,781.6,227.2,1000.5,320.25,812.4\r\n";

        bool wrote = false;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            wrote = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) &&
                    put == text.size();
            CloseHandle(file);
        }

        std::wstring            reason;
        const std::vector<HistoryRow> rows = wrote ? LoadHistoryFrom(path, &reason)
                                                   : std::vector<HistoryRow>();
        if (wrote) DeleteFileW(path.c_str());

        const bool oneRow = wrote && rows.size() == 1;
        bool       pass = oneRow;
        std::wstring detail = wrote ? std::to_wstring(rows.size()) + L" row read back"
                                    : L"could not write the test file";
        if (oneRow) {
            const HistoryRow& row = rows[0];
            const std::vector<Metric>& catalog = Metrics();
            pass = row.host == "workshop, upstairs" &&
                   row.cpu == "the \"big\" one" &&
                   row.cores == 4 && row.threads == 8 &&
                   std::fabs(row.ramGiB - 31.9) < 0.001 &&
                   row.os == "Windows 11 Pro" &&
                   row.present.size() == catalog.size() &&
                   row.values.size() == catalog.size() &&
                   std::fabs(row.values[0] - 481.6) < 0.001 &&
                   std::fabs(row.values[1] - 2886.9) < 0.001 &&
                   std::fabs(row.values[2] - 10.02) < 0.001 &&
                   std::fabs(row.values[4] - 262.3) < 0.001 &&
                   std::fabs(row.values[11] - 227.2) < 0.001 &&
                   std::fabs(row.values[12] - 1000.5) < 0.001 &&
                   std::fabs(row.values[13] - 320.25) < 0.001 &&
                   !row.present[3] &&  // the blank column stays blank
                   row.present[0] && row.present[10];
            // Only claim the fields survived when the comparison above agreed;
            // a failure keeps the neutral line so it cannot read as a pass.
            detail = pass ? L"1 row, " + std::to_wstring(row.values.size()) +
                                L" columns, quoted comma and quote kept, one blank column"
                          : L"1 row read back, but a field did not survive the round trip";
        }

        results.push_back(Check(L"results.csv reads back with quotes and a blank column intact",
                                pass, detail));

        // The migration: a file under the older header - no threads, no arch,
        // and `cores` holding the logical count - has to be rewritten by the
        // next append rather than grown a ragged row.
        wchar_t legacyDir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, legacyDir);
        const std::wstring legacyPath = std::wstring(legacyDir) +
                                        L"PCSpeedTest-selftest-legacy-" +
                                        std::to_wstring(GetCurrentProcessId()) + L".csv";

        const std::string legacy =
            "timestamp_utc,host,cpu,cores,ram_gib,os,cpu_single_mops,cpu_multi_mops,"
            "mem_copy_gbs,mem_latency_ns,disk_write_mbs,disk_read_mbs,lan_link_gbps,"
            "lan_loopback_mbs,lan_latency_ms,wan_latency_ms,wan_down_mbps,wan_up_mbps\r\n"
            "2026-11-17T01:02:03Z,OLD-PC,Old CPU,8,15.9,Windows 10 Pro (build 26300),300,600,5,,"
            "100,200,1.00,1000,1,9,100,50\r\n";

        auto writeSmall = [](const std::wstring& target, const std::string& body) {
            HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;
            DWORD      put = 0;
            const bool ok = WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &put,
                                      nullptr) && put == body.size();
            CloseHandle(file);
            return ok;
        };

        bool         migrated = writeSmall(legacyPath, legacy);
        std::wstring problem;
        if (migrated) {
            const std::vector<Reading> none(Metrics().size());
            migrated = AppendCsvTo(legacyPath, Describe(), none, &problem);
        }

        std::wstring reasonAfter;
        const std::vector<HistoryRow> after =
            migrated ? LoadHistoryFrom(legacyPath, &reasonAfter) : std::vector<HistoryRow>();
        if (migrated) DeleteFileW(legacyPath.c_str());

        bool migratedOk = migrated && after.size() == 2;
        std::wstring migratedDetail = migratedOk
                                          ? L"2 rows after the rewrite"
                                          : (migrated ? L"the rewrite lost a row"
                                                      : L"could not write the test file");
        if (migratedOk) {
            migratedOk = after[0].threads == 8 && after[0].cores == 0 &&
                         after[0].os == "Windows 11 Pro (build 26300)" &&
                         std::fabs(after[0].values[0] - 300.0) < 0.001 &&
                         after[1].cores >= 1 && after[1].threads >= after[1].cores;
            migratedDetail = migratedOk
                                 ? L"old row kept, 8 threads moved out of cores, new row " +
                                       std::to_wstring(after[1].cores) + L" cores / " +
                                       std::to_wstring(after[1].threads) + L" threads"
                                 : L"the rewrite lost a row or a figure";
        }
        results.push_back(Check(L"an older results.csv is migrated, not lost", migratedOk,
                                migratedDetail));

        // The firmware memory table, parsed from bytes the test builds itself: no
        // hardware needed, and the arithmetic offsets are what is being checked.
        {
            struct Module {
                WORD        size;
                WORD        speed;
                const char* maker;
            };
            auto build = [](std::initializer_list<Module> modules) {
                std::vector<BYTE> table(8, 0);  // the raw table's header, skipped by the parser
                for (const Module& module : modules) {
                    const size_t at = table.size();
                    table.resize(at + 0x18, 0);
                    table[at + 0] = 17;    // one memory device
                    table[at + 1] = 0x18;  // formatted length
                    table[at + 0x0C] = static_cast<BYTE>(module.size & 0xFF);
                    table[at + 0x0D] = static_cast<BYTE>(module.size >> 8);
                    table[at + 0x15] = static_cast<BYTE>(module.speed & 0xFF);
                    table[at + 0x16] = static_cast<BYTE>(module.speed >> 8);
                    table[at + 0x17] = 1;  // first string in the set
                    for (const char* c = module.maker; *c; ++c)
                        table.push_back(static_cast<BYTE>(*c));
                    table.push_back(0);
                    table.push_back(0);
                }
                return table;
            };

            // An empty slot must not be counted, and two matching modules must
            // read as one speed rather than a range.
            const MemoryReport matched = ParseMemoryDevices(build({
                {8192, 3200, "Corsair"},
                {8192, 3200, "Corsair"},
                {0, 0, ""},
            }));
            const MemoryReport mixed = ParseMemoryDevices(build({
                {8192, 2666, "Corsair"},
                {4096, 3200, "Kingston"},
            }));

            const bool pass = matched.speed == L"3200 MT/s" && matched.maker == L"Corsair" &&
                              mixed.speed == L"2666-3200 MT/s" && mixed.maker == L"mixed";
            results.push_back(Check(L"a firmware memory table gives a speed and a maker", pass,
                                    matched.speed + L" " + matched.maker + L" / " +
                                        mixed.speed + L" " + mixed.maker));
        }

        // Emptying: two runs in, header alone out, and a reader that still
        // understands the file afterwards.
        {
            wchar_t emptyDir[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, emptyDir);
            const std::wstring emptyPath = std::wstring(emptyDir) +
                                           L"PCSpeedTest-selftest-empty-" +
                                           std::to_wstring(GetCurrentProcessId()) + L".csv";
            DeleteFileW(emptyPath.c_str());

            const std::vector<Reading> none(Metrics().size());
            std::wstring               problem;
            bool ok = AppendCsvTo(emptyPath, Describe(), none, &problem) &&
                      AppendCsvTo(emptyPath, Describe(), none, &problem);
            const size_t before = ok ? LoadHistoryFrom(emptyPath, &problem).size() : 0;
            ok = ok && EmptyCsvTo(emptyPath, &problem);

            std::wstring reason;
            const std::vector<HistoryRow> after =
                ok ? LoadHistoryFrom(emptyPath, &reason) : std::vector<HistoryRow>();
            std::string contents;
            const bool  headerOnly = ok && ReadAllText(emptyPath, &contents) &&
                                     contents == BuildHeader() + "\r\n";
            DeleteFileW(emptyPath.c_str());

            const bool pass = ok && before == 2 && after.empty() && headerOnly;
            results.push_back(Check(L"emptying results.csv keeps the header and loses the runs", pass,
                                    ok ? std::to_wstring(before) + L" rows in, none out, header kept"
                                       : problem));
        }

        // The graphics path: either a rate from a real device, or the reason
        // there is none - never a zero standing in for a measurement.
        {
            const Reading gpu = MeasureGpuCompute(nullptr);
            const bool    plausible = gpu.ok && gpu.value > 1.0 && gpu.value < 1e6;
            const bool    pass = gpu.ok ? plausible : !gpu.note.empty();
            results.push_back(Check(L"a compute dispatch reports a rate or says why not", pass,
                                    gpu.ok ? FormatDouble(gpu.value, 1) + L" GFLOP/s, " + gpu.note
                                           : gpu.note));
        }
    }

    // 10. the description
    {
        const Machine machine = Describe();

        // The name must agree with the build number: Windows 11 keeps
        // "Windows 10" in ProductName, so a name taken from the registry alone
        // would call this machine 10 while it runs 11.
        const size_t buildAt = machine.os.find(L"build ");
        const int    build = buildAt == std::wstring::npos
                                 ? 0
                                 : _wtoi(machine.os.c_str() + buildAt + 6);
        const bool namedEleven = machine.os.find(L"Windows 11") != std::wstring::npos;
        const bool namedTen = machine.os.find(L"Windows 10") != std::wstring::npos;
        const bool isServer = machine.os.find(L"Windows Server") != std::wstring::npos;
        const bool versionAgrees = build == 0 || isServer || namedEleven == (build >= 22000) ||
                                   (build < 22000 && namedTen);

        // Memory speed is optional - a machine can refuse the firmware table - but
        // what is there has to be a rate, not a leftover number.
        const bool memorySane =
            machine.ramSpeed.empty() || machine.ramSpeed.find(L"MT/s") != std::wstring::npos;

        const bool pass = machine.cores >= 1 && machine.physicalCores >= 1 &&
                          machine.physicalCores <= machine.cores && machine.ramGiB > 0.1 &&
                          !machine.cpu.empty() && !machine.os.empty() &&
                          !machine.arch.empty() && !machine.hostUtf8.empty() &&
                          memorySane && versionAgrees;
        std::wstring described =
            std::to_wstring(machine.physicalCores) + L" cores / " +
            std::to_wstring(machine.cores) + L" threads, " + FormatDouble(machine.ramGiB, 1) +
            L" GiB, build " + std::to_wstring(build) + L", " + machine.arch;
        described += machine.gpu.empty() ? L", no adapter" : L", " + machine.gpu;
        if (!machine.ramSpeed.empty()) described += L", " + machine.ramSpeed;
        results.push_back(Check(L"the machine description agrees with the build it reads", pass,
                                described));
    }

    return results;
}

}  // namespace pt
