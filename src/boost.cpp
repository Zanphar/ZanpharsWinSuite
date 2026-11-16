// boost.cpp - the System Turbo engine: a reversible boost made of three tweaks,
// each one journalled before it is changed so Restore can put it back exactly.
#include "extras.h"

#include <powrprof.h>

#include <string>
#include <vector>

#include "core.h"

const wchar_t* kHighPerformancePlan = L"{8C5E7FDA-E8BF-4A96-9A85-A6E23A8C635C}";

namespace {

const wchar_t* kJournalKey = L"Software\\ZanpharsWinSuite\\Turbo";
const wchar_t* kGameDvrKey = L"System\\GameConfigStore";
const wchar_t* kAppCaptureKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR";

// Services the boost parks. Each one is stopped and set to manual, and every
// original value is written to the journal first.
const wchar_t* kParkedServices[] = {
    L"WSearch",       // Windows Search
    L"SysMain",       // SysMain (Superfetch)
    L"Spooler",       // Print Spooler
    L"BTAGService",   // Bluetooth Support
    L"DPS",           // Diagnostic Policy
    L"Fax",           // Fax
    L"WMPNetworkSvc", // WMP Network Sharing
};

std::wstring GuidToString(const GUID& guid) {
    wchar_t buf[64] = {};
    StringFromGUID2(guid, buf, 64);
    return buf;
}

bool ParseGuid(const std::wstring& text, GUID& guid) {
    if (text.empty()) return false;
    return SUCCEEDED(CLSIDFromString(text.c_str(), &guid));
}

std::wstring ActivePlanGuid() {
    GUID* active = nullptr;
    if (FAILED(PowerGetActiveScheme(nullptr, &active)) || !active) return L"";
    std::wstring text = GuidToString(*active);
    LocalFree(active);
    return text;
}

std::wstring PlanName(const std::wstring& guidText) {
    GUID guid;
    if (!ParseGuid(guidText, guid)) return L"";
    DWORD size = 0;
    if (PowerReadFriendlyName(nullptr, &guid, nullptr, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size == 0)
        return L"";
    std::vector<BYTE> buffer(size);
    if (PowerReadFriendlyName(nullptr, &guid, nullptr, nullptr, buffer.data(), &size) !=
        ERROR_SUCCESS)
        return L"";
    return std::wstring(reinterpret_cast<wchar_t*>(buffer.data()));
}

void JournalWrite(const std::wstring& name, const std::wstring& data) {
    RegValue value;
    value.type = REG_SZ;
    value.data = data;
    std::wstring ignored;
    WriteRegValue(HKEY_CURRENT_USER, kJournalKey, name, 0, value, ignored);
}

std::wstring JournalRead(const std::wstring& name) {
    RegValue value;
    std::wstring error;
    if (!ReadRegValue(HKEY_CURRENT_USER, kJournalKey, name, 0, value, error)) return L"";
    return value.data;
}

void JournalDelete(const std::wstring& name) {
    std::wstring ignored;
    DeleteRegValue(HKEY_CURRENT_USER, kJournalKey, name, 0, ignored);
}

// Game DVR is stored as REG_DWORD, which the suite's string-based registry
// helpers in core.cpp deliberately do not handle, so it gets its own path.
bool ReadDword(HKEY root, const std::wstring& key, const std::wstring& name, DWORD& out) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &handle) !=
        ERROR_SUCCESS)
        return false;
    DWORD type = 0;
    DWORD data = 0;
    DWORD size = sizeof(data);
    LONG rc = RegQueryValueExW(handle, name.c_str(), nullptr, &type,
                               reinterpret_cast<LPBYTE>(&data), &size);
    RegCloseKey(handle);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return false;
    out = data;
    return true;
}

bool WriteDword(HKEY root, const std::wstring& key, const std::wstring& name, DWORD value) {
    HKEY handle = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
                        nullptr, &handle, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExW(handle, name.c_str(), 0, REG_DWORD,
                             reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(handle);
    return rc == ERROR_SUCCESS;
}

DWORD JournalReadDword(const std::wstring& name, DWORD fallback) {
    std::wstring text = JournalRead(name);
    if (text.empty()) return fallback;
    return (DWORD)wcstoul(text.c_str(), nullptr, 10);
}

struct ServiceSnapshot {
    DWORD start = SERVICE_DEMAND_START;
    bool running = false;
    bool delayed = false;  // Automatic (Delayed Start), the default for WSearch and SysMain
    bool valid = false;
};

// A parked service has to come back exactly as it was, so the record carries the start
// type, whether it was running, and the delayed-autostart flag. Records written before
// the flag existed only have two fields, which decode as "the flag was not recorded".
struct ServiceRecord {
    DWORD start = SERVICE_DEMAND_START;
    bool running = false;
    int delayed = -1;  // -1: not recorded
    bool valid = false;
};

std::wstring EncodeServiceRecord(const ServiceRecord& record) {
    wchar_t text[96] = {};
    swprintf(text, 96, L"%lu|%d|%d", (unsigned long)record.start, record.running ? 1 : 0,
             record.delayed < 0 ? 0 : record.delayed);
    return text;
}

ServiceRecord DecodeServiceRecord(const std::wstring& text) {
    ServiceRecord record;
    size_t first = text.find(L'|');
    if (first == std::wstring::npos) return record;
    record.start = (DWORD)wcstoul(text.c_str(), nullptr, 10);
    std::wstring rest = text.substr(first + 1);
    size_t second = rest.find(L'|');
    record.running = (second == std::wstring::npos ? rest : rest.substr(0, second)) == L"1";
    if (second != std::wstring::npos) record.delayed = rest.substr(second + 1) == L"1" ? 1 : 0;
    record.valid = true;
    return record;
}

ServiceSnapshot QueryService(SC_HANDLE service) {
    ServiceSnapshot snapshot;
    DWORD needed = 0;
    if (QueryServiceConfigW(service, nullptr, 0, &needed) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return snapshot;
    std::vector<BYTE> buffer(needed);
    auto* config = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
    if (!QueryServiceConfigW(service, config, needed, &needed)) return snapshot;
    snapshot.start = config->dwStartType;

    SERVICE_DELAYED_AUTO_START_INFO delayed = {};
    if (QueryServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                             reinterpret_cast<LPBYTE>(&delayed), sizeof(delayed), &needed))
        snapshot.delayed = delayed.fDelayedAutostart != FALSE;
    SERVICE_STATUS_PROCESS status = {};
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status),
                             sizeof(status), &needed))
        snapshot.running = status.dwCurrentState == SERVICE_RUNNING ||
                           status.dwCurrentState == SERVICE_START_PENDING;
    snapshot.valid = true;
    return snapshot;
}

}  // namespace

std::wstring CurrentPowerPlanName() {
    return PlanName(ActivePlanGuid());
}

bool BoostServiceJournalRoundTrip(std::wstring& detail) {
    ServiceRecord parked;
    parked.start = SERVICE_AUTO_START;
    parked.running = true;
    parked.delayed = 1;
    ServiceRecord back = DecodeServiceRecord(EncodeServiceRecord(parked));
    ServiceRecord legacy = DecodeServiceRecord(L"3|0");
    const bool ok = back.valid && back.start == SERVICE_AUTO_START && back.running &&
                    back.delayed == 1 && legacy.valid &&
                    legacy.start == SERVICE_DEMAND_START && !legacy.running &&
                    legacy.delayed == -1;
    detail = ok ? L"auto, running and delayed kept; a two-field record reads as not recorded"
                : L"encoded \"" + EncodeServiceRecord(parked) + L"\" decoded start " +
                      std::to_wstring(back.start) + L", delayed " +
                      std::to_wstring(back.delayed);
    return ok;
}

bool BoostDwordRoundTrip(std::wstring& detail) {
    const std::wstring name = L"SelfTestDword";
    const DWORD expected = 4321;
    bool written = WriteDword(HKEY_CURRENT_USER, kJournalKey, name, expected);
    DWORD readBack = 0;
    bool read = ReadDword(HKEY_CURRENT_USER, kJournalKey, name, readBack);
    std::wstring ignored;
    DeleteRegValue(HKEY_CURRENT_USER, kJournalKey, name, 0, ignored);
    detail = L"wrote " + std::to_wstring(expected) + L", read " + std::to_wstring(readBack);
    return written && read && readBack == expected;
}

BoostState LoadBoostState() {
    BoostState state;
    state.active = JournalRead(L"Active") == L"1";
    state.planName = JournalRead(L"PrevPlanName");
    state.currentPlan = CurrentPowerPlanName();
    for (const wchar_t* name : kParkedServices)
        if (!JournalRead(std::wstring(L"Svc|") + name).empty()) state.parked.push_back(name);
    return state;
}

BoostResult ApplyBoost() {
    BoostResult result;
    if (LoadBoostState().active) {
        result.message = L"Boost is already on - Restore it first to apply it again.";
        return result;
    }

    // A refusal is different from "nothing needed doing": it must be reported, and it must
    // stop the profile from claiming the machine is in boost shape when it is not.
    std::wstring firstProblem;
    auto refuse = [&](const std::wstring& why, DWORD code) {
        ++result.failed;
        if (code == ERROR_ACCESS_DENIED) result.adminNeeded = true;
        if (firstProblem.empty()) firstProblem = why;
    };

    // 1. power plan
    std::wstring previous = ActivePlanGuid();
    JournalWrite(L"PrevPlan", previous);
    JournalWrite(L"PrevPlanName", PlanName(previous));
    GUID highPerformance;
    if (ParseGuid(kHighPerformancePlan, highPerformance)) {
        const std::wstring wanted = GuidToString(highPerformance);
        DWORD rc = PowerSetActiveScheme(nullptr, &highPerformance);
        if (rc == ERROR_SUCCESS)
            ++result.changed;
        else if (ActivePlanGuid() != wanted)
            refuse(L"the High performance plan could not be selected: " + WinErrorText(rc), rc);
    } else {
        refuse(L"the High performance plan is not available on this PC", ERROR_NOT_FOUND);
    }

    // 2. background services
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        DWORD rc = GetLastError();
        refuse(L"the service manager could not be opened: " + WinErrorText(rc), rc);
    } else {
        for (const wchar_t* name : kParkedServices) {
            SC_HANDLE service = OpenServiceW(
                scm, name, SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS |
                               SERVICE_STOP | SERVICE_START);
            if (!service) {
                DWORD rc = GetLastError();
                // Not installed on this PC is not a failure; no rights to park it is.
                if (rc == ERROR_ACCESS_DENIED)
                    refuse(std::wstring(name) + L": " + WinErrorText(rc), rc);
                continue;
            }
            ServiceSnapshot before = QueryService(service);
            if (!before.valid || before.start == SERVICE_DISABLED ||
                before.start != SERVICE_AUTO_START) {
                CloseServiceHandle(service);
                continue;
            }

            const std::wstring entry = std::wstring(L"Svc|") + name;
            ServiceRecord record;
            record.start = before.start;
            record.running = before.running;
            record.delayed = before.delayed ? 1 : 0;
            JournalWrite(entry, EncodeServiceRecord(record));

            // The start type is changed before the service is stopped. If the change is
            // refused nothing has happened yet, so the journal entry is dropped again and
            // the service keeps running exactly as it was.
            if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_DEMAND_START,
                                      SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr)) {
                DWORD rc = GetLastError();
                JournalDelete(entry);
                refuse(std::wstring(name) + L": " + WinErrorText(rc), rc);
                CloseServiceHandle(service);
                continue;
            }
            ++result.changed;

            if (before.running) {
                SERVICE_STATUS status = {};
                ControlService(service, SERVICE_CONTROL_STOP, &status);
                for (int attempt = 0; attempt < 40; ++attempt) {
                    SERVICE_STATUS_PROCESS probe = {};
                    DWORD needed = 0;
                    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                             reinterpret_cast<LPBYTE>(&probe), sizeof(probe),
                                             &needed) &&
                        probe.dwCurrentState == SERVICE_STOPPED)
                        break;
                    Sleep(150);
                }
            }
            CloseServiceHandle(service);
        }
        CloseServiceHandle(scm);
    }

    // 3. Game DVR background recording
    DWORD dvr = 0;
    if (ReadDword(HKEY_CURRENT_USER, kGameDvrKey, L"GameDVR_Enabled", dvr)) {
        JournalWrite(L"GameDvr", std::to_wstring(dvr));
        if (dvr != 0) {
            if (WriteDword(HKEY_CURRENT_USER, kGameDvrKey, L"GameDVR_Enabled", 0))
                ++result.changed;
            else {
                DWORD rc = GetLastError();
                refuse(L"Game DVR recording could not be turned off: " + WinErrorText(rc), rc);
            }
        }
    }
    DWORD capture = 0;
    if (ReadDword(HKEY_LOCAL_MACHINE, kAppCaptureKey, L"AppCaptureEnabled", capture)) {
        JournalWrite(L"AppCapture", std::to_wstring(capture));
        if (capture != 0) {
            if (WriteDword(HKEY_LOCAL_MACHINE, kAppCaptureKey, L"AppCaptureEnabled", 0))
                ++result.changed;
            else {
                DWORD rc = GetLastError();
                refuse(L"the background capture setting could not be turned off: " +
                           WinErrorText(rc),
                       rc);
            }
        }
    }

    // The profile only counts as on when something changed, or when nothing needed doing.
    // A run that was refused outright leaves it off, so Restore is not offered for a boost
    // that never happened.
    if (result.changed > 0 || result.failed == 0) JournalWrite(L"Active", L"1");

    wchar_t buf[320];
    if (result.failed > 0) {
        result.ok = false;
        if (result.changed == 0) {
            swprintf(buf, 320, L"Nothing could be changed: %lu setting(s) refused - ",
                     (unsigned long)result.failed);
        } else {
            swprintf(buf, 320, L"Boost partly on: %lu change(s) applied, %lu refused - ",
                     (unsigned long)result.changed, (unsigned long)result.failed);
        }
        result.message = buf + firstProblem;
        if (result.adminNeeded) result.message += L" Restart as administrator and try again.";
    } else if (result.changed == 0) {
        result.message = L"Nothing needed changing - this PC was already in boost shape.";
    } else {
        swprintf(buf, 320, L"Boost on: %lu change(s) applied. Restore undoes every one.",
                 (unsigned long)result.changed);
        result.message = buf;
    }
    return result;
}

BoostResult RestoreBoost() {
    BoostResult result;
    if (!LoadBoostState().active) {
        result.message = L"The system profile is not on, so there is nothing to put back.";
        return result;
    }

    std::wstring firstProblem;
    auto failed = [&](const std::wstring& why, DWORD code) {
        ++result.failed;
        if (code == ERROR_ACCESS_DENIED) result.adminNeeded = true;
        if (firstProblem.empty()) firstProblem = why;
    };

    // services first, so the machine is usable while the rest is put back. A record is only
    // dropped once the service is really back: losing it on a refused write would leave the
    // service parked at Manual with nothing left to say what it used to be.
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        DWORD rc = GetLastError();
        failed(L"the service manager could not be opened: " + WinErrorText(rc), rc);
    } else {
        for (const wchar_t* name : kParkedServices) {
            const std::wstring entry = std::wstring(L"Svc|") + name;
            std::wstring journal = JournalRead(entry);
            if (journal.empty()) continue;
            ServiceRecord record = DecodeServiceRecord(journal);
            if (!record.valid) {
                JournalDelete(entry);
                continue;
            }
            SC_HANDLE service = OpenServiceW(
                scm, name, SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG | SERVICE_START |
                              SERVICE_STOP);
            if (!service) {
                DWORD rc = GetLastError();
                failed(std::wstring(name) + L": " + WinErrorText(rc), rc);
                continue;
            }
            if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, record.start, SERVICE_NO_CHANGE,
                                      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                      nullptr)) {
                DWORD rc = GetLastError();
                failed(std::wstring(name) + L": " + WinErrorText(rc), rc);
                CloseServiceHandle(service);
                continue;  // keep the record so a retry can still put it back
            }
            ++result.changed;
            if (record.delayed >= 0 && record.start == SERVICE_AUTO_START) {
                SERVICE_DELAYED_AUTO_START_INFO delayed = {};
                delayed.fDelayedAutostart = record.delayed != 0;
                if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                           &delayed)) {
                    DWORD rc = GetLastError();
                    failed(std::wstring(name) + L" delayed start: " + WinErrorText(rc), rc);
                    CloseServiceHandle(service);
                    continue;
                }
            }
            if (record.running) StartServiceW(service, 0, nullptr);
            CloseServiceHandle(service);
            JournalDelete(entry);
        }
        CloseServiceHandle(scm);
    }

    // power plan
    std::wstring plan = JournalRead(L"PrevPlan");
    GUID previous;
    if (ParseGuid(plan, previous)) {
        DWORD rc = PowerSetActiveScheme(nullptr, &previous);
        if (rc == ERROR_SUCCESS) {
            ++result.changed;
            JournalDelete(L"PrevPlan");
            JournalDelete(L"PrevPlanName");
        } else {
            failed(L"the previous power plan could not be selected: " + WinErrorText(rc), rc);
        }
    } else {
        JournalDelete(L"PrevPlan");
        JournalDelete(L"PrevPlanName");
    }

    // Game DVR
    if (!JournalRead(L"GameDvr").empty()) {
        if (WriteDword(HKEY_CURRENT_USER, kGameDvrKey, L"GameDVR_Enabled",
                       JournalReadDword(L"GameDvr", 1))) {
            ++result.changed;
            JournalDelete(L"GameDvr");
        } else {
            DWORD rc = GetLastError();
            failed(L"Game DVR recording could not be put back: " + WinErrorText(rc), rc);
        }
    }
    if (!JournalRead(L"AppCapture").empty()) {
        if (WriteDword(HKEY_LOCAL_MACHINE, kAppCaptureKey, L"AppCaptureEnabled",
                       JournalReadDword(L"AppCapture", 1))) {
            ++result.changed;
            JournalDelete(L"AppCapture");
        } else {
            DWORD rc = GetLastError();
            failed(L"the background capture setting could not be put back: " + WinErrorText(rc), rc);
        }
    }

    wchar_t buf[320];
    if (result.failed > 0) {
        // Still on: the parts that were put back are gone from the journal, and the rest can
        // be retried once the rights are there.
        result.ok = false;
        swprintf(buf, 320, L"Restored %lu change(s); %lu refused - ",
                 (unsigned long)result.changed, (unsigned long)result.failed);
        result.message = buf + firstProblem;
        if (result.adminNeeded) result.message += L" Restart as administrator and try again.";
    } else {
        JournalWrite(L"Active", L"0");
        swprintf(buf, 320, L"Restored: %lu change(s) put back.", (unsigned long)result.changed);
        result.message = buf;
    }
    return result;
}
