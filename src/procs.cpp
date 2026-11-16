// procs.cpp - task control for the Task Manager page and the sign-in agent.
//
// Listing is done with a toolhelp snapshot; anything that needs to look inside a
// process opens it with the narrowest rights that will do, and a process we cannot open
// is listed as read-only instead of disappearing from the list.
#include <windows.h>
#include <psapi.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "core.h"

namespace {

std::wstring Lowercase(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), (DWORD)text.size());
    return text;
}

// One lookup per SID, not per process: a PC with forty svchost hosts would otherwise
// resolve the same account forty times, and domain accounts cost a network round trip.
std::wstring LookupSidText(HANDLE token, std::map<std::wstring, std::wstring>& cache) {
    DWORD needed = 0;
    if (!GetTokenInformation(token, TokenUser, nullptr, 0, &needed) || !needed) return L"";
    std::vector<unsigned char> buffer(needed);
    auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    if (!GetTokenInformation(token, TokenUser, user, needed, &needed)) return L"";

    LPWSTR wideSid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &wideSid)) return L"";
    std::wstring sidText = wideSid;
    LocalFree(wideSid);

    auto found = cache.find(sidText);
    if (found != cache.end()) return found->second;

    wchar_t account[256] = {}, domain[256] = {};
    DWORD accountLen = 256, domainLen = 256;
    SID_NAME_USE use = SidTypeUnknown;
    std::wstring name;
    if (LookupAccountSidW(nullptr, user->User.Sid, account, &accountLen, domain, &domainLen,
                          &use)) {
        name = domain[0] ? std::wstring(domain) + L"\\" + account : std::wstring(account);
    }
    cache[sidText] = name;
    return name;
}

}  // namespace

std::wstring PriorityLabel(DWORD priority) {
    switch (priority) {
        case IDLE_PRIORITY_CLASS:
            return L"Idle";
        case BELOW_NORMAL_PRIORITY_CLASS:
            return L"Below normal";
        case NORMAL_PRIORITY_CLASS:
            return L"Normal";
        case ABOVE_NORMAL_PRIORITY_CLASS:
            return L"Above normal";
        case HIGH_PRIORITY_CLASS:
            return L"High";
        case REALTIME_PRIORITY_CLASS:
            return L"Real time";
        default:
            return L"Unknown";
    }
}

std::wstring AffinityText(ULONG_PTR mask) {
    if (!mask) return L"-";
    std::wstring text;
    wchar_t buffer[32];
    for (int cpu = 0; cpu < 64; ++cpu) {
        if (!((mask >> cpu) & 1)) continue;
        swprintf(buffer, 32, L"%s%d", text.empty() ? L"" : L",", cpu);
        text += buffer;
    }
    return text.empty() ? L"-" : L"CPU " + text;
}

std::vector<ProcRow> LoadProcessRows() {
    std::vector<ProcRow> rows;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return rows;

    std::map<std::wstring, std::wstring> sidCache;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcRow row;
            row.pid = entry.th32ProcessID;
            row.name = entry.szExeFile;
            ProcessIdToSessionId(row.pid, &row.sessionId);

            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                                             PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION,
                                         FALSE, row.pid);
            if (process) {
                wchar_t path[MAX_PATH * 4] = {};
                DWORD pathLen = (DWORD)(sizeof(path) / sizeof(path[0]));
                if (QueryFullProcessImageNameW(process, 0, path, &pathLen)) row.path = path;

                FILETIME created = {}, exited = {}, kernel = {}, user = {};
                if (GetProcessTimes(process, &created, &exited, &kernel, &user))
                    row.created = ((uint64_t)created.dwHighDateTime << 32) |
                                  created.dwLowDateTime;

                PROCESS_MEMORY_COUNTERS_EX counters = {};
                counters.cb = sizeof(counters);
                if (GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS*)&counters,
                                         sizeof(counters)))
                    row.workingSet = counters.WorkingSetSize;

                row.priority = GetPriorityClass(process);
                if (!row.priority) row.priority = NORMAL_PRIORITY_CLASS;
                DWORD_PTR processMask = 0, systemMask = 0;
                if (GetProcessAffinityMask(process, &processMask, &systemMask))
                    row.affinity = processMask;

                HANDLE token = nullptr;
                if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
                    row.user = LookupSidText(token, sidCache);
                    CloseHandle(token);
                }
                row.accessible = true;
                CloseHandle(process);
            }
            // A process we cannot open is still listed: the user can see it running, and
            // the page explains why acting on it needs administrator rights.
            rows.push_back(row);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    std::sort(rows.begin(), rows.end(), [](const ProcRow& a, const ProcRow& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return rows;
}

namespace {

bool HasWindow(DWORD pid) {
    struct Search {
        DWORD pid;
        bool found;
    } search = {pid, false};
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<Search*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner != state->pid || !IsWindowVisible(window)) return TRUE;
            if (GetWindow(window, GW_OWNER) != nullptr) return TRUE;
            state->found = true;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

void PostClose(DWORD pid) {
    struct Search {
        DWORD pid;
    } search = {pid};
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<Search*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner == state->pid && GetWindow(window, GW_OWNER) == nullptr)
                PostMessageW(window, WM_CLOSE, 0, 0);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
}

}  // namespace

bool EndRunningTask(DWORD pid, bool force, std::wstring& error) {
    error.clear();
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION,
                                 FALSE, pid);
    if (!process) {
        DWORD code = GetLastError();
        // A pid that no longer exists is not an access problem: the row was simply a
        // moment out of date by the time the button was pressed.
        error = code == ERROR_INVALID_PARAMETER ? L"The process has already ended."
                                                : WinErrorText(code);
        return false;
    }

    if (force) {
        bool ok = TerminateProcess(process, 1) != FALSE;
        DWORD code = ok ? 0 : GetLastError();
        CloseHandle(process);
        if (!ok) error = WinErrorText(code);
        return ok;
    }

    // A normal end asks the program to close: every top-level window it owns gets
    // WM_CLOSE, which is the same request the title bar's X makes.
    //
    // Console programs are deliberately not signalled: the only way to reach one is a
    // Ctrl+Break raised on the console it shares, and that console includes this process,
    // so the suite would be asking itself to close. Losing an unsaved document because a
    // button in this tool killed the tool is not a trade worth making, so a program with
    // no window is told about Force end instead.
    if (!HasWindow(pid)) {
        CloseHandle(process);
        error = L"This process has no window to ask, so nothing was sent. Use Force end "
                L"instead.";
        return false;
    }
    PostClose(pid);

    // Give it a few seconds to leave on its own terms.
    bool exited = WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
    CloseHandle(process);
    if (!exited)
        error = L"It was asked to close and is still running. Give it a moment, then use "
                L"Force end if it stays.";
    return exited;
}

bool SetTaskAffinity(DWORD pid, ULONG_PTR mask, std::wstring& error) {
    error.clear();
    if (!mask) {
        error = L"Pick at least one CPU.";
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!process) {
        error = WinErrorText(GetLastError());
        return false;
    }
    bool ok = SetProcessAffinityMask(process, (DWORD_PTR)mask) != FALSE;
    DWORD code = ok ? 0 : GetLastError();
    CloseHandle(process);
    if (!ok) error = WinErrorText(code);
    return ok;
}

bool SetTaskPriority(DWORD pid, DWORD priority, std::wstring& error) {
    error.clear();
    HANDLE process = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!process) {
        error = WinErrorText(GetLastError());
        return false;
    }
    bool ok = SetPriorityClass(process, priority) != FALSE;
    DWORD code = ok ? 0 : GetLastError();
    CloseHandle(process);
    if (!ok) error = WinErrorText(code);
    return ok;
}

bool LaunchNewTask(const std::wstring& commandLine, std::wstring& error) {
    error.clear();
    if (commandLine.empty()) {
        error = L"Type a program or command to start.";
        return false;
    }
    std::wstring mutableLine = commandLine;
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info = {};
    if (!CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &info)) {
        error = WinErrorText(GetLastError());
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

// ------------------------------------------------------------------- rules
std::wstring SuiteSettingsPath() {
    wchar_t base[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return L"";
    std::wstring folder = std::wstring(base) + L"\\ZanpharsWinSuite";
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder + L"\\settings.ini";
}

std::wstring SuiteAgentPath() {
    wchar_t exe[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH * 2)) return L"";
    std::wstring path = exe;
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return path.substr(0, slash + 1) + L"WinSuiteAgent.exe";
}

// The Run value the agent lives behind. One per user, so a rule set belongs to the
// account that saved it and no machine-wide change is ever needed.
static const wchar_t* kAgentRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kAgentRunValue = L"ZanpharsWinSuiteAgent";

bool TaskAgentRegistered() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAgentRunKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return false;
    LSTATUS status = RegQueryValueExW(key, kAgentRunValue, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool RegisterTaskAgent(bool present, std::wstring& error) {
    error.clear();
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kAgentRunKey, 0, nullptr, 0,
                                     KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        error = WinErrorText((DWORD)status);
        return false;
    }

    bool ok = false;
    if (!present) {
        status = RegDeleteValueW(key, kAgentRunValue);
        ok = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
        if (!ok) error = WinErrorText((DWORD)status);
    } else {
        std::wstring exe = SuiteAgentPath();
        if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            error = L"WinSuiteAgent.exe is missing next to the suite, so the rules would "
                    L"not come back at sign-in.";
        } else {
            std::wstring value = L"\"" + exe + L"\"";
            status = RegSetValueExW(key, kAgentRunValue, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(value.c_str()),
                                    (DWORD)((value.size() + 1) * sizeof(wchar_t)));
            ok = status == ERROR_SUCCESS;
            if (!ok) error = WinErrorText((DWORD)status);
        }
    }
    RegCloseKey(key);
    return ok;
}

// The helper takes a mutex for its whole life, so an open mutex is a running helper. That
// is cheaper and race-free compared with listing processes, and asking first means this
// function never starts a second copy just to have it exit again.
bool EnsureTaskAgentRunning() {
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ZanpharsWinSuiteAgent");
    if (running) {
        CloseHandle(running);
        return true;
    }

    std::wstring exe = SuiteAgentPath();
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    std::wstring command = L"\"" + exe + L"\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info = {};
    // No console and no window: the helper is a background poller and must outlive the
    // suite, so a rule keeps being enforced after the window is closed.
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &info))
        return false;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

std::vector<TaskRule> LoadTaskRules(const std::wstring& settingsPath) {
    std::vector<TaskRule> rules;
    if (settingsPath.empty()) return rules;
    for (int size = 4096; size <= 65536; size *= 2) {
        std::vector<wchar_t> buffer((size_t)size);
        DWORD read = GetPrivateProfileSectionW(L"TaskRules", buffer.data(), (DWORD)buffer.size(),
                                               settingsPath.c_str());
        if (read < (DWORD)buffer.size() - 2) {
            for (const wchar_t* line = buffer.data(); *line; line += wcslen(line) + 1) {
                std::wstring entry = line;
                size_t equals = entry.find(L'=');
                if (equals == std::wstring::npos) continue;
                std::wstring key = entry.substr(0, equals);
                std::wstring value = entry.substr(equals + 1);
                size_t bar = value.find(L'|');
                if (bar == std::wstring::npos) continue;
                TaskRule rule;
                rule.exe = Lowercase(key);
                rule.affinity = (ULONG_PTR)wcstoull(value.substr(0, bar).c_str(), nullptr, 0);
                rule.priority = (DWORD)wcstoul(value.substr(bar + 1).c_str(), nullptr, 0);
                if (!rule.exe.empty() && (rule.affinity || rule.priority))
                    rules.push_back(rule);
            }
            return rules;
        }
    }
    return rules;
}

bool SaveTaskRules(const std::wstring& settingsPath, const std::vector<TaskRule>& rules) {
    if (settingsPath.empty()) return false;
    WritePrivateProfileStringW(L"TaskRules", nullptr, nullptr, settingsPath.c_str());
    for (const auto& rule : rules) {
        wchar_t value[64];
        swprintf(value, 64, L"0x%llx|%lu", (unsigned long long)rule.affinity,
                 (unsigned long)rule.priority);
        WritePrivateProfileStringW(L"TaskRules", rule.exe.c_str(), value, settingsPath.c_str());
    }
    return true;
}

uint32_t ApplyTaskRules(const std::vector<TaskRule>& rules, uint32_t* considered) {
    uint32_t changed = 0, seen = 0;
    if (rules.empty()) {
        if (considered) *considered = 0;
        return 0;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (considered) *considered = 0;
        return 0;
    }
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::wstring name = Lowercase(entry.szExeFile);
            for (const auto& rule : rules) {
                if (rule.exe != name) continue;
                ++seen;
                HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION |
                                                 SYNCHRONIZE,
                                             FALSE, entry.th32ProcessID);
                if (!process) continue;
                bool touched = false;
                if (rule.affinity) {
                    DWORD_PTR current = 0, system = 0;
                    if (GetProcessAffinityMask(process, &current, &system) &&
                        (ULONG_PTR)current != rule.affinity &&
                        SetProcessAffinityMask(process, (DWORD_PTR)rule.affinity))
                        touched = true;
                }
                if (rule.priority) {
                    DWORD current = GetPriorityClass(process);
                    if (current != rule.priority &&
                        SetPriorityClass(process, rule.priority))
                        touched = true;
                }
                CloseHandle(process);
                if (touched) ++changed;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (considered) *considered = seen;
    return changed;
}
