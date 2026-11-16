// svcctrl.cpp - full service control for the Services page.
//
// The Deactivate page only parks auto-start third-party services; this engine is the
// general one: every installed Win32 service, its start type (including the delayed
// automatic flag), start/stop, and creating, editing and deleting entries.
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core.h"

namespace {

SC_HANDLE OpenManager(DWORD access) {
    return OpenSCManagerW(nullptr, nullptr, access);
}

// Every failure carries whether more rights would have been enough, so the page can
// offer the administrator restart instead of just reporting a bare error.
ServiceOp Fail(DWORD code) {
    ServiceOp op;
    op.error = WinErrorText(code);
    op.adminNeeded = (code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD);
    return op;
}

bool ReadStartType(SC_HANDLE service, DWORD& startType, bool& delayed) {
    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    if (!needed) return false;
    std::vector<unsigned char> buffer(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service, config, needed, &needed)) return false;
    startType = config->dwStartType;

    SERVICE_DELAYED_AUTO_START_INFO info = {};
    DWORD returned = 0;
    delayed = QueryServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                   reinterpret_cast<LPBYTE>(&info), sizeof(info), &returned) &&
              info.fDelayedAutostart;
    return true;
}

std::wstring ReadDescription(SC_HANDLE service) {
    DWORD needed = 0;
    QueryServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &needed);
    if (!needed) return std::wstring();
    std::vector<unsigned char> buffer(needed);
    auto* info = reinterpret_cast<SERVICE_DESCRIPTIONW*>(buffer.data());
    if (!QueryServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION,
                              reinterpret_cast<LPBYTE>(info), needed, &needed) ||
        !info->lpDescription)
        return std::wstring();
    return info->lpDescription;
}

bool WriteDescription(SC_HANDLE service, const std::wstring& description) {
    SERVICE_DESCRIPTIONW info = {};
    info.lpDescription = const_cast<wchar_t*>(description.c_str());
    return ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &info) != FALSE;
}

}  // namespace

std::wstring ServiceStartTypeLabel(DWORD startType) {
    switch (startType) {
        case SERVICE_BOOT_START:
            return L"Boot";
        case SERVICE_SYSTEM_START:
            return L"System";
        case SERVICE_AUTO_START:
            return L"Automatic";
        case SERVICE_DEMAND_START:
            return L"Manual";
        case SERVICE_DISABLED:
            return L"Disabled";
        default:
            return L"Unknown";
    }
}

std::wstring ServiceStateLabel(DWORD state) {
    switch (state) {
        case SERVICE_STOPPED:
            return L"Stopped";
        case SERVICE_START_PENDING:
            return L"Starting";
        case SERVICE_STOP_PENDING:
            return L"Stopping";
        case SERVICE_RUNNING:
            return L"Running";
        case SERVICE_CONTINUE_PENDING:
            return L"Resuming";
        case SERVICE_PAUSE_PENDING:
            return L"Pausing";
        case SERVICE_PAUSED:
            return L"Paused";
        default:
            return L"Unknown";
    }
}

std::vector<ServiceRow> LoadAllServices() {
    std::vector<ServiceRow> rows;
    SC_HANDLE manager = OpenManager(SC_MANAGER_ENUMERATE_SERVICE);
    if (!manager) return rows;

    DWORD bytesNeeded = 0, count = 0, resume = 0;
    EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                          nullptr, 0, &bytesNeeded, &count, &resume, nullptr);
    if (!bytesNeeded) {
        CloseServiceHandle(manager);
        return rows;
    }
    std::vector<unsigned char> buffer(bytesNeeded);
    if (!EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                               buffer.data(), bytesNeeded, &bytesNeeded, &count, &resume,
                               nullptr)) {
        CloseServiceHandle(manager);
        return rows;
    }

    auto* entries = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    rows.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        ServiceRow row;
        row.name = entries[i].lpServiceName ? entries[i].lpServiceName : L"";
        row.display = entries[i].lpDisplayName ? entries[i].lpDisplayName : row.name;
        row.state = entries[i].ServiceStatusProcess.dwCurrentState;
        row.running = row.state == SERVICE_RUNNING || row.state == SERVICE_START_PENDING;
        row.driver = (entries[i].ServiceStatusProcess.dwServiceType & SERVICE_DRIVER) != 0;

        SC_HANDLE service = OpenServiceW(manager, row.name.c_str(),
                                         SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
        if (!service) {
            row.error = WinErrorText(GetLastError());
            rows.push_back(row);
            continue;
        }
        DWORD needed = 0;
        QueryServiceConfigW(service, nullptr, 0, &needed);
        std::vector<unsigned char> configBuffer(needed ? needed : 1);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data());
        if (needed && QueryServiceConfigW(service, config, needed, &needed)) {
            row.imagePath = config->lpBinaryPathName ? config->lpBinaryPathName : L"";
            if (config->lpDisplayName && config->lpDisplayName[0])
                row.display = config->lpDisplayName;
        }
        ReadStartType(service, row.startType, row.delayed);
        row.description = ReadDescription(service);
        row.windows = !IsThirdPartyServicePath(row.imagePath);
        CloseServiceHandle(service);
        rows.push_back(row);
    }
    CloseServiceHandle(manager);

    std::sort(rows.begin(), rows.end(), [](const ServiceRow& a, const ServiceRow& b) {
        return _wcsicmp(a.display.c_str(), b.display.c_str()) < 0;
    });
    return rows;
}

bool SetServiceStart(const std::wstring& name, DWORD startType, bool delayed, ServiceOp& op) {
    op = ServiceOp();
    SC_HANDLE manager = OpenManager(SC_MANAGER_CONNECT);
    if (!manager) {
        op = Fail(GetLastError());
        op.ok = false;
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, name.c_str(), SERVICE_CHANGE_CONFIG);
    if (!service) {
        DWORD code = GetLastError();
        CloseServiceHandle(manager);
        op = Fail(code);
        return false;
    }
    bool ok = ChangeServiceConfigW(service, SERVICE_NO_CHANGE, startType, SERVICE_NO_CHANGE,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr) != FALSE;
    if (ok) {
        SERVICE_DELAYED_AUTO_START_INFO info = {};
        info.fDelayedAutostart = (startType == SERVICE_AUTO_START && delayed) ? TRUE : FALSE;
        ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &info);
    }
    DWORD code = ok ? 0 : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!ok) {
        op = Fail(code);
        return false;
    }
    op.ok = true;
    return true;
}

bool StartNamedService(const std::wstring& name, ServiceOp& op) {
    op = ServiceOp();
    SC_HANDLE manager = OpenManager(SC_MANAGER_CONNECT);
    if (!manager) {
        op = Fail(GetLastError());
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, name.c_str(), SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        DWORD code = GetLastError();
        CloseServiceHandle(manager);
        op = Fail(code);
        return false;
    }
    bool ok = StartServiceW(service, 0, nullptr) != FALSE;
    DWORD code = ok ? 0 : GetLastError();
    if (ok) {
        SERVICE_STATUS status = {};
        // Give it a moment to come up so the row shows Running when the list refreshes.
        for (int i = 0; i < 20; ++i) {
            if (!QueryServiceStatus(service, &status)) break;
            if (status.dwCurrentState != SERVICE_START_PENDING) break;
            Sleep(150);
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!ok) {
        op = Fail(code);
        return false;
    }
    op.ok = true;
    return true;
}

bool StopNamedService(const std::wstring& name, ServiceOp& op) {
    op = ServiceOp();
    SC_HANDLE manager = OpenManager(SC_MANAGER_CONNECT);
    if (!manager) {
        op = Fail(GetLastError());
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        DWORD code = GetLastError();
        CloseServiceHandle(manager);
        op = Fail(code);
        return false;
    }
    SERVICE_STATUS status = {};
    bool ok = ControlService(service, SERVICE_CONTROL_STOP, &status) != FALSE;
    DWORD code = ok ? 0 : GetLastError();
    if (ok) {
        for (int i = 0; i < 40; ++i) {
            if (!QueryServiceStatus(service, &status)) break;
            if (status.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(150);
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!ok) {
        op = Fail(code);
        return false;
    }
    op.ok = true;
    return true;
}

bool CreateNewService(const std::wstring& name, const std::wstring& display,
                      const std::wstring& imagePath, const std::wstring& description,
                      ServiceOp& op) {
    op = ServiceOp();
    if (name.empty() || imagePath.empty()) {
        op.error = L"A service needs a name and a binary path.";
        return false;
    }
    SC_HANDLE manager = OpenManager(SC_MANAGER_CREATE_SERVICE);
    if (!manager) {
        op = Fail(GetLastError());
        return false;
    }
    SC_HANDLE service = CreateServiceW(
        manager, name.c_str(), display.empty() ? name.c_str() : display.c_str(),
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL, imagePath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!service) {
        DWORD code = GetLastError();
        CloseServiceHandle(manager);
        op = Fail(code);
        return false;
    }
    if (!description.empty()) WriteDescription(service, description);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    op.ok = true;
    return true;
}

bool UpdateExistingService(const std::wstring& name, const std::wstring& display,
                           const std::wstring& imagePath, const std::wstring& description,
                           ServiceOp& op) {
    op = ServiceOp();
    SC_HANDLE manager = OpenManager(SC_MANAGER_CONNECT);
    if (!manager) {
        op = Fail(GetLastError());
        return false;
    }
    SC_HANDLE service =
        OpenServiceW(manager, name.c_str(), SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG);
    if (!service) {
        DWORD code = GetLastError();
        CloseServiceHandle(manager);
        op = Fail(code);
        return false;
    }
    bool ok = ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                                   SERVICE_NO_CHANGE, imagePath.empty() ? nullptr
                                                                       : imagePath.c_str(),
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   display.empty() ? nullptr : display.c_str()) != FALSE;
    if (ok && !description.empty()) WriteDescription(service, description);
    DWORD code = ok ? 0 : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!ok) {
        op = Fail(code);
        return false;
    }
    op.ok = true;
    return true;
}

bool DeleteNamedService(const ServiceRow& row, ServiceOp& op) {
    op = ServiceOp();
    // Windows owns its own binaries: deleting those rows is never offered, and this is
    // the second lock on the same door in case a caller forgets.
    if (row.windows) {
        op.error = L"Windows components are never removed from here.";
        return false;
    }
    SC_HANDLE manager = OpenManager(SC_MANAGER_CONNECT);
    if (!manager) {
        op = Fail(GetLastError());
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, row.name.c_str(), DELETE | SERVICE_STOP |
                                                                     SERVICE_QUERY_STATUS);
    if (!service) {
        DWORD code = GetLastError();
        CloseServiceHandle(manager);
        op = Fail(code);
        return false;
    }
    // A running service cannot be uninstalled cleanly; stop it first and say so if it
    // refuses, rather than leaving Windows with a pending delete.
    SERVICE_STATUS status = {};
    if (QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED) {
        ControlService(service, SERVICE_CONTROL_STOP, &status);
        for (int i = 0; i < 40; ++i) {
            if (!QueryServiceStatus(service, &status)) break;
            if (status.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(150);
        }
        if (status.dwCurrentState != SERVICE_STOPPED) {
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            op.error = L"The service would not stop, so it was left in place.";
            return false;
        }
    }
    bool ok = DeleteService(service) != FALSE;
    DWORD code = ok ? 0 : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!ok) {
        op = Fail(code);
        return false;
    }
    op.ok = true;
    return true;
}
