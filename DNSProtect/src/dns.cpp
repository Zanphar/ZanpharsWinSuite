// ============================================================================
//  dns.cpp — engine implementation.
// ============================================================================

#include "dns.h"
#include "log.h"

namespace dzp {

namespace {

constexpr wchar_t kInterfacesKey[] =
    L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces";

// Registry -------------------------------------------------------------------

bool RegGetDword(HKEY key, const wchar_t* name, DWORD* out) {
    DWORD type = 0, cb = sizeof(DWORD), val = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&val), &cb)
        != ERROR_SUCCESS || type != REG_DWORD) {
        return false;
    }
    *out = val;
    return true;
}

std::wstring RegGetString(HKEY key, const wchar_t* name, bool* existed = nullptr) {
    if (existed) *existed = false;
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ)) {
        return L"";
    }
    std::vector<BYTE> buf(static_cast<size_t>(cb) + 2, 0);
    if (RegQueryValueExW(key, name, nullptr, &type, buf.data(), &cb) != ERROR_SUCCESS) {
        return L"";
    }
    if (existed) *existed = true;

    if (type == REG_MULTI_SZ) {  // join with commas for uniform handling
        std::wstring joined, cur;
        const wchar_t* p = reinterpret_cast<const wchar_t*>(buf.data());
        const size_t n = cb / sizeof(wchar_t);
        for (size_t i = 0; i <= n; ++i) {
            if (i < n && p[i]) {
                cur.push_back(p[i]);
            } else {
                if (!cur.empty()) {
                    if (!joined.empty()) joined += L',';
                    joined += cur;
                }
                cur.clear();
            }
        }
        return joined;
    }
    const wchar_t* p = reinterpret_cast<const wchar_t*>(buf.data());
    return p ? std::wstring(p) : L"";
}

bool RegWriteString(HKEY root, const std::wstring& sub,
                    const std::wstring& name, const std::wstring& value) {
    HKEY key;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    const LONG r = RegSetValueExW(key, name.c_str(), 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(value.c_str()),
                                  static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

// Adapter enumeration ----------------------------------------------------------

std::map<std::wstring, std::pair<std::wstring, bool>> FriendlyNames() {
    std::map<std::wstring, std::pair<std::wstring, bool>> byGuid;  // guid -> (name, up)
    ULONG size = 16 * 1024;
    std::vector<BYTE> buf(size);
    const PIP_ADAPTER_ADDRESSES first = [&] {
        for (int attempt = 0; attempt < 3; ++attempt) {
            ULONG r = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr,
                                           reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()),
                                           &size);
            if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(size); continue; }
            if (r != ERROR_SUCCESS) return static_cast<PIP_ADAPTER_ADDRESSES>(nullptr);
            return reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        }
        return static_cast<PIP_ADAPTER_ADDRESSES>(nullptr);
    }();

    for (auto a = first; a; a = a->Next) {
        std::wstring guid;
        if (a->AdapterName) guid.assign(a->AdapterName, a->AdapterName + strlen(a->AdapterName));
        guid = IfaceId(guid);
        if (guid.empty()) continue;
        std::wstring name = (a->FriendlyName && *a->FriendlyName)
                                ? a->FriendlyName : L"(unnamed adapter)";
        byGuid[guid] = { name, a->OperStatus == IfOperStatusUp };
    }
    return byGuid;
}

bool IsIpEnabledKey(HKEY key, bool* dhcpOut) {
    bool hasDhcpVal = false;
    DWORD dw = 0;
    if (RegGetDword(key, L"EnableDhcp", &dw)) {
        hasDhcpVal = true;
        if (dhcpOut) *dhcpOut = dw != 0;
    }
    bool hasDhcpAddr = false;
    RegGetString(key, L"DhcpIPAddress", &hasDhcpAddr);
    bool hasNs = false;
    RegGetString(key, L"NameServer", &hasNs);
    bool hasDns = false;
    RegGetString(key, L"DhcpNameServer", &hasDns);
    return hasDhcpVal || hasDhcpAddr || hasNs || hasDns;
}

// Nudges the network stack to re-read a changed configuration. Both calls are
// best-effort: they fail silently where unsupported.
void NotifyInterfaceChanged(const std::wstring& key) {
    using Fn = BOOL(WINAPI*)(LPWSTR, LPWSTR, BOOL, DWORD, DWORD, DWORD, INT);
    if (HMODULE m = LoadLibraryW(L"dhcpsapi.dll")) {
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "DhcpNotifyConfigChange"))) {
            // The API may write into the adapter name; give it a mutable buffer.
            std::vector<wchar_t> name(key.begin(), key.end());
            name.push_back(L'\0');
            fn(nullptr, name.data(), FALSE, 0, 0, 0, 0);
        }
        FreeLibrary(m);
    }
}

void FlushDnsCache() {
    // `ipconfig /flushdns`: CreateProcessW may modify the command line, so it
    // must live in a writable buffer (a string literal would crash).
    wchar_t cmdline[] = L"ipconfig /flushdns";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Engine::Impl
// ---------------------------------------------------------------------------

struct Engine::Impl {
    using Baseline = std::map<std::wstring, std::pair<std::wstring, std::wstring>>;

    DataDirs               dirs;
    HWND                   notify   = nullptr;
    bool                   elevated = false;
    Config                 cfg;

    CRITICAL_SECTION       cs;
    Baseline               baseline;      // used in "system" provider mode
    unsigned long long     hijacks = 0;
    std::map<std::wstring, DWORD> throttle;  // iface key -> tick of last log

    HANDLE   stopEvent = nullptr;
    HANDLE   thread    = nullptr;

    // ------------------------------------------------------------------ util

    void SaveBaselineLocked() {
        std::wstring blob;
        for (const auto& kv : baseline) {
            blob += Fmt(L"%ls.static=%ls", kv.first.c_str(), kv.second.first.c_str());
            blob.push_back(0);
            blob += Fmt(L"%ls.dhcp=%ls",  kv.first.c_str(), kv.second.second.c_str());
            blob.push_back(0);
        }
        blob.push_back(0);
        std::wstring ini = dirs.ini.wstring();
        WritePrivateProfileSectionW(L"Baseline", blob.c_str(), ini.c_str());
    }

    void LoadBaseline() {
        std::vector<wchar_t> buf(32768, 0);
        GetPrivateProfileSectionW(L"Baseline", buf.data(), 32768, dirs.ini.c_str());
        const wchar_t* p = buf.data();
        while (*p) {
            const std::wstring line = p;
            const size_t eq = line.find(L'=');
            if (eq != std::wstring::npos) {
                std::wstring k = Trim(line.substr(0, eq));
                const std::wstring v = Trim(line.substr(eq + 1));
                bool isDhcp = false;
                if (k.size() > 5 && k.compare(k.size() - 5, 5, L".dhcp") == 0) {
                    isDhcp = true; k.erase(k.size() - 5);
                } else if (k.size() > 7 && k.compare(k.size() - 7, 7, L".static") == 0) {
                    k.erase(k.size() - 7);
                } else {
                    k.clear();
                }
                if (!k.empty()) {
                    auto& slot = baseline[k];
                    if (isDhcp) slot.second = v; else slot.first = v;
                }
            }
            p += line.size() + 1;
        }
    }

    // Approved values for one adapter. In "system" mode unknown adapters are
    // adopted and persisted, so a config appearing "out of nowhere" (e.g. a
    // VPN tunnel coming up) is preserved rather than fought over.
    std::pair<std::wstring, std::wstring> ExpectedForLocked(const Iface& f) {
        if (cfg.Provider == provider::kSystem) {
            const std::wstring id = IfaceId(f.Key);
            auto it = baseline.find(id);
            if (it != baseline.end()) return it->second;
            baseline[id] = { f.NameServer, f.DhcpNameServer };
            SaveBaselineLocked();
            const std::wstring cur = !f.NameServer.empty() ? f.NameServer : f.DhcpNameServer;
            log::Info(Fmt(L"New adapter detected, protecting its current DNS: %ls -> '%ls'",
                          f.Name.c_str(), cur.c_str()));
            return { f.NameServer, f.DhcpNameServer };
        }

        std::wstring p, s;
        if (cfg.Provider == provider::kCustom) {
            p = cfg.Primary; s = cfg.Secondary;
        } else if (const Provider* pr = provider::ById(cfg.Provider)) {
            p = pr->Primary; s = pr->Secondary;
        }
        std::wstring stat = p;
        if (!s.empty()) { stat += L','; stat += s; }
        return { stat, p };
    }

    // ------------------------------------------------------------------ apply

    bool WriteDnsTo(const Iface& f, const std::wstring& stat, const std::wstring& dhcp) {
        const std::wstring sub = std::wstring(kInterfacesKey) + L"\\" + f.Key;
        bool ok = RegWriteString(HKEY_LOCAL_MACHINE, sub, L"NameServer", stat);
        if (ok && f.Dhcp && !dhcp.empty()) {
            ok = RegWriteString(HKEY_LOCAL_MACHINE, sub, L"DhcpNameServer", dhcp);
        }
        if (ok) NotifyInterfaceChanged(f.Key);
        return ok;
    }

    void ApplyApprovedDnsEverywhere() {
        EnterCriticalSection(&cs);
        for (const auto& f : Enumerate()) {
            const auto exp = ExpectedForLocked(f);
            const bool ok = SameDnsList(f.NameServer, exp.first) &&
                            (!f.Dhcp || SameDnsList(f.DhcpNameServer, exp.second));
            if (ok) continue;
            log::Info(Fmt(L"Applying DNS on '%ls': NameServer='%ls'%ls%ls",
                          f.Name.c_str(), exp.first.c_str(),
                          f.Dhcp ? L" DhcpNameServer='" : L"",
                          f.Dhcp ? exp.second.c_str() : L"'"));
            WriteDnsTo(f, exp.first, exp.second);
        }
        LeaveCriticalSection(&cs);
        FlushDnsCache();
    }

    // ------------------------------------------------------------------ watch

    void CheckAndRestore() {
        bool changed = false;
        EnterCriticalSection(&cs);
        for (const auto& f : Enumerate()) {
            const auto exp = ExpectedForLocked(f);
            const bool ok = SameDnsList(f.NameServer, exp.first) &&
                            (!f.Dhcp || SameDnsList(f.DhcpNameServer, exp.second));
            if (ok) continue;

            const DWORD tick = GetTickCount();
            bool doLog = true;
            auto it = throttle.find(f.Key);
            if (it != throttle.end() && tick - it->second < 5000) {
                doLog = false;
            } else {
                throttle[f.Key] = tick;
            }
            if (doLog) {
                log::Warn(Fmt(L"DNS change detected on '%ls': found '%ls'%ls, restoring '%ls'",
                              f.Name.c_str(), f.NameServer.c_str(),
                              f.Dhcp ? Fmt(L" (DhcpNameServer='%ls')", f.DhcpNameServer.c_str()).c_str()
                                     : L"",
                              exp.first.c_str()));
            }
            WriteDnsTo(f, exp.first, exp.second);
            ++hijacks;
            changed = true;
        }
        LeaveCriticalSection(&cs);
        if (changed && notify) PostMessageW(notify, WM_APP_REFRESH, 0, 0);
    }

    static DWORD WINAPI ThreadProc(LPVOID self) {
        auto* impl = static_cast<Impl*>(self);
        for (;;) {
            const DWORD w = WaitForSingleObject(impl->stopEvent, 1500);
            if (w != WAIT_TIMEOUT) break;
            bool enabled;
            EnterCriticalSection(&impl->cs);
            enabled = impl->cfg.Enabled;
            LeaveCriticalSection(&impl->cs);
            if (!enabled) continue;
            impl->CheckAndRestore();
        }
        return 0;
    }

    // ------------------------------------------------------------------ enum

    std::vector<Iface> Enumerate() const {  // caller must hold the lock
        const auto friendly = FriendlyNames();
        std::vector<Iface> out;
        HKEY root;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kInterfacesKey, 0,
                          KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) {
            return out;
        }
        for (DWORD i = 0; ; ++i) {
            wchar_t name[256];
            DWORD len = 256;
            if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr)
                != ERROR_SUCCESS) {
                break;
            }
            HKEY key;
            if (RegOpenKeyExW(root, name, 0, KEY_READ | KEY_WOW64_64KEY, &key)
                != ERROR_SUCCESS) {
                continue;
            }
            Iface f;
            f.Key = name;
            bool dhcp = false;
            if (!IsIpEnabledKey(key, &dhcp)) { RegCloseKey(key); continue; }
            f.Dhcp           = dhcp;
            f.NameServer     = RegGetString(key, L"NameServer");
            f.DhcpNameServer = RegGetString(key, L"DhcpNameServer");
            RegCloseKey(key);

            const auto it = friendly.find(IfaceId(f.Key));
            if (it != friendly.end()) {
                f.Name = it->second.first;
                f.Up   = it->second.second;
            } else {
                f.Name = Fmt(L"Adapter %ls", f.Key.c_str());
            }
            out.push_back(std::move(f));
        }
        RegCloseKey(root);
        return out;
    }
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Engine& Engine::Instance() {
    static Engine engine;   // thread-safe static init
    return engine;
}

Engine::Engine()  = default;
Engine::~Engine() = default;

bool Engine::Prepare(const DataDirs& dirs, HWND notify, bool elevated) {
    impl_ = new Impl();
    impl_->dirs      = dirs;
    impl_->notify    = notify;
    impl_->elevated  = elevated;
    impl_->cfg       = LoadConfig(dirs);
    impl_->LoadBaseline();
    InitializeCriticalSection(&impl_->cs);
    impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->thread    = CreateThread(nullptr, 0, &Impl::ThreadProc, impl_, 0, nullptr);
    return impl_->thread != nullptr;
}

void Engine::SetNotify(HWND window) {
    if (!impl_) return;
    EnterCriticalSection(&impl_->cs);
    impl_->notify = window;
    LeaveCriticalSection(&impl_->cs);
    if (window) PostMessageW(window, WM_APP_REFRESH, 0, 0);
}

void Engine::Shutdown() {
    if (!impl_) return;
    SetEvent(impl_->stopEvent);
    if (impl_->thread) {
        WaitForSingleObject(impl_->thread, 2000);
        CloseHandle(impl_->thread);
        impl_->thread = nullptr;
    }
    DeleteCriticalSection(&impl_->cs);
    CloseHandle(impl_->stopEvent);
    delete impl_;
    impl_ = nullptr;
}

Status Engine::Snapshot() {
    Status s;
    if (!impl_) return s;
    EnterCriticalSection(&impl_->cs);
    s.Enabled   = impl_->cfg.Enabled;
    s.Provider  = impl_->cfg.Provider;
    s.CustomPrimary   = impl_->cfg.Primary;
    s.CustomSecondary = impl_->cfg.Secondary;
    s.HijacksReverted = impl_->hijacks;
    s.Elevated        = impl_->elevated;

    for (const auto& f : impl_->Enumerate()) {
        IfaceView v;
        v.adapter = f;
        if (impl_->cfg.Enabled) {
            const auto exp = impl_->ExpectedForLocked(f);
            const bool ok = SameDnsList(f.NameServer, exp.first) &&
                            (!f.Dhcp || SameDnsList(f.DhcpNameServer, exp.second));
            v.State    = ok ? AdapterState::Ok : AdapterState::Mismatch;
            v.Expected = exp.first;
        }
        s.Adapters.push_back(std::move(v));
    }
    LeaveCriticalSection(&impl_->cs);
    return s;
}

std::wstring Engine::Enable(const std::wstring& providerId,
                            const std::wstring& primary,
                            const std::wstring& secondary) {
    if (!impl_) return L"";
    if (!impl_->elevated) {
        return L"Administrator rights are required to protect DNS settings. "
               L"Right-click the program and choose 'Run as administrator'.";
    }
    if (providerId == provider::kCustom) {
        if (primary.empty() || !IsValidIpv4(primary) ||
            (!secondary.empty() && !IsValidIpv4(secondary))) {
            return L"The DNS server address is not a valid IPv4 address.";
        }
    }

    Impl::Baseline baselineCopy;
    {
        EnterCriticalSection(&impl_->cs);
        baselineCopy = impl_->baseline;
        impl_->cfg.Enabled   = true;
        impl_->cfg.Provider  = providerId;
        impl_->cfg.Primary   = primary;
        impl_->cfg.Secondary = secondary;
        LeaveCriticalSection(&impl_->cs);
    }
    SaveConfig(impl_->dirs, impl_->cfg);

    if (providerId == provider::kSystem && baselineCopy.empty()) {
        // First ever enable in system mode: adopt whatever is configured now
        // (ExpectedForLocked will persist the baseline on the next check).
        log::Info(L"DNS protection enabled for the first time; capturing current DNS as baseline");
    }
    if (providerId == provider::kSystem) {
        log::Info(L"DNS protection enabled (system settings protected as-is)");
    } else {
        log::Info(Fmt(L"DNS protection enabled using provider '%ls'", providerId.c_str()));
        impl_->ApplyApprovedDnsEverywhere();
    }
    if (impl_->notify) PostMessageW(impl_->notify, WM_APP_REFRESH, 0, 0);
    return L"";
}

void Engine::Disable() {
    if (!impl_) return;
    EnterCriticalSection(&impl_->cs);
    impl_->cfg.Enabled = false;
    LeaveCriticalSection(&impl_->cs);
    SaveConfig(impl_->dirs, impl_->cfg);
    log::Info(L"DNS protection disabled");
    if (impl_->notify) PostMessageW(impl_->notify, WM_APP_REFRESH, 0, 0);
}

std::wstring Engine::RestoreOriginal() {
    if (!impl_) return L"";
    Impl::Baseline base;
    EnterCriticalSection(&impl_->cs);
    base = impl_->baseline;
    LeaveCriticalSection(&impl_->cs);

    if (base.empty()) {
        return L"No original DNS baseline is stored yet.\nEnable protection with "
               L"'Use my current system DNS settings' first so it can be captured.";
    }
    int count = 0;
    {
        EnterCriticalSection(&impl_->cs);
        for (const auto& f : impl_->Enumerate()) {
            const auto it = base.find(IfaceId(f.Key));
            if (it == base.end()) continue;
            impl_->WriteDnsTo(f, it->second.first, it->second.second);
            ++count;
        }
        LeaveCriticalSection(&impl_->cs);
    }
    FlushDnsCache();
    log::Info(Fmt(L"Original DNS settings restored on %d adapter(s)", count));
    if (impl_->notify) PostMessageW(impl_->notify, WM_APP_REFRESH, 0, 0);
    return Fmt(L"Original DNS settings were restored on %d adapter(s).", count);
}

}  // namespace dzp
