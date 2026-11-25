// ============================================================================
//  harden.cpp — catalog, audit, journal and the service manager calls.
//
//  The rule the whole engine is built around: nothing is written without the
//  original value being saved first, under the same hive that is about to
//  change, so Revert works for the same administrator even from another
//  account. The only writes an audit makes are none - the self-test proves it.
// ============================================================================

#include "harden.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace hw {

namespace {

// The journal lives beside the settings the rest of the suite keeps, in the
// hive that owns the value being changed.
const wchar_t* kJournalPath = L"SOFTWARE\\ZanpharsWinSuite\\HardWin\\journal\\";

// Failure actions for the core services: restart after a minute, three times,
// then reset the counter a day later so a service that fails once a month is
// still restarted rather than given up on.
constexpr DWORD kRestartDelayMs = 60'000;
constexpr DWORD kRestartCount = 3;
constexpr DWORD kResetPeriodSeconds = 86'400;

const wchar_t* const kCoreServices[] = {L"EventLog", L"Winmgmt", L"WinDefend"};
constexpr size_t kCoreServiceCount = sizeof(kCoreServices) / sizeof(kCoreServices[0]);

// ---------------------------------------------------------------------------
//  Log
// ---------------------------------------------------------------------------

// Every write, refusal and rollback is appended to the log file under
// %LOCALAPPDATA%\HardWin — the same idea as the other programs' logs, and the
// only place
// a change made yesterday can be read back from. It sits under LOCALAPPDATA so
// a standard user can log the per-user checks without an elevation prompt.
bool g_quietLog = false;   // the self-test exercises the same paths as a user

void Log(const std::wstring& text) {
    if (g_quietLog) return;
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
    const std::wstring folder = std::wstring(base) + L"\\HardWin";
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::wstring path = folder + L"\\hardwin.log";

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t stamp[40] = {};
    _snwprintf(stamp, 39, L"%04d-%02d-%02d %02d:%02d:%02d  ", now.wYear, now.wMonth, now.wDay,
               now.wHour, now.wMinute, now.wSecond);

    const std::wstring line = std::wstring(stamp) + text + L"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(),
                        bytes, nullptr, nullptr);

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

std::wstring Fmt(const wchar_t* format, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf(buffer, 1023, format, args);
    va_end(args);
    buffer[1023] = 0;
    return buffer;
}

std::wstring WinError(LSTATUS status) {
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(status), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring out = n && text ? std::wstring(text, n) : Fmt(L"error %ld", status);
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' '))
        out.pop_back();
    return out;
}

const wchar_t* kDenied = L"Access is denied. Restart as administrator and try again.";

// ---------------------------------------------------------------------------
//  Registry primitives
// ---------------------------------------------------------------------------

struct RegValue {
    bool               exists = false;
    DWORD              type = 0;
    std::vector<BYTE>  data;
};

constexpr REGSAM kReadFlags = KEY_READ | KEY_WOW64_64KEY;
constexpr REGSAM kWriteFlags = KEY_SET_VALUE | KEY_WOW64_64KEY;

RegValue ReadValue(HKEY root, const wchar_t* key, const wchar_t* name) {
    RegValue value;
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key, 0, kReadFlags, &handle) != ERROR_SUCCESS) return value;
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(handle, name, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS) {
        value.data.resize(bytes);
        if (RegQueryValueExW(handle, name, nullptr, &type, value.data.data(), &bytes) ==
            ERROR_SUCCESS) {
            value.data.resize(bytes);
            value.exists = true;
            value.type = type;
        } else {
            value.data.clear();
        }
    }
    RegCloseKey(handle);
    return value;
}

bool KeyExists(HKEY root, const wchar_t* key) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key, 0, kReadFlags, &handle) != ERROR_SUCCESS) return false;
    RegCloseKey(handle);
    return true;
}

LSTATUS WriteValue(HKEY root, const wchar_t* key, const wchar_t* name, DWORD type,
                   const BYTE* data, DWORD bytes) {
    HKEY handle = nullptr;
    DWORD disposition = 0;
    LSTATUS status = RegCreateKeyExW(root, key, 0, nullptr, 0, kWriteFlags, nullptr, &handle,
                                     &disposition);
    if (status != ERROR_SUCCESS) return status;
    status = RegSetValueExW(handle, name, 0, type, data, bytes);
    RegCloseKey(handle);
    return status;
}

LSTATUS RemoveValue(HKEY root, const wchar_t* key, const wchar_t* name) {
    HKEY handle = nullptr;
    LSTATUS status = RegOpenKeyExW(root, key, 0, kWriteFlags, &handle);
    if (status != ERROR_SUCCESS) return status;
    status = RegDeleteValueW(handle, name);
    RegCloseKey(handle);
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

// ---------------------------------------------------------------------------
//  Journal
// ---------------------------------------------------------------------------

HKEY JournalHive(const Check& check) {
    return check.adminOnly ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

std::wstring JournalKey(const Check& check) {
    return std::wstring(kJournalPath) + check.id;
}

bool JournalSaveRegistry(const Check& check, std::wstring* error) {
    const HKEY hive = JournalHive(check);
    const std::wstring journal = JournalKey(check);
    const RegValue original = ReadValue(check.root, check.key, check.value);

    HKEY handle = nullptr;
    LSTATUS status = RegCreateKeyExW(hive, journal.c_str(), 0, nullptr, 0, kWriteFlags, nullptr,
                                     &handle, nullptr);
    if (status != ERROR_SUCCESS) {
        *error = WinError(status);
        return false;
    }
    const DWORD absent = original.exists ? 0 : 1;
    const DWORD keyExisted = KeyExists(check.root, check.key) ? 1 : 0;
    const DWORD type = original.type;
    status = RegSetValueExW(handle, L"Absent", 0, REG_DWORD,
                            reinterpret_cast<const BYTE*>(&absent), sizeof absent);
    if (status == ERROR_SUCCESS)
        status = RegSetValueExW(handle, L"KeyExisted", 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&keyExisted), sizeof keyExisted);
    if (status == ERROR_SUCCESS && original.exists)
        status = RegSetValueExW(handle, L"Type", 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&type), sizeof type);
    if (status == ERROR_SUCCESS && original.exists && !original.data.empty())
        status = RegSetValueExW(handle, L"Data", 0, REG_BINARY, original.data.data(),
                                static_cast<DWORD>(original.data.size()));
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS) {
        *error = WinError(status);
        return false;
    }
    return true;
}

bool JournalExists(const Check& check) {
    if (check.kind == Kind::Service) {
        const RegValue had = ReadValue(JournalHive(check), JournalKey(check).c_str(), L"Had");
        return had.exists;
    }
    const RegValue absent =
        ReadValue(JournalHive(check), JournalKey(check).c_str(), L"Absent");
    return absent.exists;
}

void JournalDrop(const Check& check) {
    RegDeleteKeyW(JournalHive(check), JournalKey(check).c_str());
}

// ---------------------------------------------------------------------------
//  Service failure actions
// ---------------------------------------------------------------------------

std::wstring ServiceStateText();

DWORD ReadDword(const RegValue& value, DWORD fallback) {
    if (!value.exists || value.type != REG_DWORD || value.data.size() < sizeof(DWORD))
        return fallback;
    DWORD out = 0;
    memcpy(&out, value.data.data(), sizeof out);
    return out;
}

bool QueryFailureActions(SC_HANDLE service, DWORD* resetSeconds,
                         std::vector<std::pair<DWORD, DWORD>>* actions, bool* configured) {
    std::vector<BYTE> buffer(8192);
    DWORD needed = 0;
    if (!QueryServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, buffer.data(),
                              static_cast<DWORD>(buffer.size()), &needed)) {
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            buffer.resize(needed);
            if (!QueryServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, buffer.data(),
                                      static_cast<DWORD>(buffer.size()), &needed))
                return false;
        } else {
            return false;
        }
    }
    const auto* failure = reinterpret_cast<const SERVICE_FAILURE_ACTIONSW*>(buffer.data());
    *resetSeconds = failure->dwResetPeriod;
    actions->clear();
    for (DWORD i = 0; i < failure->cActions && failure->lpsaActions; ++i)
        actions->emplace_back(static_cast<DWORD>(failure->lpsaActions[i].Type),
                              static_cast<DWORD>(failure->lpsaActions[i].Delay));
    *configured = !actions->empty();
    return true;
}

bool WriteFailureActions(SC_HANDLE service, DWORD resetSeconds,
                         const std::vector<std::pair<DWORD, DWORD>>& actions) {
    std::vector<SC_ACTION> native;
    native.reserve(actions.size());
    for (const auto& action : actions)
        native.push_back(SC_ACTION{static_cast<SC_ACTION_TYPE>(action.first), action.second});

    SERVICE_FAILURE_ACTIONSW failure = {};
    failure.dwResetPeriod = resetSeconds;
    failure.cActions = static_cast<DWORD>(native.size());
    failure.lpsaActions = native.empty() ? nullptr : native.data();
    return ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure) != FALSE;
}

struct ServiceRowState {
    int  configured = 0;   // services with at least one restart action
    int  reachable = 0;    // services this process could query
    bool needsAdmin = false;
};

ServiceRowState AuditServices() {
    ServiceRowState state;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        state.needsAdmin = GetLastError() == ERROR_ACCESS_DENIED;
        return state;
    }
    for (const wchar_t* name : kCoreServices) {
        SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_CONFIG);
        if (!service) continue;
        DWORD reset = 0;
        std::vector<std::pair<DWORD, DWORD>> actions;
        bool configured = false;
        if (QueryFailureActions(service, &reset, &actions, &configured)) {
            ++state.reachable;
            for (const auto& action : actions) {
                if (action.first == SC_ACTION_RESTART) {
                    ++state.configured;
                    break;
                }
            }
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return state;
}

std::wstring ServiceStateText() {
    const ServiceRowState state = AuditServices();
    if (state.needsAdmin) return L"needs administrator";
    return Fmt(L"%d of %d restart", state.configured, static_cast<int>(kCoreServiceCount));
}

// ---------------------------------------------------------------------------
//  Catalog
// ---------------------------------------------------------------------------

// HKEY_LOCAL_MACHINE is a cast integer in the headers, not a constant
// expression, so the shorthands are plain consts rather than constexpr.
const HKEY HKLM = HKEY_LOCAL_MACHINE;
const HKEY HKCU = HKEY_CURRENT_USER;

}  // namespace

// clang-format off
const std::vector<Check>& Checks() {
    static const std::vector<Check> catalog = {
        // ---------------------------------------------------------- security
        {L"smb1", Group::Security,
         L"SMBv1 file sharing",
         L"The 2017-era sharing protocol WannaCry spread through. Windows 11 never ships it.",
         Kind::Registry, HKLM, L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters",
         L"SMB1", REG_DWORD, 0, nullptr, true, false},

        {L"llmnr", Group::Security,
         L"LLMNR name lookup",
         L"Stops the multicast name resolution that spoofing tools answer before DNS does.",
         Kind::Registry, HKLM, L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\DNSClient",
         L"EnableMulticast", REG_DWORD, 0, nullptr, true, false},

        {L"autorun", Group::Security,
         L"AutoRun and AutoPlay",
         L"A USB stick or a mapped drive no longer offers to launch what is on it.",
         Kind::Registry, HKLM, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
         L"NoDriveTypeAutoRun", REG_DWORD, 0xFF, nullptr, true, false},

        {L"smartscreen", Group::Security,
         L"SmartScreen for apps",
         L"Warns before software Windows has never seen before is allowed to run.",
         Kind::Registry, HKLM, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer",
         L"SmartScreenEnabled", REG_SZ, 0, L"Warn", true, true},

        {L"pslogging", Group::Security,
         L"PowerShell script-block logging",
         L"Records every script block in the PowerShell operational log, so a run can be read back.",
         Kind::Registry, HKLM,
         L"SOFTWARE\\Policies\\Microsoft\\Windows\\PowerShell\\ScriptBlockLogging",
         L"EnableScriptBlockLogging", REG_DWORD, 1, nullptr, true, false},

        {L"pua", Group::Security,
         L"Defender blocks unwanted apps",
         L"Turns on the block level that stops most bundled adware and toolbars.",
         Kind::Registry, HKLM, L"SOFTWARE\\Policies\\Microsoft\\Windows Defender",
         L"PUAProtection", REG_DWORD, 1, nullptr, true, false},

        // ------------------------------------------------------- performance
        {L"visualfx", Group::Performance,
         L"Visual effects: best performance",
         L"Drops the animation, shadow and fade work the desktop does for nothing.",
         Kind::Registry, HKCU,
         L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects",
         L"VisualFXSetting", REG_DWORD, 2, nullptr, false, false},

        {L"startupdelay", Group::Performance,
         L"Startup program delay",
         L"Explorer stops holding your sign-in programs back for ten seconds.",
         Kind::Registry, HKCU,
         L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize",
         L"StartupDelayInMSec", REG_DWORD, 0, nullptr, false, false},

        {L"menudelay", Group::Performance,
         L"Menu show delay",
         L"Menus open in 100 ms instead of 400 ms; the setting Windows already has, just faster.",
         Kind::Registry, HKCU, L"Control Panel\\Desktop",
         L"MenuShowDelay", REG_SZ, 0, L"100", false, false},

        // ------------------------------------------------------- reliability
        {L"svcrecovery", Group::Reliability,
         L"Core services restart themselves",
         L"Event Log, WMI and Defender restart a minute after a failure instead of staying down.",
         Kind::Service, nullptr, nullptr, nullptr, 0, 0, nullptr, true, false},

        {L"maintenance", Group::Reliability,
         L"Automatic maintenance",
         L"Keeps the idle upkeep Windows does - defrag, updates, health checks - switched on.",
         Kind::Registry, HKLM,
         L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\Maintenance",
         L"MaintenanceDisabled", REG_DWORD, 0, nullptr, true, true},

        {L"reboot", Group::Reliability,
         L"No forced restart with programs open",
         L"Windows waits for you to save your work instead of restarting under it.",
         Kind::Registry, HKLM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU",
         L"NoAutoRebootWithLoggedOnUsers", REG_DWORD, 1, nullptr, true, false},

        {L"faststartup", Group::Reliability,
         L"Fast startup off",
         L"A hybrid shutdown can carry stale driver state across a power cycle; a full boot is slower but predictable.",
         Kind::Registry, HKLM,
         L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
         L"HiberbootEnabled", REG_DWORD, 0, nullptr, true, false},

        {L"crashdump", Group::Reliability,
         L"Automatic crash dump",
         L"Keeps the automatic memory dump Windows writes when it stops, so a second failure can be diagnosed.",
         Kind::Registry, HKLM, L"SYSTEM\\CurrentControlSet\\Control\\CrashControl",
         L"CrashDumpEnabled", REG_DWORD, 7, nullptr, true, false},
    };
    return catalog;
}
// clang-format on

const wchar_t* GroupName(Group group) {
    switch (group) {
        case Group::Security: return L"SECURITY";
        case Group::Performance: return L"PERFORMANCE";
        case Group::Reliability: return L"RELIABILITY";
    }
    return L"";
}

bool IsElevated() {
    BOOL admin = FALSE;
    PSID group = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &admin);
        FreeSid(group);
    }
    return admin != FALSE;
}

// ---------------------------------------------------------------------------
//  Audit
// ---------------------------------------------------------------------------

namespace {

bool MatchesDesired(const Check& check, const RegValue& value) {
    if (!value.exists) return false;
    if (check.type == REG_DWORD) {
        if (value.type != REG_DWORD || value.data.size() < sizeof(DWORD)) return false;
        DWORD current = 0;
        memcpy(&current, value.data.data(), sizeof current);
        return current == check.dword;
    }
    if (value.type != REG_SZ || !check.text) return false;
    std::wstring text(reinterpret_cast<const wchar_t*>(value.data.data()),
                      value.data.size() / sizeof(wchar_t));
    while (!text.empty() && text.back() == 0) text.pop_back();
    return _wcsicmp(text.c_str(), check.text) == 0;
}

std::wstring Describe(const Check& check, const RegValue& value) {
    (void)check;
    if (!value.exists) return L"not set";
    if (value.type == REG_DWORD && value.data.size() >= sizeof(DWORD)) {
        DWORD current = 0;
        memcpy(&current, value.data.data(), sizeof current);
        return Fmt(L"%lu", current);
    }
    if (value.type == REG_SZ) {
        std::wstring text(reinterpret_cast<const wchar_t*>(value.data.data()),
                          value.data.size() / sizeof(wchar_t));
        while (!text.empty() && text.back() == 0) text.pop_back();
        return text;
    }
    return L"set";
}

}  // namespace

State Engine::Audit(const Check& check) const {
    State state;
    if (check.kind == Kind::Service) {
        const ServiceRowState services = AuditServices();
        state.hardened = services.configured == static_cast<int>(kCoreServiceCount);
        // A process that could not reach the service manager at all has no
        // reading, not an open one; say so rather than guess.
        state.needsAdmin =
            (services.needsAdmin || services.reachable == 0) && !state.hardened;
        state.current = ServiceStateText();
        state.journalled = JournalExists(check);
        return state;
    }

    const RegValue value = ReadValue(check.root, check.key, check.value);
    state.hardened =
        MatchesDesired(check, value) || (!value.exists && check.absentIsHardened);
    state.needsAdmin = check.adminOnly && !state.hardened && !IsElevated();
    state.current = Describe(check, value);
    if (!value.exists) state.current += L" (Windows default)";
    state.journalled = JournalExists(check);
    return state;
}

std::vector<State> Engine::AuditAll() const {
    std::vector<State> states;
    states.reserve(Checks().size());
    for (const Check& check : Checks()) states.push_back(Audit(check));
    return states;
}

// ---------------------------------------------------------------------------
//  Apply / revert
// ---------------------------------------------------------------------------

bool Engine::Apply(const Check& check, std::wstring* error) {
    error->clear();

    if (check.kind == Kind::Service) {
        if (!IsElevated()) {
            *error = kDenied;
            return false;
        }
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager) {
            *error = WinError(GetLastError());
            return false;
        }
        const std::wstring journal = JournalKey(check);
        HKEY handle = nullptr;
        LSTATUS status = RegCreateKeyExW(JournalHive(check), journal.c_str(), 0, nullptr, 0,
                                         kWriteFlags, nullptr, &handle, nullptr);
        if (status != ERROR_SUCCESS) {
            CloseServiceHandle(manager);
            *error = WinError(status);
            return false;
        }
        const DWORD had = 1;
        RegSetValueExW(handle, L"Had", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&had),
                       sizeof had);

        std::wstring refused;
        for (const wchar_t* name : kCoreServices) {
            SC_HANDLE service =
                OpenServiceW(manager, name, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG);
            if (!service) {
                refused += Fmt(L" %ls: %ls", name, WinError(GetLastError()).c_str());
                continue;
            }
            DWORD reset = 0;
            std::vector<std::pair<DWORD, DWORD>> actions;
            bool configured = false;
            if (QueryFailureActions(service, &reset, &actions, &configured)) {
                const std::vector<BYTE> blob = detail::EncodeFailureActions(reset, actions);
                const DWORD count = configured ? 1 : 0;
                const std::wstring valueName = std::wstring(L"Blob.") + name;
                const std::wstring flagName = std::wstring(L"Had.") + name;
                RegSetValueExW(handle, flagName.c_str(), 0, REG_DWORD,
                               reinterpret_cast<const BYTE*>(&count), sizeof count);
                if (!blob.empty())
                    RegSetValueExW(handle, valueName.c_str(), 0, REG_BINARY, blob.data(),
                                   static_cast<DWORD>(blob.size()));
            }
            std::vector<std::pair<DWORD, DWORD>> restart;
            for (DWORD i = 0; i < kRestartCount; ++i)
                restart.emplace_back(SC_ACTION_RESTART, kRestartDelayMs);
            if (!WriteFailureActions(service, kResetPeriodSeconds, restart)) {
                refused += Fmt(L" %ls: %ls", name, WinError(GetLastError()).c_str());
                Log(Fmt(L"failed    %ls  recovery: %ls", name, WinError(GetLastError()).c_str()));
            } else {
                ++writes_;
                // Confirm the manager kept what it was handed.
                DWORD nowReset = 0;
                std::vector<std::pair<DWORD, DWORD>> now;
                bool nowConfigured = false;
                const bool took = QueryFailureActions(service, &nowReset, &now, &nowConfigured) &&
                                  nowConfigured && now.front().first == SC_ACTION_RESTART;
                if (!took) {
                    refused += Fmt(L" %ls: the recovery actions did not take.", name);
                    Log(Fmt(L"unverified %ls  recovery actions did not take", name));
                } else {
                    Log(Fmt(L"applied   svcrecovery  %ls restarts after failure", name));
                }
            }
            CloseServiceHandle(service);
        }
        RegCloseKey(handle);
        CloseServiceHandle(manager);
        if (!refused.empty()) {
            *error = L"Some services refused:" + refused;
            return false;
        }
        return true;
    }

    if (check.adminOnly && !IsElevated()) {
        *error = kDenied;
        return false;
    }
    if (!JournalSaveRegistry(check, error)) return false;

    std::vector<BYTE> payload;
    if (check.type == REG_DWORD) {
        payload.resize(sizeof(DWORD));
        memcpy(payload.data(), &check.dword, sizeof(DWORD));
    } else {
        const size_t bytes = (wcslen(check.text) + 1) * sizeof(wchar_t);
        payload.resize(bytes);
        memcpy(payload.data(), check.text, bytes);
    }
    const LSTATUS status =
        WriteValue(check.root, check.key, check.value, check.type, payload.data(),
                   static_cast<DWORD>(payload.size()));
    if (status != ERROR_SUCCESS) {
        *error = WinError(status);
        Log(L"failed    " + std::wstring(check.id) + L"  " + *error);
        return false;
    }
    ++writes_;

    // Read it back. A group policy or another tool can leave the write reported
    // as success while the effective value is unchanged; a row that did not
    // take is put back rather than counted as hardened.
    const RegValue written = ReadValue(check.root, check.key, check.value);
    if (!MatchesDesired(check, written)) {
        std::wstring rollbackError;
        const bool restored = Revert(check, &rollbackError);
        *error = restored ? L"the value did not take, so the original was put back."
                          : L"the value did not take and the original could not be put back: " +
                                rollbackError;
        Log(L"unverified " + std::wstring(check.id) + L"  " + *error);
        return false;
    }
    Log(L"applied   " + std::wstring(check.id) + L"  " + std::wstring(check.name) + L" = " +
        Describe(check, written));
    return true;
}

bool Engine::Revert(const Check& check, std::wstring* error) {
    error->clear();
    if (!JournalExists(check)) {
        *error = L"Nothing is saved for this check, so there is nothing to put back.";
        return false;
    }
    if (check.adminOnly && !IsElevated()) {
        *error = kDenied;
        return false;
    }

    if (check.kind == Kind::Service) {
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager) {
            *error = WinError(GetLastError());
            return false;
        }
        std::wstring refused;
        for (const wchar_t* name : kCoreServices) {
            const std::wstring valueName = std::wstring(L"Blob.") + name;
            const std::wstring flagName = std::wstring(L"Had.") + name;
            const RegValue blob =
                ReadValue(JournalHive(check), JournalKey(check).c_str(), valueName.c_str());
            const RegValue had =
                ReadValue(JournalHive(check), JournalKey(check).c_str(), flagName.c_str());
            if (!had.exists) continue;

            SC_HANDLE service = OpenServiceW(manager, name, SERVICE_CHANGE_CONFIG);
            if (!service) {
                refused += Fmt(L" %ls: %ls", name, WinError(GetLastError()).c_str());
                continue;
            }
            DWORD reset = 0;
            std::vector<std::pair<DWORD, DWORD>> actions;
            bool decoded = false;
            if (ReadDword(had, 0) != 0)
                decoded = detail::DecodeFailureActions(blob.data, &reset, &actions);
            if (ReadDword(had, 0) == 0) {
                actions.clear();
                reset = 0;
            }
            if (!decoded && ReadDword(had, 0) != 0) {
                refused += Fmt(L" %ls: the saved actions could not be read.", name);
                Log(Fmt(L"failed    %ls  recovery: the saved actions could not be read", name));
            } else if (!WriteFailureActions(service, reset, actions)) {
                refused += Fmt(L" %ls: %ls", name, WinError(GetLastError()).c_str());
                Log(Fmt(L"failed    %ls  recovery revert: %ls", name, WinError(GetLastError()).c_str()));
            } else {
                ++writes_;
                Log(Fmt(L"reverted  svcrecovery  %ls", name));
            }
            CloseServiceHandle(service);
        }
        CloseServiceHandle(manager);
        if (!refused.empty()) {
            *error = L"Some services refused:" + refused;
            return false;
        }
        JournalDrop(check);
        return true;
    }

    const HKEY hive = JournalHive(check);
    const std::wstring journal = JournalKey(check);
    const RegValue absent = ReadValue(hive, journal.c_str(), L"Absent");
    const RegValue keyExisted = ReadValue(hive, journal.c_str(), L"KeyExisted");

    LSTATUS status = ERROR_SUCCESS;
    if (ReadDword(absent, 1) != 0) {
        status = RemoveValue(check.root, check.key, check.value);
    } else {
        const RegValue type = ReadValue(hive, journal.c_str(), L"Type");
        const RegValue data = ReadValue(hive, journal.c_str(), L"Data");
        if (!type.exists || !data.exists) {
            *error = L"The saved value is incomplete, so it was left alone.";
            return false;
        }
        status = WriteValue(check.root, check.key, check.value, ReadDword(type, REG_SZ),
                            data.data.data(), static_cast<DWORD>(data.data.size()));
    }
    if (status != ERROR_SUCCESS) {
        *error = WinError(status);
        Log(L"failed    " + std::wstring(check.id) + L"  revert: " + *error);
        return false;
    }
    ++writes_;

    // A key this tool had to create goes away again once its last value is
    // gone; Windows refuses the delete while anything else lives there.
    if (ReadDword(keyExisted, 1) == 0) RegDeleteKeyW(check.root, check.key);
    JournalDrop(check);
    Log(L"reverted  " + std::wstring(check.id) + L"  " + std::wstring(check.name));
    return true;
}

// The only batch loops in the program. The window hands over the rows the user
// ticked, so there is no second copy of the skip rules to drift from these.
Engine::Batch Engine::ApplyChecks(const std::vector<const Check*>& checks) {
    Batch batch;
    for (const Check* check : checks) {
        if (!check) continue;
        if (Audit(*check).hardened) {
            ++batch.skipped;
            continue;
        }
        std::wstring error;
        if (Apply(*check, &error))
            ++batch.changed;
        else
            batch.refused += Fmt(L" %ls - %ls.", check->name, error.c_str());
    }
    return batch;
}

Engine::Batch Engine::RevertChecks(const std::vector<const Check*>& checks) {
    Batch batch;
    for (const Check* check : checks) {
        if (!check) continue;
        // No saved original means there is nothing of this tool's to put back;
        // that is a skip, not a failure.
        if (!JournalExists(*check)) {
            ++batch.skipped;
            continue;
        }
        std::wstring error;
        if (Revert(*check, &error))
            ++batch.changed;
        else
            batch.refused += Fmt(L" %ls - %ls.", check->name, error.c_str());
    }
    return batch;
}

// ---------------------------------------------------------------------------
//  Journal encoding for failure actions
// ---------------------------------------------------------------------------

namespace detail {

std::vector<BYTE> EncodeFailureActions(
    DWORD resetSeconds, const std::vector<std::pair<DWORD, DWORD>>& actions) {
    std::vector<BYTE> blob;
    blob.resize(sizeof(DWORD) * 2);
    memcpy(blob.data(), &resetSeconds, sizeof resetSeconds);
    const DWORD count = static_cast<DWORD>(actions.size());
    memcpy(blob.data() + sizeof(DWORD), &count, sizeof count);
    for (const auto& action : actions) {
        const DWORD pair[2] = {action.first, action.second};
        blob.insert(blob.end(), reinterpret_cast<const BYTE*>(pair),
                    reinterpret_cast<const BYTE*>(pair) + sizeof pair);
    }
    return blob;
}

bool DecodeFailureActions(const std::vector<BYTE>& blob, DWORD* resetSeconds,
                          std::vector<std::pair<DWORD, DWORD>>* actions) {
    if (blob.size() < sizeof(DWORD) * 2) return false;
    DWORD reset = 0;
    DWORD count = 0;
    memcpy(&reset, blob.data(), sizeof reset);
    memcpy(&count, blob.data() + sizeof(DWORD), sizeof count);
    if (count > 64) return false;
    if (blob.size() != sizeof(DWORD) * 2 + count * sizeof(DWORD) * 2) return false;
    actions->clear();
    for (DWORD i = 0; i < count; ++i) {
        DWORD pair[2] = {0, 0};
        memcpy(pair, blob.data() + sizeof(DWORD) * 2 + i * sizeof(DWORD) * 2, sizeof pair);
        actions->emplace_back(pair[0], pair[1]);
    }
    *resetSeconds = reset;
    return true;
}

}  // namespace detail

// ---------------------------------------------------------------------------
//  Self-test
// ---------------------------------------------------------------------------

namespace {

// A scratch check the journal can be round-tripped through without touching a
// real Windows setting. It points at a throwaway key of the tool's own, which
// the self-test deletes again before it returns.
const Check& ProbeCheck() {
    static const Check probe = {L"selftest.probe", Group::Security, L"probe", L"scratch",
                                Kind::Registry, HKEY_CURRENT_USER,
                                L"SOFTWARE\\ZanpharsWinSuite\\HardWinTest", L"probe",
                                REG_DWORD, 0, nullptr, false, false};
    return probe;
}

TestResult Pass(const wchar_t* name) { return TestResult{name, Outcome::Pass, L""}; }
TestResult Fail(const wchar_t* name, const std::wstring& detail) {
    return TestResult{name, Outcome::Fail, detail};
}

// Writes the probe value, journals it, overwrites it and reverts: the value has
// to come back with the data it had, not with the data the tool wrote.
TestResult JournalRoundTrip() {
    const Check& probe = ProbeCheck();
    const wchar_t* name = L"a saved value is put back byte for byte";
    const DWORD original = 7;
    LSTATUS status = WriteValue(HKEY_CURRENT_USER, probe.key, probe.value, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&original), sizeof original);
    if (status != ERROR_SUCCESS)
        return TestResult{name, Outcome::Skip, L"could not write the scratch key"};

    Engine engine;
    std::wstring error;
    if (!JournalSaveRegistry(probe, &error))
        return Fail(name, L"journal save failed: " + error);

    const DWORD written = 42;
    WriteValue(HKEY_CURRENT_USER, probe.key, probe.value, REG_DWORD,
               reinterpret_cast<const BYTE*>(&written), sizeof written);
    if (!engine.Revert(probe, &error)) return Fail(name, L"revert failed: " + error);

    const RegValue back = ReadValue(HKEY_CURRENT_USER, probe.key, probe.value);
    const DWORD restored = ReadDword(back, 0xFFFFFFFF);
    if (!back.exists || restored != original)
        return Fail(name, Fmt(L"expected %lu, found %lu", original, restored));
    return Pass(name);
}

// A value that did not exist when the check was applied has to be gone again,
// not merely reset to some default this tool guessed at.
TestResult AbsentValueComesBack() {
    const Check& probe = ProbeCheck();
    const wchar_t* name = L"an absent value is removed again on revert";
    RemoveValue(HKEY_CURRENT_USER, probe.key, probe.value);

    Engine engine;
    std::wstring error;
    if (!JournalSaveRegistry(probe, &error)) return Fail(name, L"journal save failed: " + error);
    const DWORD written = 42;
    WriteValue(HKEY_CURRENT_USER, probe.key, probe.value, REG_DWORD,
               reinterpret_cast<const BYTE*>(&written), sizeof written);
    if (!engine.Revert(probe, &error)) return Fail(name, L"revert failed: " + error);

    if (ReadValue(HKEY_CURRENT_USER, probe.key, probe.value).exists)
        return Fail(name, L"the value is still there");
    return Pass(name);
}

// A write is only called success once the value reads back the way it was
// meant to; this is the path a policy-protected machine would take to a
// failure, exercised here on the scratch key.
TestResult ApplyIsVerified() {
    const wchar_t* name = L"an applied value is read back and confirmed";
    const Check& probe = ProbeCheck();
    RemoveValue(HKEY_CURRENT_USER, probe.key, probe.value);

    Engine engine;
    std::wstring error;
    if (!engine.Apply(probe, &error)) return Fail(name, L"apply failed: " + error);

    const RegValue written = ReadValue(HKEY_CURRENT_USER, probe.key, probe.value);
    if (!written.exists || ReadDword(written, 0xFFFFFFFFu) != 0)
        return Fail(name, L"the value did not read back as written");

    if (!engine.Revert(probe, &error)) return Fail(name, L"revert failed: " + error);
    if (ReadValue(HKEY_CURRENT_USER, probe.key, probe.value).exists)
        return Fail(name, L"the probe value is still there after reverting");
    return Pass(name);
}

// The batch skip rule is the engine's, and this proves it: a row already in the
// wanted state costs a skip and no write, and the same call still reverts it.
TestResult BatchSkipsSettledRows() {
    const wchar_t* name = L"a batch does not rewrite a row that is already hardened";
    const Check& probe = ProbeCheck();
    RemoveValue(HKEY_CURRENT_USER, probe.key, probe.value);

    Engine engine;
    std::wstring error;
    if (!engine.Apply(probe, &error)) return Fail(name, L"apply failed: " + error);

    const int writes = engine.Writes();
    const Engine::Batch apply = engine.ApplyChecks({&probe});
    if (apply.changed != 0) return Fail(name, L"the batch wrote a settled row");
    if (apply.skipped != 1) return Fail(name, Fmt(L"expected one skip, got %d", apply.skipped));
    if (!apply.refused.empty()) return Fail(name, L"the batch refused a settled row");
    if (engine.Writes() != writes) return Fail(name, L"the skipped row still counted a write");

    const Engine::Batch revert = engine.RevertChecks({&probe});
    if (revert.changed != 1) return Fail(name, L"the batch did not revert the probe");
    if (!revert.refused.empty()) return Fail(name, L"the revert refused: " + revert.refused);
    return Pass(name);
}

}  // namespace

std::vector<TestResult> RunSelfTest() {
    g_quietLog = true;
    std::vector<TestResult> results;
    const auto& catalog = Checks();

    // ------------------------------------------------------------- catalog
    {
        int security = 0, performance = 0, reliability = 0;
        for (const Check& check : catalog) {
            if (check.group == Group::Security) ++security;
            else if (check.group == Group::Performance) ++performance;
            else ++reliability;
        }
        if (catalog.size() == 14 && security == 6 && performance == 3 && reliability == 5)
            results.push_back(Pass(L"the catalog lists fourteen checks across three groups"));
        else
            results.push_back(Fail(L"the catalog lists fourteen checks across three groups",
                                   Fmt(L"%d checks: %d security, %d performance, %d reliability",
                                       static_cast<int>(catalog.size()), security, performance,
                                       reliability)));
    }
    {
        bool unique = true;
        std::wstring duplicate;
        for (size_t i = 0; i < catalog.size() && unique; ++i)
            for (size_t j = i + 1; j < catalog.size(); ++j)
                if (wcscmp(catalog[i].id, catalog[j].id) == 0) {
                    unique = false;
                    duplicate = catalog[i].id;
                    break;
                }
        results.push_back(unique ? Pass(L"every check id is unique")
                                 : Fail(L"every check id is unique",
                                        L"duplicated id: " + duplicate));
    }
    {
        // The four settings this tool is known for, pinned so a careless edit
        // of the catalog cannot quietly harden less than it says.
        auto find = [&](const wchar_t* id) -> const Check* {
            for (const Check& check : catalog)
                if (wcscmp(check.id, id) == 0) return &check;
            return nullptr;
        };
        struct Expect {
            const wchar_t* id;
            DWORD          dword;
            bool           isDword;
        };
        const Expect expected[] = {{L"smb1", 0, true},         {L"llmnr", 0, true},
                                   {L"autorun", 0xFF, true},   {L"pua", 1, true},
                                   {L"visualfx", 2, true},     {L"startupdelay", 0, true},
                                   {L"reboot", 1, true},       {L"menudelay", 0, false},
                                   {L"faststartup", 0, true},  {L"crashdump", 7, true}};
        std::wstring problem;
        for (const Expect& want : expected) {
            const Check* check = find(want.id);
            if (!check) {
                problem = Fmt(L"%ls is missing from the catalog", want.id);
                break;
            }
            if (want.isDword && (check->type != REG_DWORD || check->dword != want.dword)) {
                problem = Fmt(L"%ls does not hold the documented value", want.id);
                break;
            }
            if (!want.isDword && (check->type != REG_SZ || !check->text ||
                                  wcscmp(check->text, L"100") != 0)) {
                problem = Fmt(L"%ls does not hold the documented value", want.id);
                break;
            }
        }
        results.push_back(problem.empty()
                              ? Pass(L"the known checks hold the values they document")
                              : Fail(L"the known checks hold the values they document", problem));
    }
    {
        bool service = false;
        for (const Check& check : catalog)
            if (check.kind == Kind::Service) service = true;
        results.push_back(service ? Pass(L"the reliability group carries the service check")
                                  : Fail(L"the reliability group carries the service check",
                                         L"no service row in the catalog"));
    }

    // --------------------------------------------------------------- audit
    {
        Engine engine;
        const std::vector<State> states = engine.AuditAll();
        if (engine.Writes() != 0)
            results.push_back(Fail(L"an audit reads every check and writes nothing",
                                   L"the audit wrote to the system"));
        else if (states.size() != catalog.size())
            results.push_back(Fail(L"an audit reads every check and writes nothing",
                                   Fmt(L"%d states for %d checks",
                                       static_cast<int>(states.size()),
                                       static_cast<int>(catalog.size()))));
        else
            results.push_back(Pass(L"an audit reads every check and writes nothing"));
    }

    // ------------------------------------------------------------- journal
    results.push_back(JournalRoundTrip());
    results.push_back(AbsentValueComesBack());
    results.push_back(ApplyIsVerified());
    results.push_back(BatchSkipsSettledRows());

    // ------------------------------------------------------- service blob
    {
        const wchar_t* name = L"failure actions survive the journal encoding";
        const std::vector<std::pair<DWORD, DWORD>> actions = {
            {SC_ACTION_RESTART, 60000}, {SC_ACTION_RESTART, 60000}, {SC_ACTION_RESTART, 60000}};
        const std::vector<BYTE> blob = detail::EncodeFailureActions(86400, actions);
        DWORD reset = 0;
        std::vector<std::pair<DWORD, DWORD>> back;
        if (!detail::DecodeFailureActions(blob, &reset, &back))
            results.push_back(Fail(name, L"the encoding did not decode"));
        else if (reset != 86400 || back != actions)
            results.push_back(Fail(name, L"the decoded actions differ from the encoded ones"));
        else
            results.push_back(Pass(name));
    }
    {
        const wchar_t* name = L"a truncated action blob is refused, not guessed at";
        std::vector<BYTE> blob = detail::EncodeFailureActions(86400, {{SC_ACTION_RESTART, 1}});
        blob.resize(blob.size() - 3);
        DWORD reset = 0;
        std::vector<std::pair<DWORD, DWORD>> actions;
        results.push_back(detail::DecodeFailureActions(blob, &reset, &actions)
                              ? Fail(name, L"a short blob was accepted")
                              : Pass(name));
    }

    // Nothing the probe wrote outlives the run: the scratch value, its key and
    // the journal entry made for it all go.
    const Check& probe = ProbeCheck();
    RegDeleteKeyW(HKEY_CURRENT_USER, probe.key);
    RegDeleteKeyW(HKEY_CURRENT_USER, JournalKey(probe).c_str());
    g_quietLog = false;

    return results;
}

}  // namespace hw
