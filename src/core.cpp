// Zanphar's WinSuite - core services
#include "core.h"

#include <psapi.h>
#include <shlobj.h>
#include <winioctl.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <thread>

// ------------------------------------------------------------------ helpers
static std::wstring TrimTrailingSlash(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

static bool EndsWith(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() &&
           _wcsicmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

static std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

std::wstring WinErrorText(DWORD code) {
    wchar_t* buffer = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&buffer, 0, nullptr);
    std::wstring out;
    if (len && buffer) {
        out.assign(buffer, len);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' '))
            out.pop_back();
    } else {
        out = L"error " + std::to_wstring(code);
    }
    if (buffer) LocalFree(buffer);
    return out;
}

std::wstring FormatCount(uint64_t n) {
    std::wstring digits = std::to_wstring(n);
    std::wstring out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count && count % 3 == 0) out.insert(out.begin(), L',');
        out.insert(out.begin(), *it);
        ++count;
    }
    return out;
}

std::wstring FormatBytes(uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t buf[64];
    if (unit == 0)
        swprintf(buf, 64, L"%llu B", (unsigned long long)bytes);
    else if (value >= 100.0)
        swprintf(buf, 64, L"%.0f %ls", value, units[unit]);
    else
        swprintf(buf, 64, L"%.1f %ls", value, units[unit]);
    return buf;
}

std::wstring FormatDuration(uint64_t seconds) {
    uint64_t days = seconds / 86400;
    uint64_t hours = (seconds % 86400) / 3600;
    uint64_t mins = (seconds % 3600) / 60;
    wchar_t buf[128];
    if (days > 0)
        swprintf(buf, 128, L"%llud %lluh %llum", (unsigned long long)days,
                 (unsigned long long)hours, (unsigned long long)mins);
    else if (hours > 0)
        swprintf(buf, 128, L"%lluh %llum", (unsigned long long)hours, (unsigned long long)mins);
    else
        swprintf(buf, 128, L"%llum", (unsigned long long)mins);
    return buf;
}

std::wstring KnownFolderPath(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
        std::wstring out(raw);
        CoTaskMemFree(raw);
        return out;
    }
    if (raw) CoTaskMemFree(raw);
    return std::wstring();
}

bool ExpandEnv(const std::wstring& in, std::wstring& out) {
    DWORD needed = ExpandEnvironmentStringsW(in.c_str(), nullptr, 0);
    if (needed == 0) return false;
    std::wstring buffer(needed, L'\0');
    DWORD written = ExpandEnvironmentStringsW(in.c_str(), buffer.data(), needed);
    if (written == 0) return false;
    buffer.resize(written > 0 ? written - 1 : 0);
    out = buffer;
    return true;
}

// -------------------------------------------------------------- registry io
static bool EnsureKey(HKEY root, const std::wstring& keyPath, DWORD extraFlags, HKEY& out) {
    DWORD disposition = 0;
    LONG rc = RegCreateKeyExW(root, keyPath.c_str(), 0, nullptr, 0,
                              KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_ENUMERATE_SUB_KEYS |
                                  KEY_WOW64_64KEY | extraFlags,
                              nullptr, &out, &disposition);
    return rc == ERROR_SUCCESS;
}

bool ReadRegValue(HKEY root, const std::wstring& keyPath, const std::wstring& valueName,
                  DWORD extraFlags, RegValue& out, std::wstring& error) {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(root, keyPath.c_str(), 0, KEY_QUERY_VALUE | extraFlags, &key);
    if (rc != ERROR_SUCCESS) {
        error = WinErrorText(rc);
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    rc = RegQueryValueExW(key, valueName.c_str(), nullptr, &type, nullptr, &size);
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(key);
        error = WinErrorText(rc);
        return false;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        RegCloseKey(key);
        error = L"unsupported value type";
        return false;
    }
    std::wstring data(size / sizeof(wchar_t) + 1, L'\0');
    rc = RegQueryValueExW(key, valueName.c_str(), nullptr, &type, (LPBYTE)data.data(), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        error = WinErrorText(rc);
        return false;
    }
    data.resize(size / sizeof(wchar_t));
    while (!data.empty() && data.back() == L'\0') data.pop_back();
    out.type = type;
    out.data = data;
    return true;
}

bool WriteRegValue(HKEY root, const std::wstring& keyPath, const std::wstring& valueName,
                   DWORD extraFlags, const RegValue& value, std::wstring& error) {
    HKEY key = nullptr;
    if (!EnsureKey(root, keyPath, extraFlags, key)) {
        error = WinErrorText(GetLastError());
        return false;
    }
    DWORD bytes = (DWORD)((value.data.size() + 1) * sizeof(wchar_t));
    LONG rc = RegSetValueExW(key, valueName.c_str(), 0, value.type, (const BYTE*)value.data.c_str(),
                             bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        error = WinErrorText(rc);
        return false;
    }
    return true;
}

bool DeleteRegValue(HKEY root, const std::wstring& keyPath, const std::wstring& valueName,
                    DWORD extraFlags, std::wstring& error) {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(root, keyPath.c_str(), 0, KEY_SET_VALUE | extraFlags, &key);
    if (rc != ERROR_SUCCESS) {
        error = WinErrorText(rc);
        return false;
    }
    rc = RegDeleteValueW(key, valueName.c_str());
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        error = WinErrorText(rc);
        return false;
    }
    return true;
}

// ------------------------------------------------------------ system info
static std::wstring ReadRegString(HKEY root, const std::wstring& key, const std::wstring& value) {
    RegValue rv;
    std::wstring err;
    if (ReadRegValue(root, key, value, KEY_WOW64_64KEY, rv, err)) return rv.data;
    return std::wstring();
}

static void ReadOsVersion(std::wstring& name, std::wstring& build) {
    typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    DWORD major = 0, minor = 0, buildNo = 0;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = (RtlGetVersionPtr)(void*)GetProcAddress(ntdll, "RtlGetVersion");
        if (fn) {
            RTL_OSVERSIONINFOW vi = {};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0) {
                major = vi.dwMajorVersion;
                minor = vi.dwMinorVersion;
                buildNo = vi.dwBuildNumber;
            }
        }
    }
    wchar_t buf[128];
    if (major >= 10) {
        if (buildNo >= 22000)
            name = L"Windows 11";
        else
            name = L"Windows 10";
    } else if (major == 6 && minor == 3) {
        name = L"Windows 8.1";
    } else if (major == 6 && minor == 2) {
        name = L"Windows 8";
    } else if (major == 6 && minor == 1) {
        name = L"Windows 7";
    } else {
        name = L"Windows";
    }
    swprintf(buf, 128, L"build %lu.%lu.%lu", (unsigned long)major, (unsigned long)minor,
             (unsigned long)buildNo);
    build = buf;
}

static unsigned CountPhysicalCores() {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0) return 0;
    std::vector<BYTE> buffer(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data(),
                                          &len))
        return 0;
    unsigned cores = 0;
    DWORD offset = 0;
    while (offset < len) {
        auto* entry = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer.data() + offset);
        if (entry->Relationship == RelationProcessorCore) ++cores;
        offset += entry->Size;
    }
    return cores;
}

SysInfo CollectSystemInfo() {
    SysInfo info;

    ReadOsVersion(info.osName, info.osBuild);
#ifdef _WIN64
    info.arch = L"64-bit (x64)";
#else
    info.arch = L"32-bit (x86)";
#endif

    wchar_t nameBuf[256] = {};
    DWORD nameLen = 256;
    if (GetComputerNameW(nameBuf, &nameLen)) info.computer = nameBuf;
    wchar_t userBuf[256] = {};
    DWORD userLen = 256;
    if (GetUserNameW(userBuf, &userLen)) info.user = userBuf;

    info.cpuName = ReadRegString(HKEY_LOCAL_MACHINE,
                                 L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                 L"ProcessorNameString");
    while (!info.cpuName.empty() && info.cpuName.front() == L' ') info.cpuName.erase(0, 1);
    while (!info.cpuName.empty() && info.cpuName.back() == L' ') info.cpuName.pop_back();

    SYSTEM_INFO si = {};
    GetNativeSystemInfo(&si);
    info.logicalCpus = si.dwNumberOfProcessors;
    info.physicalCpus = CountPhysicalCores();
    if (info.physicalCpus == 0) info.physicalCpus = info.logicalCpus;

    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        info.ramTotal = mem.ullTotalPhys;
        info.ramAvail = mem.ullAvailPhys;
    }

    info.uptimeSeconds = GetTickCount64() / 1000ULL;

    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER freeAvail = {}, total = {}, totalFree = {};
        if (!GetDiskFreeSpaceExW(root, &freeAvail, &total, &totalFree)) continue;
        DriveInfo d;
        d.root = root;
        d.total = total.QuadPart;
        d.freeBytes = freeAvail.QuadPart;
        wchar_t label[MAX_PATH + 1] = {};
        wchar_t fs[MAX_PATH + 1] = {};
        DWORD serial = 0, maxLen = 0, flags = 0;
        if (GetVolumeInformationW(root, label, MAX_PATH, &serial, &maxLen, &flags, fs, MAX_PATH)) {
            d.label = label;
            d.fileSystem = fs;
        }
        info.drives.push_back(d);
    }
    return info;
}

double SampleCpuUsagePercent() {
    auto snap = [](ULONGLONG& idle, ULONGLONG& kernel, ULONGLONG& user) {
        FILETIME i, k, u;
        if (!GetSystemTimes(&i, &k, &u)) return false;
        idle = ((ULONGLONG)i.dwHighDateTime << 32) | i.dwLowDateTime;
        kernel = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime;
        user = ((ULONGLONG)u.dwHighDateTime) << 32 | u.dwLowDateTime;
        return true;
    };
    ULONGLONG i1 = 0, k1 = 0, u1 = 0, i2 = 0, k2 = 0, u2 = 0;
    if (!snap(i1, k1, u1)) return 0.0;
    Sleep(120);
    if (!snap(i2, k2, u2)) return 0.0;
    ULONGLONG idleDelta = i2 - i1;
    ULONGLONG totalDelta = (k2 - k1) + (u2 - u1);
    if (totalDelta == 0) return 0.0;
    double usage = 100.0 * (double)(totalDelta - idleDelta) / (double)totalDelta;
    if (usage < 0) usage = 0;
    if (usage > 100) usage = 100;
    return usage;
}

std::wstring SystemReportText(const SysInfo& info, double cpuPercent) {
    std::wstring out;
    auto line = [&](const std::wstring& a, const std::wstring& b) {
        out += a + L": " + b + L"\r\n";
    };
    out += L"Zanphar's WinSuite - system report\r\n";
    out += L"--------------------------------\r\n";
    line(L"Computer", info.computer);
    line(L"User", info.user);
    line(L"Operating system", info.osName + L" (" + info.osBuild + L")");
    line(L"Architecture", info.arch);
    line(L"Processor", info.cpuName);
    wchar_t buf[128];
    swprintf(buf, 128, L"%u cores / %u threads", info.physicalCpus, info.logicalCpus);
    line(L"Cores", buf);
    line(L"CPU load", std::to_wstring((int)(cpuPercent + 0.5)) + L" %");
    line(L"Memory", FormatBytes(info.ramTotal - info.ramAvail) + L" used of " +
                       FormatBytes(info.ramTotal) + L" (" +
                       std::to_wstring((int)(100.0 * (info.ramTotal - info.ramAvail) /
                                              (info.ramTotal ? info.ramTotal : 1))) +
                       L" %)");
    line(L"Uptime", FormatDuration(info.uptimeSeconds));
    for (const auto& d : info.drives) {
        swprintf(buf, 128, L"%ls free of %ls", FormatBytes(d.freeBytes).c_str(),
                 FormatBytes(d.total).c_str());
        line(L"Drive " + d.root + (d.label.empty() ? L"" : L" (" + d.label + L")"), buf);
    }
    return out;
}

// ---------------------------------------------------------------- storage
static std::wstring TrimSpace(std::wstring s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(0, 1);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    return s;
}

static const wchar_t* DriveTypeName(UINT type) {
    switch (type) {
        case DRIVE_REMOVABLE:
            return L"Removable";
        case DRIVE_FIXED:
            return L"Fixed";
        case DRIVE_REMOTE:
            return L"Network";
        case DRIVE_CDROM:
            return L"Optical";
        case DRIVE_RAMDISK:
            return L"RAM disk";
        default:
            return L"Unknown";
    }
}

// STORAGE_BUS_TYPE values, mapped by number so older SDK headers still compile.
static const wchar_t* BusTypeName(int type) {
    switch (type) {
        case 1: return L"SCSI";
        case 2: return L"ATAPI";
        case 3: return L"ATA";
        case 4: return L"1394";
        case 7: return L"USB";
        case 8: return L"RAID";
        case 9: return L"iSCSI";
        case 10: return L"SAS";
        case 11: return L"SATA";
        case 12: return L"SD";
        case 13: return L"MMC";
        case 14: return L"Virtual";
        case 15: return L"File-backed virtual";
        case 16: return L"Storage Spaces";
        case 17: return L"NVMe";
        case 18: return L"SCM";
        case 19: return L"UFS";
        default: return L"";
    }
}

// Asks the volume for its underlying device descriptor. Quietly does nothing when
// Windows refuses (non-elevated on some systems), in which case the row simply
// shows less detail instead of failing.
static void ReadVolumeTech(const std::wstring& root, StorageDevice& device) {
    if (root.size() < 2) return;
    std::wstring path = L"\\\\.\\" + root.substr(0, 2);
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    BYTE buffer[1024] = {};
    DWORD returned = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer,
                        sizeof(buffer), &returned, nullptr) &&
        returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const STORAGE_DEVICE_DESCRIPTOR* desc = (const STORAGE_DEVICE_DESCRIPTOR*)buffer;
        auto textAt = [&](DWORD offset) -> std::wstring {
            if (!offset || offset >= returned) return std::wstring();
            const char* text = (const char*)buffer + offset;
            int len = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
            if (len <= 0) return std::wstring();
            std::wstring wide((size_t)len, L'\0');
            MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), len);
            while (!wide.empty() && (wide.back() == L'\0' || wide.back() == L' ')) wide.pop_back();
            return TrimSpace(wide);
        };
        std::wstring vendor = textAt(desc->VendorIdOffset);
        std::wstring product = textAt(desc->ProductIdOffset);
        device.bus = BusTypeName((int)desc->BusType);
        device.model = vendor;
        if (!product.empty()) device.model += (device.model.empty() ? L"" : L" ") + product;
    }

    // The seek-penalty answer is what tells a mechanical disk from an SSD, and
    // therefore which of defrag.exe's modes is the right one.
    STORAGE_PROPERTY_QUERY seek = {};
    seek.PropertyId = StorageDeviceSeekPenaltyProperty;
    seek.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty = {};
    DWORD seekGot = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &seek, sizeof(seek), &penalty,
                        sizeof(penalty), &seekGot, nullptr) &&
        seekGot >= sizeof(penalty)) {
        device.mediaKnown = true;
        device.ssd = penalty.IncursSeekPenalty == FALSE;
    }
    CloseHandle(h);
}

std::vector<StorageDevice> CollectStorageDevices() {
    std::vector<StorageDevice> out;
    wchar_t buffer[512] = {};
    if (!GetLogicalDriveStringsW(511, buffer)) return out;

    for (const wchar_t* p = buffer; *p; p += wcslen(p) + 1) {
        std::wstring root(p);
        UINT type = GetDriveTypeW(root.c_str());

        StorageDevice device;
        device.root = root;
        device.typeName = DriveTypeName(type);

        wchar_t label[MAX_PATH + 1] = {};
        wchar_t fileSystem[64] = {};
        DWORD serial = 0, maxComponent = 0, flags = 0;
        if (GetVolumeInformationW(root.c_str(), label, MAX_PATH, &serial, &maxComponent, &flags,
                                  fileSystem, 64)) {
            device.label = TrimSpace(label);
            device.fileSystem = TrimSpace(fileSystem);
            device.accessible = true;
        }

        if (type != DRIVE_CDROM) {
            ULARGE_INTEGER freeAvail = {}, total = {};
            if (GetDiskFreeSpaceExW(root.c_str(), &freeAvail, &total, nullptr)) {
                device.total = total.QuadPart;
                device.freeBytes = freeAvail.QuadPart;
                device.accessible = true;
            }
        }

        ReadVolumeTech(root, device);
        out.push_back(std::move(device));
    }
    return out;
}

// Only one defrag.exe runs at a time: the Storage page disables every action while
// it is busy. The handle is kept here so Cancel can reach the exact child process,
// and the mutex closes the gap between CreateProcess and the handle being stored.
static std::mutex g_defragMutex;
static HANDLE g_defragProcess = nullptr;
static std::atomic<bool> g_defragCancel{false};

void ResetVolumeOptimize() { g_defragCancel.store(false); }

void CancelVolumeOptimize() {
    g_defragCancel.store(true);
    std::lock_guard<std::mutex> lock(g_defragMutex);
    if (g_defragProcess) TerminateProcess(g_defragProcess, 1);
}

static int LastPercent(const std::wstring& text) {
    size_t percent = text.find_last_of(L'%');
    if (percent == std::wstring::npos) return -1;
    size_t start = percent;
    while (start > 0 && text[start - 1] >= L'0' && text[start - 1] <= L'9') --start;
    if (start == percent) return -1;
    int value = 0;
    for (size_t i = start; i < percent; ++i) value = value * 10 + (text[i] - L'0');
    return value >= 0 && value <= 100 ? value : -1;
}

static bool RunDefrag(const std::wstring& executable, const std::wstring& arguments,
                      std::wstring& output, DWORD& exitCode,
                      const std::function<void(int)>& progress) {
    SECURITY_ATTRIBUTES inherit = {};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &inherit, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION info = {};
    // Resolve the Windows copy explicitly. CreateProcess's bare-name search checks the
    // application folder before PATH, so a stray defrag.exe beside the suite must never
    // be able to run with the elevated token.
    std::wstring program = executable;
    if (program == L"defrag.exe") {
        wchar_t systemDir[MAX_PATH] = {};
        if (GetSystemDirectoryW(systemDir, MAX_PATH))
            program = std::wstring(systemDir) + L"\\defrag.exe";
    }
    std::wstring command = L"\"" + program + L"\" " + arguments;
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    CloseHandle(writePipe);
    if (!started) {
        output = WinErrorText(GetLastError());
        CloseHandle(readPipe);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_defragMutex);
        // A cancel that arrived while CreateProcess was starting must not be lost.
        if (g_defragCancel.load())
            TerminateProcess(info.hProcess, 1);
        else
            g_defragProcess = info.hProcess;
    }

    std::string bytes;
    char buffer[4096];
    DWORD got = 0;
    int reported = -1;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        bytes.append(buffer, got);
        // Keep a little history so a percentage split across two pipe reads is still
        // read as one number rather than as the tail of the old one.
        const size_t tailFrom = bytes.size() > 4096 ? bytes.size() - 4096 : 0;
        const int length = MultiByteToWideChar(CP_OEMCP, 0, bytes.data() + tailFrom,
                                               (int)(bytes.size() - tailFrom), nullptr, 0);
        if (length > 0 && progress) {
            std::wstring chunk((size_t)length, L'\0');
            MultiByteToWideChar(CP_OEMCP, 0, bytes.data() + tailFrom,
                                (int)(bytes.size() - tailFrom), chunk.data(), length);
            const int percent = LastPercent(chunk);
            if (percent >= 0 && percent != reported) {
                reported = percent;
                progress(percent);
            }
        }
        if (g_defragCancel.load()) break;
    }
    CloseHandle(readPipe);
    WaitForSingleObject(info.hProcess, INFINITE);
    GetExitCodeProcess(info.hProcess, &exitCode);
    {
        std::lock_guard<std::mutex> lock(g_defragMutex);
        if (g_defragProcess == info.hProcess) g_defragProcess = nullptr;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);

    const int length = MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (length > 0) {
        output.resize((size_t)length);
        MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), (int)bytes.size(), output.data(), length);
    }
    return true;
}

static std::wstring LastOutputLine(const std::wstring& output) {
    size_t end = output.find_last_not_of(L"\r\n \t");
    if (end == std::wstring::npos) return std::wstring();
    size_t start = output.find_last_of(L"\r\n", end);
    start = start == std::wstring::npos ? 0 : start + 1;
    std::wstring line = output.substr(start, end - start + 1);
    while (!line.empty() && (line.front() == L' ' || line.front() == L'\t')) line.erase(0, 1);
    return line;
}

std::wstring StorageOptimizeRefusal(const StorageDevice& device) {
    if (device.typeName == L"Network" || device.typeName == L"Optical")
        return device.root + L" is not a local disk, so it was left alone.";
    if (!device.mediaKnown)
        return device.root + L" did not report whether it is solid-state or mechanical, so "
               L"it was left alone.";
    if (!device.total) return device.root + L" has no mounted media to optimize.";
    return std::wstring();
}

OptimizeOutcome OptimizeVolume(const StorageDevice& device,
                              const std::function<void(int)>& progress,
                              const std::wstring& executable) {
    OptimizeOutcome result;
    const std::wstring refusal = StorageOptimizeRefusal(device);
    if (!refusal.empty()) {
        result.message = refusal;
        return result;
    }
    const std::wstring volume = device.root.substr(0, 2);
    const std::wstring arguments = L"\"" + volume + L"\" " + (device.ssd ? L"/L" : L"/D /H");
    DWORD exitCode = 0;
    std::wstring output;
    if (!RunDefrag(executable, arguments, output, exitCode, progress)) {
        result.message = L"defrag.exe could not start: " + output;
        return result;
    }
    if (g_defragCancel.load()) {
        result.cancelled = true;
        result.message = volume + L" optimization was cancelled.";
        return result;
    }
    if (exitCode != 0) {
        const std::wstring line = LastOutputLine(output);
        result.message = volume + L" was not optimized" + (line.empty() ? L"." : L": " + line);
        return result;
    }
    result.ok = true;
    result.message = volume + (device.ssd ? L" was retrimmed." : L" was defragmented.");
    return result;
}

// ----------------------------------------------------------------- memory
static bool EnablePrivilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    LUID luid = {};
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, name, &luid)) {
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) != FALSE &&
             GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return ok;
}

MemStats CollectMemStats() {
    MemStats s;
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.total = ms.ullTotalPhys;
        s.avail = ms.ullAvailPhys;
        s.loadPercent = ms.dwMemoryLoad;
    }

    PERFORMANCE_INFORMATION perf = {};
    perf.cb = sizeof(perf);
    if (GetPerformanceInfo(&perf, sizeof(perf))) {
        uint64_t page = perf.PageSize ? perf.PageSize : 4096;
        s.committed = (uint64_t)perf.CommitTotal * page;
        s.commitLimit = (uint64_t)perf.CommitLimit * page;
        s.cached = (uint64_t)perf.SystemCache * page;
        s.kernelPaged = (uint64_t)perf.KernelPaged * page;
        s.kernelNonPaged = (uint64_t)perf.KernelNonpaged * page;
        s.processCount = perf.ProcessCount;
        s.threadCount = perf.ThreadCount;
        s.handleCount = perf.HandleCount;
        if (!s.total) s.total = (uint64_t)perf.PhysicalTotal * page;
        if (!s.avail) s.avail = (uint64_t)perf.PhysicalAvailable * page;
    }
    return s;
}

static bool SameSession(DWORD pid) {
    DWORD session = 0, mine = 0;
    if (!ProcessIdToSessionId(pid, &session)) return false;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &mine)) return false;
    return session == mine;
}

static bool ReadProcessMemoryInfo(DWORD pid, PROCESS_MEMORY_COUNTERS_EX& out) {
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    out.cb = sizeof(out);
    bool ok = GetProcessMemoryInfo(p, (PROCESS_MEMORY_COUNTERS*)&out, sizeof(out)) != FALSE;
    CloseHandle(p);
    return ok;
}

std::vector<ProcMem> TopMemoryProcesses(size_t maxResults) {
    std::vector<ProcMem> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID == 0 || !SameSession(entry.th32ProcessID)) continue;
            PROCESS_MEMORY_COUNTERS_EX counters = {};
            if (!ReadProcessMemoryInfo(entry.th32ProcessID, counters)) continue;
            ProcMem item;
            item.name = entry.szExeFile;
            item.pid = entry.th32ProcessID;
            item.workingSet = counters.WorkingSetSize;
            item.privateBytes = counters.PrivateUsage;
            out.push_back(std::move(item));
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);

    std::sort(out.begin(), out.end(), [](const ProcMem& a, const ProcMem& b) {
        return a.workingSet > b.workingSet;
    });
    if (out.size() > maxResults) out.resize(maxResults);
    return out;
}

bool TrimProcessWorkingSet(DWORD pid, uint64_t* reclaimed, std::wstring& error) {
    if (reclaimed) *reclaimed = 0;
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA, FALSE, pid);
    if (!p) {
        error = WinErrorText(GetLastError());
        return false;
    }

    PROCESS_MEMORY_COUNTERS_EX before = {};
    before.cb = sizeof(before);
    GetProcessMemoryInfo(p, (PROCESS_MEMORY_COUNTERS*)&before, sizeof(before));

    BOOL trimmed = EmptyWorkingSet(p);
    DWORD err = trimmed ? ERROR_SUCCESS : GetLastError();

    PROCESS_MEMORY_COUNTERS_EX after = {};
    after.cb = sizeof(after);
    if (GetProcessMemoryInfo(p, (PROCESS_MEMORY_COUNTERS*)&after, sizeof(after)) && reclaimed &&
        before.WorkingSetSize > after.WorkingSetSize)
        *reclaimed = before.WorkingSetSize - after.WorkingSetSize;
    CloseHandle(p);

    if (!trimmed) error = WinErrorText(err);
    return trimmed != FALSE;
}

// Core Windows processes are left alone: trimming them buys nothing and can
// stall the session, so a cleaner has no business touching them.
static bool IsProtectedProcess(const std::wstring& name) {
    static const wchar_t* kProtected[] = {L"system",     L"registry",  L"smss.exe",
                                          L"csrss.exe",  L"wininit.exe", L"winlogon.exe",
                                          L"services.exe", L"lsass.exe", L"lsaiso.exe",
                                          L"memcompression"};
    for (const wchar_t* p : kProtected)
        if (_wcsicmp(name.c_str(), p) == 0) return true;
    return false;
}

MemCleanResult TrimWorkingSets(bool includeOtherSessions) {
    MemCleanResult result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        result.error = WinErrorText(GetLastError());
        return result;
    }

    DWORD self = GetCurrentProcessId();
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            DWORD pid = entry.th32ProcessID;
            if (pid == 0 || pid == 4 || pid == self) continue;
            if (!includeOtherSessions && !SameSession(pid)) {
                ++result.skipped;
                continue;
            }
            if (IsProtectedProcess(entry.szExeFile)) {
                ++result.skipped;
                continue;
            }
            uint64_t freed = 0;
            std::wstring error;
            if (TrimProcessWorkingSet(pid, &freed, error)) {
                ++result.trimmed;
                result.reclaimed += freed;
            } else {
                ++result.skipped;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return result;
}

bool DeepCleanAvailable() {
    if (!EnablePrivilege(SE_PROF_SINGLE_PROCESS_NAME)) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    LUID luid = {};
    bool available = false;
    if (LookupPrivilegeValueW(nullptr, SE_PROF_SINGLE_PROCESS_NAME, &luid)) {
        PRIVILEGE_SET set = {};
        set.PrivilegeCount = 1;
        set.Control = PRIVILEGE_SET_ALL_NECESSARY;
        set.Privilege[0].Luid = luid;
        set.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;
        BOOL held = FALSE;
        if (PrivilegeCheck(token, &set, &held)) available = held != FALSE;
    }
    CloseHandle(token);
    return available;
}

MemCleanResult DeepCleanMemory() {
    MemCleanResult result = TrimWorkingSets(true);

    if (!EnablePrivilege(SE_PROF_SINGLE_PROCESS_NAME)) {
        result.error =
            L"deep clean needs administrator rights (SeProfileSingleProcessPrivilege)";
        return result;
    }

    typedef LONG(NTAPI * NtSetSystemInformationFn)(ULONG, PVOID, ULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto setInformation =
        ntdll ? (NtSetSystemInformationFn)(void*)GetProcAddress(ntdll, "NtSetSystemInformation")
              : nullptr;
    if (!setInformation) {
        result.error = L"NtSetSystemInformation is not available on this Windows build";
        return result;
    }

    // SYSTEM_MEMORY_LIST_INFORMATION (80) with MemoryPurgeStandbyList (4).
    ULONG command = 4;
    LONG status = setInformation(80, &command, sizeof(command));
    if (status == 0) {
        result.purgedStandby = true;
    } else {
        wchar_t buf[128];
        swprintf(buf, 128, L"the standby purge was refused (NTSTATUS 0x%08lX)",
                 (unsigned long)status);
        result.error = buf;
    }
    return result;
}

// --------------------------------------------------------------- cleanup
static std::wstring EnvPath(const wchar_t* var) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return std::wstring();
    return buf;
}

std::vector<CleanGroup> BuildCleanGroups() {
    std::vector<CleanGroup> groups;

    auto add = [&](const wchar_t* id, const wchar_t* name, std::wstring path, bool admin,
                   bool selected, bool personal, const wchar_t* note) {
        CleanGroup g;
        g.id = id;
        g.name = name;
        g.path = TrimTrailingSlash(path);
        g.needsAdmin = admin;
        g.selected = selected;
        g.personal = personal;
        if (!g.path.empty()) {
            DWORD attrs = GetFileAttributesW(g.path.c_str());
            g.exists = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
        }
        if (note && *note) g.error = note;
        groups.push_back(g);
    };

    std::wstring userTemp = EnvPath(L"TEMP");
    if (userTemp.empty()) {
        std::wstring local = KnownFolderPath(FOLDERID_LocalAppData);
        if (!local.empty()) userTemp = local + L"\\Temp";
    }
    add(L"user_temp", L"Your temporary files", userTemp, false, true, false, L"");

    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    add(L"windows_temp", L"Windows temporary files", std::wstring(winDir) + L"\\Temp", true,
        true, false, L"");

    std::wstring local = KnownFolderPath(FOLDERID_LocalAppData);
    if (!local.empty()) {
        add(L"inet_cache", L"Browser internet cache",
            local + L"\\Microsoft\\Windows\\INetCache", false, true, false, L"");
        add(L"crash_dumps", L"Crash dump files", local + L"\\CrashDumps", false, true, false,
            L"");
        add(L"wer", L"Windows error reports", local + L"\\Microsoft\\Windows\\WER", false, true,
            false, L"");
    }

    // The user's own downloads: listed and measured, but never armed by default.
    std::wstring downloads = KnownFolderPath(FOLDERID_Downloads);
    if (downloads.empty()) {
        std::wstring profile = EnvPath(L"USERPROFILE");
        if (!profile.empty()) downloads = profile + L"\\Downloads";
    }
    add(L"downloads", L"Your Downloads folder", downloads, false, false, true, L"");

    // The Recycle Bin holds what Downloads and Explorer have already deleted, spread over
    // every drive. It is not a folder to walk, so it is measured and emptied through the
    // shell and `path` only carries the line the row shows under its name. It is the
    // user's own data all the same: unticked by default, never ticked in bulk, never saved.
    CleanGroup bin;
    bin.id = L"recycle_bin";
    bin.name = L"Recycle Bin";
    bin.path = L"Everything deleted on every drive - emptying it is permanent";
    bin.personal = true;
    bin.recycleBin = true;
    bin.selected = false;
    bin.exists = true;
    groups.push_back(bin);

    // Windows Update and the other system caches that grow back on their own.
    add(L"wu_downloads", L"Windows Update downloads",
        std::wstring(winDir) + L"\\SoftwareDistribution\\Download", true, true, false, L"");
    add(L"prefetch", L"Windows Prefetch", std::wstring(winDir) + L"\\Prefetch", true, true,
        false, L"");
    add(L"live_kernel", L"Live kernel reports", std::wstring(winDir) + L"\\LiveKernelReports",
        true, true, false, L"");
    add(L"minidump", L"Windows minidumps", std::wstring(winDir) + L"\\Minidump", true, true,
        false, L"");
    std::wstring programData = EnvPath(L"ProgramData");
    if (programData.empty()) programData = L"C:\\ProgramData";
    add(L"system_wer", L"System error reports", programData + L"\\Microsoft\\Windows\\WER", true,
        true, false, L"");
    return groups;
}

static bool IsUnderRoot(const std::wstring& rootLower, const std::wstring& candidateLower) {
    if (candidateLower.size() <= rootLower.size()) return false;
    if (candidateLower.compare(0, rootLower.size(), rootLower) != 0) return false;
    wchar_t boundary = candidateLower[rootLower.size()];
    return boundary == L'\\' || boundary == L'/';
}

struct ScanStats {
    uint64_t bytes = 0;
    uint32_t files = 0;
};

static void ScanTree(const std::wstring& dir, ScanStats& stats, int depth,
                     const std::atomic<bool>* cancel) {
    if (depth > 40) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        // Checked per entry: a big cache folder takes seconds to walk, and a cancel must
        // take effect while it is being walked, not only between folders.
        if (cancel && cancel->load(std::memory_order_relaxed)) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring child = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanTree(child, stats, depth + 1, cancel);
        } else {
            stats.bytes += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            ++stats.files;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// The Recycle Bin has no folder to walk: the shell keeps it per volume and reports the
// totals for all of them in one call. A null root asks for every drive.
static bool QueryRecycleBin(uint64_t& bytes, uint32_t& items) {
    SHQUERYRBINFO info = {};
    info.cbSize = sizeof(info);
    if (FAILED(SHQueryRecycleBinW(nullptr, &info))) return false;
    bytes = info.i64Size > 0 ? (uint64_t)info.i64Size : 0;
    items = info.i64NumItems > 0 ? (uint32_t)info.i64NumItems : 0;
    return true;
}

void ScanCleanGroup(CleanGroup& group, const std::atomic<bool>* cancel) {
    group.bytes = 0;
    group.files = 0;
    group.scanned = true;
    group.error.clear();
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        group.scanned = false;  // stopped before it began: nothing measured
        return;
    }
    if (group.recycleBin) {
        // One shell call, so a cancel can only arrive before or after it; either way the
        // row is either measured or not, never half of a folder.
        uint64_t bytes = 0;
        uint32_t items = 0;
        if (!QueryRecycleBin(bytes, items)) {
            group.scanned = false;
            group.error = L"The Recycle Bin could not be measured";
            return;
        }
        group.exists = true;
        group.bytes = bytes;
        group.files = items;
        return;
    }
    if (!group.exists) {
        group.error = L"Folder not present on this PC";
        return;
    }
    WIN32_FIND_DATAW probe;
    HANDLE test = FindFirstFileW((group.path + L"\\*").c_str(), &probe);
    if (test == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        group.error = err == ERROR_ACCESS_DENIED
                          ? L"Needs administrator rights to measure"
                          : WinErrorText(err);
        return;
    }
    FindClose(test);

    ScanStats stats;
    ScanTree(group.path, stats, 0, cancel);
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        // Stopped part way: the counts describe only part of the folder, so the row is
        // reported as unmeasured rather than as a small cache.
        group.scanned = false;
        return;
    }
    group.bytes = stats.bytes;
    group.files = stats.files;
}

static void DeleteTree(const std::wstring& rootLower, const std::wstring& dir, CleanResult& result,
                       int depth) {
    if (depth > 40) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subdirs;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring child = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            ++result.skipped;
            continue;
        }
        if (!IsUnderRoot(rootLower, ToLower(child))) {
            ++result.skipped;
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            subdirs.push_back(child);
            continue;
        }
        uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        SetFileAttributesW(child.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
        if (DeleteFileW(child.c_str())) {
            result.bytesFreed += size;
            ++result.deleted;
        } else {
            ++result.skipped;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    for (const auto& sub : subdirs) DeleteTree(rootLower, sub, result, depth + 1);
    // Empty subfolders go, but the cleanup root itself always stays: removing
    // %TEMP% (or the browser cache folder) leaves the next scan with a missing path
    // and breaks any app still holding a handle inside it.
    if (depth > 0) RemoveDirectoryW(dir.c_str());  // only succeeds when empty
}

CleanResult DeleteUnderRoot(const std::wstring& root) {
    CleanResult result;
    if (root.empty()) return result;
    std::wstring trimmed = TrimTrailingSlash(root);
    DWORD attrs = GetFileAttributesW(trimmed.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return result;
    DeleteTree(ToLower(trimmed), trimmed, result, 0);
    return result;
}

// The reversible path used for the user's own files. Each item directly under `root`
// goes to the Recycle Bin as a whole, so a folder comes back with its file layout and
// nothing is destroyed: the files are moved on the same volume, not unlinked. Items are
// handed to the shell in batches - one call per file would spin up that machinery
// thousands of times on a busy folder - and each is checked afterwards, because a file
// another program holds open stays put and must be reported as skipped.
CleanResult RecycleUnderRoot(const std::wstring& root) {
    CleanResult result;
    if (root.empty()) return result;
    std::wstring trimmed = TrimTrailingSlash(root);
    DWORD attrs = GetFileAttributesW(trimmed.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return result;
    std::wstring rootLower = ToLower(trimmed);

    struct Item {
        std::wstring path;
        uint64_t bytes = 0;
        uint32_t files = 0;
    };
    std::vector<Item> items;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((trimmed + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring child = trimmed + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            ++result.skipped;
            continue;
        }
        if (!IsUnderRoot(rootLower, ToLower(child))) {
            ++result.skipped;
            continue;
        }
        Item item;
        item.path = std::move(child);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanStats stats;
            ScanTree(item.path, stats, 1, nullptr);
            item.bytes = stats.bytes;
            item.files = stats.files;
        } else {
            item.bytes = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            item.files = 1;
        }
        items.push_back(std::move(item));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    // A double-null list is what the shell expects; keep each list well inside the
    // documented buffer so a folder with thousands of entries still goes through.
    const size_t kBatchChars = 30000;
    const size_t kBatchItems = 64;
    for (size_t start = 0; start < items.size();) {
        std::wstring list;
        size_t end = start;
        while (end < items.size() && end - start < kBatchItems) {
            const std::wstring& path = items[end].path;
            if (end > start && list.size() + path.size() + 2 > kBatchChars) break;
            list += path;
            list.push_back(L'\0');
            ++end;
        }
        list.push_back(L'\0');

        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = list.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        SHFileOperationW(&op);

        for (size_t i = start; i < end; ++i) {
            if (GetFileAttributesW(items[i].path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ++result.skipped;
                continue;
            }
            result.bytesRecycled += items[i].bytes;
            result.deleted += items[i].files;
        }
        start = end;
    }
    return result;
}

// Empties the shell's Recycle Bin on every drive. The figures are taken before and after,
// so only what actually left is reported: an item another program is holding open stays
// behind and is counted as skipped rather than as freed.
static CleanResult EmptyRecycleBin() {
    CleanResult result;
    uint64_t beforeBytes = 0;
    uint32_t beforeItems = 0;
    if (!QueryRecycleBin(beforeBytes, beforeItems) || beforeItems == 0) return result;

    SHEmptyRecycleBinW(nullptr, nullptr,
                       SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);

    uint64_t afterBytes = 0;
    uint32_t afterItems = 0;
    if (!QueryRecycleBin(afterBytes, afterItems)) {
        result.bytesFreed = beforeBytes;
        result.deleted = beforeItems;
        return result;
    }
    result.bytesFreed = beforeBytes > afterBytes ? beforeBytes - afterBytes : 0;
    result.deleted = beforeItems > afterItems ? beforeItems - afterItems : 0;
    result.skipped = afterItems;
    return result;
}

CleanResult CleanGroupContents(const CleanGroup& group) {
    if (group.recycleBin) return EmptyRecycleBin();
    if (group.path.empty()) return CleanResult();
    if (group.personal) return RecycleUnderRoot(group.path);
    return DeleteUnderRoot(group.path);
}

// --------------------------------------------------------------- startup
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kBackupKey = L"Software\\ZanpharsWinSuite\\DisabledStartup";

struct RunSource {
    const wchar_t* id;
    const wchar_t* label;
    HKEY hive;
    DWORD flags;
    StartupScope scope;
    bool system;
};

// HKCU\...\Run is a shared key on 64-bit Windows: reading it with the 32-bit view
// returns the same values, so only one source is listed for the user hive.
static const RunSource kRunSources[] = {
    {L"HKCU_RUN", L"User - registry Run", HKEY_CURRENT_USER, KEY_WOW64_64KEY,
     StartupScope::HkcuRun, false},
    {L"HKLM_RUN", L"System - registry Run (64-bit)", HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY,
     StartupScope::HklmRun, true},
    {L"HKLM_RUN32", L"System - registry Run (32-bit)", HKEY_LOCAL_MACHINE,
     KEY_WOW64_32KEY, StartupScope::HklmRun32, true},
};

static const RunSource* FindSource(const std::wstring& id) {
    for (const auto& s : kRunSources)
        if (id == s.id) return &s;
    return nullptr;
}

static std::wstring BackupName(const std::wstring& sourceId, const std::wstring& valueName) {
    return sourceId + L"|" + valueName;
}

static std::wstring EncodeBackup(const RegValue& value) {
    return std::to_wstring(value.type) + L"|" + value.data;
}

static bool DecodeBackup(const std::wstring& raw, RegValue& value) {
    size_t bar = raw.find(L'|');
    if (bar == std::wstring::npos) return false;
    value.type = (DWORD)_wtoi(raw.substr(0, bar).c_str());
    if (value.type != REG_SZ && value.type != REG_EXPAND_SZ) value.type = REG_SZ;
    value.data = raw.substr(bar + 1);
    return true;
}

static void AddRunValues(const RunSource& src, std::vector<StartupItem>& items) {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(src.hive, kRunKey, 0, KEY_QUERY_VALUE | src.flags, &key);
    if (rc != ERROR_SUCCESS) return;
    DWORD index = 0;
    for (;;) {
        wchar_t nameBuf[512];
        DWORD nameLen = 512;
        DWORD type = 0;
        DWORD dataLen = 0;
        LONG qr = RegEnumValueW(key, index, nameBuf, &nameLen, nullptr, &type, nullptr, &dataLen);
        if (qr == ERROR_NO_MORE_ITEMS) break;
        if (qr != ERROR_SUCCESS) {
            ++index;
            continue;
        }
        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            std::wstring data(dataLen / sizeof(wchar_t) + 1, L'\0');
            DWORD size = (DWORD)(data.size() * sizeof(wchar_t));
            if (RegQueryValueExW(key, nameBuf, nullptr, &type, (LPBYTE)data.data(), &size) ==
                ERROR_SUCCESS) {
                data.resize(size / sizeof(wchar_t));
                while (!data.empty() && data.back() == L'\0') data.pop_back();
                StartupItem item;
                item.name = nameBuf;
                item.command = data;
                item.locationLabel = src.label;
                item.sourceId = src.id;
                item.valueName = nameBuf;
                item.scope = src.scope;
                item.systemItem = src.system;
                item.folderItem = false;
                items.push_back(item);
            }
        }
        ++index;
    }
    RegCloseKey(key);
}

static void AddFolderItems(const wchar_t* sourceId, const wchar_t* label, REFKNOWNFOLDERID folder,
                           bool system, StartupScope scope, std::vector<StartupItem>& items) {
    std::wstring dir = KnownFolderPath(folder);
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!_wcsicmp(fd.cFileName, L"desktop.ini")) continue;
        std::wstring fullName = fd.cFileName;
        StartupItem item;
        item.folderItem = true;
        item.systemItem = system;
        item.scope = scope;
        item.sourceId = sourceId;
        item.locationLabel = label;
        item.command = dir + L"\\" + fullName;
        item.enabled = true;
        if (EndsWith(fullName, L".disabled")) {
            item.enabled = false;
            fullName = fullName.substr(0, fullName.size() - 9);
            item.command = dir + L"\\" + fullName;
        }
        item.name = fullName;
        items.push_back(item);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void AddDisabledFromBackup(std::vector<StartupItem>& items) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kBackupKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return;
    DWORD index = 0;
    for (;;) {
        wchar_t nameBuf[1024];
        DWORD nameLen = 1024;
        DWORD type = 0;
        DWORD dataLen = 0;
        LONG qr = RegEnumValueW(key, index, nameBuf, &nameLen, nullptr, &type, nullptr, &dataLen);
        if (qr == ERROR_NO_MORE_ITEMS) break;
        if (qr != ERROR_SUCCESS) {
            ++index;
            continue;
        }
        std::wstring data(dataLen / sizeof(wchar_t) + 1, L'\0');
        DWORD size = (DWORD)(data.size() * sizeof(wchar_t));
        if (RegQueryValueExW(key, nameBuf, nullptr, &type, (LPBYTE)data.data(), &size) !=
            ERROR_SUCCESS) {
            ++index;
            continue;
        }
        data.resize(size / sizeof(wchar_t));
        while (!data.empty() && data.back() == L'\0') data.pop_back();

        std::wstring rawName = nameBuf;
        size_t bar = rawName.find(L'|');
        if (bar == std::wstring::npos) {
            ++index;
            continue;
        }
        std::wstring sourceId = rawName.substr(0, bar);
        std::wstring valueName = rawName.substr(bar + 1);
        const RunSource* src = FindSource(sourceId);
        RegValue rv;
        if (!src || !DecodeBackup(data, rv)) {
            ++index;
            continue;
        }
        StartupItem item;
        item.name = valueName;
        item.command = rv.data;
        item.locationLabel = src->label;
        item.sourceId = sourceId;
        item.valueName = valueName;
        item.scope = src->scope;
        item.systemItem = src->system;
        item.enabled = false;
        items.push_back(item);
        ++index;
    }
    RegCloseKey(key);
}

std::vector<StartupItem> LoadStartupItems() {
    std::vector<StartupItem> items;
    for (const auto& src : kRunSources) AddRunValues(src, items);
    AddFolderItems(L"FOLDER_USER", L"User - Startup folder", FOLDERID_Startup, false,
                   StartupScope::FolderUser, items);
    AddFolderItems(L"FOLDER_COMMON", L"All users - Startup folder", FOLDERID_CommonStartup, true,
                   StartupScope::FolderCommon, items);
    AddDisabledFromBackup(items);

    std::stable_sort(items.begin(), items.end(), [](const StartupItem& a, const StartupItem& b) {
        if (a.enabled != b.enabled) return a.enabled;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return items;
}

bool SetStartupEnabled(const StartupItem& item, bool enable, std::wstring& error) {
    error.clear();

    if (item.folderItem) {
        std::wstring disabledPath = item.command + L".disabled";
        if (enable) {
            if (!MoveFileExW(disabledPath.c_str(), item.command.c_str(),
                             MOVEFILE_REPLACE_EXISTING)) {
                error = WinErrorText(GetLastError());
                return false;
            }
        } else {
            if (!MoveFileExW(item.command.c_str(), disabledPath.c_str(),
                             MOVEFILE_REPLACE_EXISTING)) {
                error = WinErrorText(GetLastError());
                return false;
            }
        }
        return true;
    }

    const RunSource* src = FindSource(item.sourceId);
    if (!src) {
        error = L"Unknown startup source";
        return false;
    }
    std::wstring backupName = BackupName(item.sourceId, item.valueName);

    if (enable) {
        RegValue stored;
        if (!ReadRegValue(HKEY_CURRENT_USER, kBackupKey, backupName, 0, stored, error)) return false;
        RegValue restored;
        if (!DecodeBackup(stored.data, restored)) {
            error = L"Stored entry is unreadable";
            return false;
        }
        if (!WriteRegValue(src->hive, kRunKey, item.valueName, src->flags, restored, error))
            return false;
        std::wstring cleanupErr;
        DeleteRegValue(HKEY_CURRENT_USER, kBackupKey, backupName, 0, cleanupErr);
        return true;
    }

    RegValue current;
    if (!ReadRegValue(src->hive, kRunKey, item.valueName, src->flags, current, error)) return false;
    RegValue stored;
    stored.type = REG_SZ;
    stored.data = EncodeBackup(current);
    if (!WriteRegValue(HKEY_CURRENT_USER, kBackupKey, backupName, 0, stored, error)) return false;
    if (!DeleteRegValue(src->hive, kRunKey, item.valueName, src->flags, error)) {
        std::wstring undo;
        DeleteRegValue(HKEY_CURRENT_USER, kBackupKey, backupName, 0, undo);
        return false;
    }
    return true;
}

// ------------------------------------------------- service deactivator
static const wchar_t* kServiceJournalKey = L"Software\\ZanpharsWinSuite\\DeactivatedServices";

static std::wstring ServiceImageFromConfig(const std::wstring& imagePath) {
    std::wstring path = imagePath;
    if (!path.empty() && path.front() == L'"') {
        size_t close = path.find(L'"', 1);
        path = close == std::wstring::npos ? path.substr(1) : path.substr(1, close - 1);
    } else {
        // Unquoted ImagePath: cut the arguments off after the executable name.
        size_t exe = ToLower(path).find(L".exe");
        if (exe != std::wstring::npos) path = path.substr(0, exe + 4);
    }
    // Driver paths arrive as \??\C:\... which ExpandEnvironmentStrings leaves alone.
    if (path.compare(0, 4, L"\\??\\") == 0) path = path.substr(4);
    if (path.compare(0, 4, L"\\\\?\\") == 0) path = path.substr(4);

    std::wstring expanded;
    if (!ExpandEnv(path, expanded) || expanded.empty()) expanded = path;
    std::replace(expanded.begin(), expanded.end(), L'/', L'\\');
    return TrimTrailingSlash(expanded);
}

bool IsThirdPartyServicePath(const std::wstring& imagePath) {
    std::wstring path = ToLower(ServiceImageFromConfig(imagePath));
    if (path.empty()) return false;

    wchar_t winDir[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(winDir, MAX_PATH)) return false;
    std::wstring windows = ToLower(TrimTrailingSlash(winDir));
    if (path.size() > windows.size() && path.compare(0, windows.size(), windows) == 0 &&
        path[windows.size()] == L'\\')
        return false;

    // A bare name (no drive, no variable) is resolved by the service manager inside
    // System32, so it is a Windows component too.
    if (path.find(L":") == std::wstring::npos && path.compare(0, 2, L"\\\\") != 0)
        return false;
    return true;
}

// The CompanyName in the executable's version block is the most reliable sign of who
// really ships a service: Defender's MsMpEng.exe lives under ProgramData, not Windows,
// and WSL ships from its own folder. Empty when the file has no version resource.
static std::wstring FileCompanyName(const std::wstring& path) {
    if (path.empty()) return L"";
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return L"";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return L"";

    struct Translation {
        WORD language;
        WORD codePage;
    };
    Translation* translations = nullptr;
    UINT translationBytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&translations,
                        &translationBytes) ||
        !translations || translationBytes < sizeof(Translation))
        return L"";

    wchar_t table[64];
    swprintf(table, 64, L"\\StringFileInfo\\%04x%04x\\CompanyName", translations[0].language,
             translations[0].codePage);
    wchar_t* value = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(data.data(), table, (void**)&value, &bytes) || !value || bytes == 0)
        return L"";
    return value;
}

static bool IsMicrosoftPublisher(const std::wstring& path) {
    if (ToLower(FileCompanyName(path)).rfind(L"microsoft", 0) == 0) return true;
    // Some Microsoft executables (GameInput) carry no usable version block, so the
    // install location decides instead.
    std::wstring lower = ToLower(path);
    return lower.find(L"\\windowsapps\\") != std::wstring::npos ||
           lower.find(L"\\programdata\\microsoft\\") != std::wstring::npos ||
           lower.find(L"\\program files\\microsoft ") != std::wstring::npos ||
           lower.find(L"\\program files (x86)\\microsoft ") != std::wstring::npos;
}

// Security software is never offered, even when it is third party: switching an
// antivirus off is not a performance tweak.
static bool IsSecuritySoftware(const std::wstring& name, const std::wstring& display,
                               const std::wstring& path) {
    static const wchar_t* kWords[] = {
        L"defender", L"mcafee",      L"norton",     L"avast",        L"avg ",
        L"kaspersky", L"bitdefender", L"eset ",      L"sophos",       L"malwarebytes",
        L"webroot",  L"trend micro", L"avira",      L"comodo",       L"zonealarm",
        L"f-secure", L"antivirus",   L"anti-virus", L"securityhealth", L"wscsvc",
    };
    std::wstring haystack = ToLower(name + L" " + display + L" " + path);
    for (const wchar_t* word : kWords)
        if (haystack.find(word) != std::wstring::npos) return true;
    return false;
}

// Services Windows cannot run without. The third-party filter already keeps these
// out of the list; this is the second lock on the door.
static bool IsCoreServiceName(const std::wstring& name) {
    static const wchar_t* kNames[] = {
        L"RpcSs",     L"DcomLaunch", L"RpcEptMapper", L"LSM",       L"Power",
        L"PlugPlay",  L"ProfSvc",    L"Schedule",     L"Winmgmt",   L"EventLog",
        L"UserManager", L"StateRepository", L"SysMain", L"Themes", L"AudioSrv",
        L"BrokerInfrastructure", L"SystemEventsBroker", L"BFE", L"MpsSvc",
    };
    for (const wchar_t* candidate : kNames)
        if (_wcsicmp(name.c_str(), candidate) == 0) return true;
    return false;
}

struct ServiceJournalRecord {
    DWORD start = SERVICE_AUTO_START;
    std::wstring display;
};

static bool IsKnownStartType(DWORD start) {
    return start == SERVICE_BOOT_START || start == SERVICE_SYSTEM_START ||
           start == SERVICE_AUTO_START || start == SERVICE_DEMAND_START;
}

// The journal lives in the user's own hive, so it is readable even when the service
// manager will not let a standard user ask about the service itself.
static bool ReadServiceJournal(const std::wstring& name, ServiceJournalRecord& out) {
    RegValue stored;
    std::wstring error;
    if (!ReadRegValue(HKEY_CURRENT_USER, kServiceJournalKey, name, 0, stored, error))
        return false;
    size_t bar = stored.data.find(L'|');
    DWORD start = (DWORD)_wtoi(stored.data.substr(0, bar).c_str());
    if (!IsKnownStartType(start)) return false;
    out.start = start;
    out.display = bar == std::wstring::npos ? std::wstring() : stored.data.substr(bar + 1);
    return true;
}

static bool WriteServiceJournal(const std::wstring& name, DWORD start,
                                const std::wstring& display, std::wstring& error) {
    RegValue stored;
    stored.type = REG_SZ;
    stored.data = std::to_wstring(start) + L"|" + display;
    return WriteRegValue(HKEY_CURRENT_USER, kServiceJournalKey, name, 0, stored, error);
}

static void ForgetServiceJournal(const std::wstring& name) {
    std::wstring ignored;
    DeleteRegValue(HKEY_CURRENT_USER, kServiceJournalKey, name, 0, ignored);
}

static bool QueryServiceConfigData(SC_HANDLE service, QUERY_SERVICE_CONFIGW& out,
                                   std::vector<BYTE>& storage, std::wstring& error) {
    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    if (needed == 0) {
        error = WinErrorText(GetLastError());
        return false;
    }
    storage.assign(needed, 0);
    auto* config = (QUERY_SERVICE_CONFIGW*)storage.data();
    if (!QueryServiceConfigW(service, config, needed, &needed)) {
        error = WinErrorText(GetLastError());
        return false;
    }
    out = *config;
    return true;
}

std::vector<ServiceEntry> LoadServices() {
    std::vector<ServiceEntry> services;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr,
                                   SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return services;

    std::vector<BYTE> buffer(64 * 1024);
    DWORD bytesNeeded = 0;
    DWORD count = 0;
    bool enumerated = false;
    for (int attempt = 0; attempt < 5 && !enumerated; ++attempt) {
        DWORD resume = 0;
        if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                                  buffer.data(), (DWORD)buffer.size(), &bytesNeeded, &count,
                                  &resume, nullptr)) {
            enumerated = true;
        } else if (GetLastError() == ERROR_MORE_DATA && bytesNeeded > buffer.size()) {
            buffer.resize(bytesNeeded + 8 * 1024);
        } else {
            count = 0;
            break;
        }
    }
    if (!enumerated) {
        CloseServiceHandle(scm);
        return services;
    }

    auto* entries = (ENUM_SERVICE_STATUS_PROCESSW*)buffer.data();
    for (DWORD i = 0; i < count; ++i) {
        const ENUM_SERVICE_STATUS_PROCESSW& raw = entries[i];
        if ((raw.ServiceStatusProcess.dwServiceType & SERVICE_WIN32) == 0) continue;
        if (raw.ServiceStatusProcess.dwServiceType & SERVICE_DRIVER) continue;

        ServiceEntry entry;
        entry.name = raw.lpServiceName ? raw.lpServiceName : L"";
        entry.display = raw.lpDisplayName ? raw.lpDisplayName : entry.name;
        entry.state = raw.ServiceStatusProcess.dwCurrentState;
        if (entry.name.empty()) continue;

        bool haveConfig = false;
        SC_HANDLE service =
            OpenServiceW(scm, entry.name.c_str(), SERVICE_QUERY_CONFIG);
        if (service) {
            QUERY_SERVICE_CONFIGW config = {};
            std::vector<BYTE> storage;
            std::wstring configError;
            if (QueryServiceConfigData(service, config, storage, configError)) {
                haveConfig = true;
                entry.startType = config.dwStartType;
                entry.imagePath = ServiceImageFromConfig(config.lpBinaryPathName
                                                             ? config.lpBinaryPathName
                                                             : L"");
                if (config.lpDisplayName && *config.lpDisplayName)
                    entry.display = config.lpDisplayName;
            }
            CloseServiceHandle(service);
        }
        entry.driver = (raw.ServiceStatusProcess.dwServiceType & SERVICE_DRIVER) != 0;
        entry.thirdParty = IsThirdPartyServicePath(entry.imagePath) &&
                           !IsMicrosoftPublisher(entry.imagePath) &&
                           !IsSecuritySoftware(entry.name, entry.display, entry.imagePath);
        entry.autoStart = entry.startType == SERVICE_AUTO_START;

        ServiceJournalRecord journal;
        entry.deactivated = ReadServiceJournal(entry.name, journal);
        if (entry.deactivated) {
            entry.originalStart = journal.start;
            if (!haveConfig) entry.startType = SERVICE_DISABLED;
        }

        // Auto-start third-party services are the ones worth turning off, plus
        // anything WinSuite already turned off so it can be turned back on.
        if (!entry.deactivated && !(haveConfig && entry.autoStart && entry.thirdParty)) continue;

        if (!entry.deactivated) {
            if (!entry.thirdParty)
                entry.note = L"Windows component";
            else if (IsCoreServiceName(entry.name))
                entry.note = L"Core Windows service";
        }
        entry.canDeactivate =
            entry.thirdParty && !entry.deactivated && haveConfig && entry.autoStart &&
            entry.startType != SERVICE_DISABLED && !IsCoreServiceName(entry.name);
        services.push_back(entry);
    }
    CloseServiceHandle(scm);

    std::stable_sort(services.begin(), services.end(), [](const ServiceEntry& a,
                                                         const ServiceEntry& b) {
        if (a.deactivated != b.deactivated) return !a.deactivated;
        return _wcsicmp(a.display.c_str(), b.display.c_str()) < 0;
    });
    return services;
}

static bool WaitForServiceState(SC_HANDLE service, DWORD wanted, DWORD timeoutMs) {
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    SERVICE_STATUS_PROCESS status = {};
    DWORD bytes = 0;
    for (;;) {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status,
                                  sizeof(status), &bytes))
            return false;
        if (status.dwCurrentState == wanted) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(120);
    }
}

ServiceChange DeactivateService(const ServiceEntry& entry) {
    ServiceChange result;
    if (!entry.canDeactivate) {
        result.error = L"WinSuite does not offer to deactivate this service";
        return result;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        result.error = WinErrorText(GetLastError());
        return result;
    }
    SC_HANDLE service = OpenServiceW(scm, entry.name.c_str(),
                                     SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG |
                                         SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_START);
    if (!service) {
        DWORD error = GetLastError();
        result.error = WinErrorText(error);
        result.adminNeeded = error == ERROR_ACCESS_DENIED;
        CloseServiceHandle(scm);
        return result;
    }

    QUERY_SERVICE_CONFIGW config = {};
    std::vector<BYTE> storage;
    std::wstring configError;
    if (!QueryServiceConfigData(service, config, storage, configError)) {
        result.error = L"could not read the service configuration: " + configError;
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return result;
    }
    if (config.dwStartType == SERVICE_DISABLED) {
        result.error = L"the service is already disabled";
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return result;
    }

    std::wstring display = config.lpDisplayName && *config.lpDisplayName
                               ? config.lpDisplayName
                               : entry.display;
    std::wstring journalError;
    if (!WriteServiceJournal(entry.name, config.dwStartType, display, journalError)) {
        result.error = L"could not record the original start type: " + journalError;
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return result;
    }

    if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
        DWORD error = GetLastError();
        ForgetServiceJournal(entry.name);  // nothing changed, so nothing to restore
        result.error = L"Windows refused to disable it: " + WinErrorText(error);
        result.adminNeeded = error == ERROR_ACCESS_DENIED;
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return result;
    }

    SERVICE_STATUS_PROCESS status = {};
    DWORD bytes = 0;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status),
                             &bytes) &&
        status.dwCurrentState != SERVICE_STOPPED) {
        SERVICE_STATUS ignored = {};
        // Short wait only: the page runs this on the UI thread, and a service that
        // needs longer than this is reported as still running rather than hanging
        // the window. It stays disabled either way.
        if (!ControlService(service, SERVICE_CONTROL_STOP, &ignored) ||
            !WaitForServiceState(service, SERVICE_STOPPED, 2500))
            result.stillRunning = true;
    }

    result.ok = true;
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return result;
}

ServiceChange ReactivateService(const ServiceEntry& entry) {
    ServiceChange result;
    ServiceJournalRecord journal;
    if (!ReadServiceJournal(entry.name, journal)) {
        result.error = L"WinSuite has no record of disabling this service";
        return result;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        result.error = WinErrorText(GetLastError());
        return result;
    }
    SC_HANDLE service = OpenServiceW(
        scm, entry.name.c_str(), SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG | SERVICE_START);
    if (!service) {
        DWORD error = GetLastError();
        result.error = WinErrorText(error);
        result.adminNeeded = error == ERROR_ACCESS_DENIED;
        CloseServiceHandle(scm);
        return result;
    }

    if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, journal.start, SERVICE_NO_CHANGE,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
        DWORD error = GetLastError();
        result.error = L"Windows refused to restore it: " + WinErrorText(error);
        result.adminNeeded = error == ERROR_ACCESS_DENIED;
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return result;
    }

    ForgetServiceJournal(entry.name);
    result.ok = true;
    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    // It kept running until the next boot otherwise, so start it now when it used to.
    // The request runs on its own thread: StartServiceW does not return until the
    // service reports running, and a service that never answers would otherwise
    // freeze the window for as long as the service manager is willing to wait.
    if (journal.start == SERVICE_AUTO_START) {
        std::wstring name = entry.name;
        std::thread([name]() {
            SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!manager) return;
            SC_HANDLE started = OpenServiceW(manager, name.c_str(), SERVICE_START);
            if (started) {
                StartServiceW(started, 0, nullptr);
                CloseServiceHandle(started);
            }
            CloseServiceHandle(manager);
        }).detach();
    }
    return result;
}

// ------------------------------------------------------------- large files
static bool IsProtectedLocation(const std::wstring& lowerPath) {
    std::vector<std::wstring> protectedDirs;
    wchar_t buf[MAX_PATH * 2] = {};

    if (GetWindowsDirectoryW(buf, MAX_PATH)) protectedDirs.push_back(ToLower(buf));
    for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432",
                               L"ProgramData"}) {
        DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH * 2);
        if (n > 0 && n < MAX_PATH * 2) protectedDirs.push_back(ToLower(buf));
    }
    wchar_t sysDrive[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"SystemDrive", sysDrive, MAX_PATH)) {
        std::wstring root = ToLower(sysDrive);
        for (const wchar_t* extra : {L"\\$Recycle.Bin", L"\\System Volume Information", L"\\Recovery",
                                     L"\\Boot", L"\\EFI", L"\\PerfLogs", L"\\Windows.old"})
            protectedDirs.push_back(root + extra);
    }
    for (const auto& dir : protectedDirs) {
        std::wstring trimmed = ToLower(TrimTrailingSlash(dir));
        if (!trimmed.empty() && IsUnderRoot(trimmed, lowerPath)) return true;
    }

    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) && ToLower(exe) == lowerPath) return true;
    return false;
}

std::vector<std::wstring> LargeFileRoots() {
    std::vector<std::wstring> roots;
    std::wstring profile = KnownFolderPath(FOLDERID_Profile);
    if (!profile.empty()) roots.push_back(TrimTrailingSlash(profile));

    wchar_t sysDrive[MAX_PATH] = {};
    GetEnvironmentVariableW(L"SystemDrive", sysDrive, MAX_PATH);
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        if (!_wcsnicmp(root, sysDrive, 2)) continue;  // system drive is covered by the profile
        roots.push_back(root);
    }
    return roots;
}

static void ScanLargeTree(const std::wstring& dir, const LargeFileScan& options,
                          std::vector<LargeFile>& out, int depth) {
    if (depth > (int)options.maxDepth) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (!_wcsicmp(fd.cFileName, L"$Recycle.Bin") ||
            !_wcsicmp(fd.cFileName, L"System Volume Information"))
            continue;
        std::wstring child = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanLargeTree(child, options, out, depth + 1);
            continue;
        }
        uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (size < options.minBytes) continue;
        LargeFile file;
        file.path = child;
        file.bytes = size;
        file.modifiedTicks =
            ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        out.push_back(std::move(file));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void ScanLargeFiles(const std::vector<std::wstring>& roots, const LargeFileScan& options,
                    std::vector<LargeFile>& out) {
    out.clear();
    for (const auto& root : roots) {
        std::wstring trimmed = TrimTrailingSlash(root);
        if (trimmed.empty()) continue;
        DWORD attrs = GetFileAttributesW(trimmed.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
        ScanLargeTree(trimmed, options, out, 0);
    }
    std::sort(out.begin(), out.end(), [](const LargeFile& a, const LargeFile& b) {
        if (a.bytes != b.bytes) return a.bytes > b.bytes;
        return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
    });
    if (out.size() > options.maxResults) out.resize(options.maxResults);
}

bool CanRemoveLargeFile(const std::wstring& path, const std::vector<std::wstring>& roots,
                        std::wstring& error) {
    error.clear();
    if (path.empty()) {
        error = L"empty path";
        return false;
    }
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        error = L"file no longer exists";
        return false;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        error = L"not a file";
        return false;
    }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        error = L"refusing to follow a link";
        return false;
    }
    std::wstring lower = ToLower(path);
    if (IsProtectedLocation(lower)) {
        error = L"protected Windows or program location";
        return false;
    }
    bool inside = false;
    for (const auto& root : roots) {
        std::wstring lowerRoot = ToLower(TrimTrailingSlash(root));
        if (lowerRoot.empty()) continue;
        if (lower == lowerRoot || IsUnderRoot(lowerRoot, lower)) {
            inside = true;
            break;
        }
    }
    if (!inside) {
        error = L"outside the scanned folders";
        return false;
    }
    return true;
}

LargeDeleteResult RemoveLargeFiles(const std::vector<std::wstring>& paths,
                                   const std::vector<std::wstring>& roots) {
    LargeDeleteResult result;
    std::vector<std::wstring> accepted;
    std::vector<uint64_t> sizes;

    for (const auto& path : paths) {
        std::wstring reason;
        if (!CanRemoveLargeFile(path, roots, reason)) {
            ++result.failed;
            if (result.firstError.empty()) result.firstError = path + L": " + reason;
            continue;
        }
        uint64_t size = 0;
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        accepted.push_back(path);
        sizes.push_back(size);
    }
    if (accepted.empty()) return result;

    std::wstring list;
    for (const auto& path : accepted) {
        list += path;
        list.push_back(L'\0');
    }
    list.push_back(L'\0');

    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
    int rc = SHFileOperationW(&op);

    for (size_t i = 0; i < accepted.size(); ++i) {
        if (GetFileAttributesW(accepted[i].c_str()) == INVALID_FILE_ATTRIBUTES) {
            ++result.removed;
            result.bytes += sizes[i];
        } else {
            ++result.failed;
            if (result.firstError.empty()) {
                result.firstError = rc != 0
                                        ? L"recycle failed (code " + std::to_wstring(rc) + L")"
                                        : accepted[i] + L": still present (in use?)";
            }
        }
    }
    return result;
}

std::wstring FormatFileTime(uint64_t ticks) {
    if (ticks == 0) return L"-";
    FILETIME ft = {};
    ft.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFull);
    ft.dwHighDateTime = (DWORD)(ticks >> 32);
    FILETIME local = {};
    if (!FileTimeToLocalFileTime(&ft, &local)) return L"-";
    SYSTEMTIME st = {};
    if (!FileTimeToSystemTime(&local, &st)) return L"-";
    wchar_t buf[32];
    swprintf(buf, 32, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}
