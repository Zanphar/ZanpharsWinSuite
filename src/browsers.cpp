// browsers.cpp - the Browser Cleaner engine: what each browser stores, how much
// of it there is, and removing it with the browser closed.
#include "extras.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>

#include "core.h"

namespace {

std::wstring EnvDir(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return L"";
    return buf;
}

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return text;
}

bool IsDir(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Sums one file or a whole tree. Follows the same rules as the cleaner: files only.
void Measure(const std::wstring& path, uint64_t& bytes, uint32_t& files) {
    if (path.empty()) return;
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
            bytes += ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
            ++files;
        }
        return;
    }

    WIN32_FIND_DATAW find = {};
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(find.cFileName, L".") == 0 || wcscmp(find.cFileName, L"..") == 0) continue;
        std::wstring child = path + L"\\" + find.cFileName;
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            Measure(child, bytes, files);
        } else {
            bytes += ((uint64_t)find.nFileSizeHigh << 32) | find.nFileSizeLow;
            ++files;
        }
    } while (FindNextFileW(h, &find));
    FindClose(h);
}

void DeleteOne(const std::wstring& path, BrowserCleanResult& result) {
    if (path.empty()) return;
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        CleanResult r = DeleteUnderRoot(path);  // contents only, folder stays
        result.bytesFreed += r.bytesFreed;
        result.deleted += r.deleted;
        result.skipped += r.skipped;
        return;
    }

    uint64_t bytes = 0;
    uint32_t files = 0;
    Measure(path, bytes, files);
    // SQLite keeps the same rows in these sidecars; leaving them behind keeps the data.
    const wchar_t* sidecars[] = {L"-wal", L"-shm", L"-journal"};
    bool removed = DeleteFileW(path.c_str()) != FALSE;
    for (const wchar_t* suffix : sidecars) DeleteFileW((path + suffix).c_str());
    if (removed) {
        result.bytesFreed += bytes;
        ++result.deleted;
    } else {
        ++result.skipped;
    }
}

std::wstring ChromiumProfile(const wchar_t* env, const wchar_t* relative) {
    std::wstring base = EnvDir(env);
    if (base.empty()) return L"";
    return base + L"\\" + relative;
}

BrowserEntry MakeChromium(const wchar_t* key, const wchar_t* name, const wchar_t* /*env*/,
                          const wchar_t* /*relative*/, const std::wstring& profile,
                          const std::vector<std::wstring>& processes) {
    BrowserEntry entry;
    entry.key = key;
    entry.name = name;
    entry.detail = profile;
    entry.processes = processes;
    entry.installed = !profile.empty() && (IsDir(profile) || Exists(profile));
    if (!entry.installed) return entry;

    auto addTrace = [&](const wchar_t* label, std::vector<std::wstring> paths) {
        BrowserTrace trace;
        trace.label = label;
        trace.paths = std::move(paths);
        entry.traces.push_back(trace);
    };

    addTrace(L"History", {profile + L"\\History"});
    std::vector<std::wstring> cookiePaths;
    if (Exists(profile + L"\\Network\\Cookies")) cookiePaths.push_back(profile + L"\\Network\\Cookies");
    cookiePaths.push_back(profile + L"\\Cookies");
    addTrace(L"Cookies", cookiePaths);
    addTrace(L"Autofill", {profile + L"\\Web Data"});
    addTrace(L"Cache",
             {profile + L"\\Cache", profile + L"\\Code Cache", profile + L"\\GPUCache"});
    addTrace(L"Sessions", {profile + L"\\Sessions", profile + L"\\Current Session",
                           profile + L"\\Last Session", profile + L"\\Current Tabs",
                           profile + L"\\Last Tabs"});
    return entry;
}

void FirefoxProfiles(std::vector<std::wstring>& out) {
    std::wstring roaming = EnvDir(L"APPDATA");
    if (roaming.empty()) return;
    std::wstring root = roaming + L"\\Mozilla\\Firefox\\Profiles";
    WIN32_FIND_DATAW find = {};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (wcscmp(find.cFileName, L".") == 0 || wcscmp(find.cFileName, L"..") == 0) continue;
        out.push_back(root + L"\\" + find.cFileName);
    } while (FindNextFileW(h, &find));
    FindClose(h);
}

struct ProcName {
    DWORD pid;
    std::wstring name;  // lower-case exe name
};

std::vector<ProcName> ProcessList() {
    std::vector<ProcName> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            out.push_back({entry.th32ProcessID, Lower(entry.szExeFile)});
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return out;
}

bool TargetsBrowser(const std::wstring& exe, const BrowserEntry& entry) {
    for (const auto& name : entry.processes)
        if (Lower(name) == exe) return true;
    return false;
}

}  // namespace

std::vector<BrowserEntry> BuildBrowserCatalog() {
    std::vector<BrowserEntry> catalog;

    const std::vector<std::wstring> chromeProcs = {L"chrome.exe"};
    const std::vector<std::wstring> edgeProcs = {L"msedge.exe"};
    const std::vector<std::wstring> braveProcs = {L"brave.exe"};
    const std::vector<std::wstring> vivaldiProcs = {L"vivaldi.exe"};
    const std::vector<std::wstring> operaProcs = {L"opera.exe"};
    const std::vector<std::wstring> firefoxProcs = {L"firefox.exe"};

    catalog.push_back(MakeChromium(
        L"chrome", L"Google Chrome", L"LOCALAPPDATA",
        L"Google\\Chrome\\User Data\\Default",
        ChromiumProfile(L"LOCALAPPDATA", L"Google\\Chrome\\User Data\\Default"), chromeProcs));
    catalog.push_back(MakeChromium(
        L"edge", L"Microsoft Edge", L"LOCALAPPDATA",
        L"Microsoft\\Edge\\User Data\\Default",
        ChromiumProfile(L"LOCALAPPDATA", L"Microsoft\\Edge\\User Data\\Default"), edgeProcs));

    // IE has no Chromium-style profile directory. Its history, cache and cookies
    // live in WinINet's shared Windows folders; Recovery holds the last session.
    // WebCache is shared by WinINet and can hold cookies and other state, so it is
    // deliberately not a trace here: deleting it with History would clear cookies
    // even when the Cookies item is unchecked. The Windows folder is the common
    // base, so the browser is listed wherever IE exists as part of Windows.
    BrowserEntry ie;
    ie.key = L"ie";
    ie.name = L"Internet Explorer";
    ie.processes = {L"iexplore.exe"};
    const std::wstring localWin = EnvDir(L"LOCALAPPDATA") + L"\\Microsoft\\Windows";
    const std::wstring roamingWin = EnvDir(L"APPDATA") + L"\\Microsoft\\Windows";
    ie.installed = !EnvDir(L"LOCALAPPDATA").empty() && IsDir(localWin);
    ie.detail = ie.installed ? localWin : L"";
    if (ie.installed) {
        auto addTrace = [&](const wchar_t* label, std::vector<std::wstring> paths) {
            BrowserTrace trace;
            trace.label = label;
            trace.paths = std::move(paths);
            ie.traces.push_back(trace);
        };
        addTrace(L"History", {localWin + L"\\History"});
        std::vector<std::wstring> cookiePaths = {localWin + L"\\INetCookies"};
        if (!EnvDir(L"APPDATA").empty())
            cookiePaths.push_back(roamingWin + L"\\Cookies");
        addTrace(L"Cookies", cookiePaths);
        addTrace(L"Cache", {localWin + L"\\INetCache"});
        addTrace(L"Sessions", {localWin + L"\\Internet Explorer\\Recovery"});
    }
    catalog.push_back(ie);

    catalog.push_back(MakeChromium(
        L"brave", L"Brave", L"LOCALAPPDATA",
        L"BraveSoftware\\Brave-Browser\\User Data\\Default",
        ChromiumProfile(L"LOCALAPPDATA", L"BraveSoftware\\Brave-Browser\\User Data\\Default"),
        braveProcs));
    catalog.push_back(MakeChromium(
        L"vivaldi", L"Vivaldi", L"LOCALAPPDATA", L"Vivaldi\\User Data\\Default",
        ChromiumProfile(L"LOCALAPPDATA", L"Vivaldi\\User Data\\Default"), vivaldiProcs));
    catalog.push_back(MakeChromium(
        L"opera", L"Opera", L"APPDATA", L"Opera Software\\Opera Stable",
        ChromiumProfile(L"APPDATA", L"Opera Software\\Opera Stable"), operaProcs));
    catalog.push_back(MakeChromium(
        L"operagx", L"Opera GX", L"APPDATA", L"Opera Software\\Opera GX Stable",
        ChromiumProfile(L"APPDATA", L"Opera Software\\Opera GX Stable"), operaProcs));

    BrowserEntry firefox;
    firefox.key = L"firefox";
    firefox.name = L"Firefox";
    firefox.processes = firefoxProcs;
    std::vector<std::wstring> profiles;
    FirefoxProfiles(profiles);
    firefox.installed = !profiles.empty();
    firefox.detail = profiles.empty() ? L"" : profiles.front();
    if (firefox.installed) {
        auto addTrace = [&](const wchar_t* label, const wchar_t* file) {
            BrowserTrace trace;
            trace.label = label;
            for (const auto& profile : profiles) {
                trace.paths.push_back(profile + L"\\" + file);
                trace.paths.push_back(profile + L"\\" + file + L"-wal");
                trace.paths.push_back(profile + L"\\" + file + L"-shm");
            }
            firefox.traces.push_back(trace);
        };
        addTrace(L"History", L"places.sqlite");
        addTrace(L"Cookies", L"cookies.sqlite");
        addTrace(L"Autofill", L"formhistory.sqlite");

        BrowserTrace cache;
        cache.label = L"Cache";
        BrowserTrace sessions;
        sessions.label = L"Sessions";
        for (const auto& profile : profiles) {
            cache.paths.push_back(profile + L"\\cache2");
            sessions.paths.push_back(profile + L"\\sessionstore-backups");
            sessions.paths.push_back(profile + L"\\sessionstore.jsonlz4");
        }
        firefox.traces.push_back(cache);
        firefox.traces.push_back(sessions);
    }
    catalog.push_back(firefox);
    return catalog;
}

void ScanBrowserEntry(BrowserEntry& entry) {
    entry.bytes = 0;
    entry.files = 0;
    for (auto& trace : entry.traces) {
        trace.bytes = 0;
        trace.files = 0;
        trace.exists = false;
        for (const auto& path : trace.paths) {
            uint64_t bytes = 0;
            uint32_t files = 0;
            Measure(path, bytes, files);
            if (files > 0 || IsDir(path)) trace.exists = trace.exists || IsDir(path);
            trace.bytes += bytes;
            trace.files += files;
        }
        entry.bytes += trace.bytes;
        entry.files += trace.files;
    }
    entry.scanned = true;
}

BrowserCleanResult CleanBrowserEntries(std::vector<BrowserEntry>& entries) {
    BrowserCleanResult total;
    for (auto& entry : entries) {
        if (!entry.selected || entry.running || !entry.installed) continue;
        for (const auto& trace : entry.traces) {
            for (const auto& path : trace.paths) DeleteOne(path, total);
        }
    }
    return total;
}

std::vector<std::wstring> RunningBrowsers(const std::vector<BrowserEntry>& entries) {
    std::vector<std::wstring> running;
    auto processes = ProcessList();
    for (const auto& entry : entries) {
        for (const auto& proc : processes) {
            if (TargetsBrowser(proc.name, entry)) {
                running.push_back(entry.name);
                break;
            }
        }
    }
    return running;
}

uint32_t CloseRunningBrowsers(const std::vector<BrowserEntry>& entries, std::wstring& report) {
    std::vector<DWORD> pids;
    for (const auto& proc : ProcessList()) {
        for (const auto& entry : entries) {
            if (TargetsBrowser(proc.name, entry)) {
                pids.push_back(proc.pid);
                break;
            }
        }
    }
    if (pids.empty()) {
        report = L"No browser was running.";
        return 0;
    }

    struct CloseCtx {
        const std::vector<DWORD>* pids;
    } ctx{&pids};

    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* ctx = reinterpret_cast<CloseCtx*>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (std::find(ctx->pids->begin(), ctx->pids->end(), pid) == ctx->pids->end())
                return TRUE;
            if (!IsWindowVisible(hwnd)) return TRUE;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    DWORD remaining = 0;
    for (int attempt = 0; attempt < 30; ++attempt) {
        Sleep(200);
        auto processes = ProcessList();
        remaining = 0;
        for (const auto& proc : processes) {
            for (const auto& entry : entries) {
                if (TargetsBrowser(proc.name, entry)) {
                    ++remaining;
                    break;
                }
            }
        }
        if (remaining == 0) break;
    }

    if (remaining == 0) {
        report = L"Browsers closed. Scan again, then clean.";
    } else {
        wchar_t buf[160];
        swprintf(buf, 160, L"%lu browser process(es) did not close; close them by hand.",
                 (unsigned long)remaining);
        report = buf;
    }
    return remaining;
}
