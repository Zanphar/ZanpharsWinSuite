// netgame.cpp - the Internet & networking page's engine.
//
// Four well-known Windows latency settings, each written only after its previous value
// has been saved under HKCU\Software\ZanpharsWinSuite\NetGaming. Restore reads those
// entries back, so it puts the machine where it was rather than guessing at defaults.
//
// It also measures round-trip time to a host of the user's choosing, which is how the
// page shows what the tweaks did instead of only claiming they help.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "extras.h"

#include <ipexport.h>
#include <icmpapi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core.h"

namespace {

const wchar_t* kJournalKey = L"Software\\ZanpharsWinSuite\\NetGaming";
const wchar_t* kInterfacesKey =
    L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces";
const wchar_t* kSystemProfileKey =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile";
const wchar_t* kGamesKey =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games";
const wchar_t* kPschedKey = L"SOFTWARE\\Policies\\Microsoft\\Windows\\Psched";

// A registry value the tweak wants to see. Everything but Nagle lives at a fixed path;
// Nagle is built per adapter when the tweak is resolved.
struct Target {
    HKEY root = HKEY_LOCAL_MACHINE;
    std::wstring key;
    std::wstring name;
    DWORD type = REG_DWORD;
    DWORD dword = 0;
    std::wstring text;
};

struct TweakDef {
    std::wstring id;
    std::wstring title;
    std::wstring summary;
    std::wstring writes;
};

struct Current {
    bool exists = false;
    DWORD type = 0;
    DWORD dword = 0;
    std::wstring text;
};

const std::vector<TweakDef>& Catalog() {
    static const std::vector<TweakDef> defs = {
        {L"nagle", L"Nagle's algorithm off",
         L"Small packets leave at once and acknowledgements are not delayed - lower "
         L"latency, a few more packets.",
         L"TcpAckFrequency=1, TCPNoDelay=1"},
        {L"throttle", L"Network throttling off",
         L"Stops Windows rationing the network stack for media and holding back CPU "
         L"time for background work.",
         L"NetworkThrottlingIndex=0xFFFFFFFF, SystemResponsiveness=0"},
        {L"games", L"Games task profile",
         L"Tells the multimedia scheduler a game is top priority, so its GPU and disk "
         L"queues are served first.",
         L"GPU Priority=8, Priority=6, Scheduling Category=High, SFIO Priority=High"},
        {L"qos", L"QoS reservation off",
         L"Removes the QoS scheduler's own slice of the link. Only matters when an app "
         L"asks for QoS.",
         L"NonBestEffortLimit=0"},
    };
    return defs;
}

const TweakDef* FindDef(const std::wstring& id) {
    for (const auto& def : Catalog())
        if (def.id == id) return &def;
    return nullptr;
}

std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

// --------------------------------------------------------------- raw values
// Reads a value with its type intact. A missing key or value is not an error: it comes
// back as exists=false, which is exactly what the journal needs to record.
bool ReadValue(HKEY root, const std::wstring& keyPath, const std::wstring& name, Current& out,
               DWORD& error) {
    out = Current();
    error = 0;
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(root, keyPath.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) return true;
    if (rc != ERROR_SUCCESS) {
        error = (DWORD)rc;
        return false;
    }

    DWORD type = 0, size = 0;
    rc = RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size);
    if (rc == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        return true;
    }
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(key);
        error = (DWORD)rc;
        return false;
    }

    std::vector<BYTE> data(size + sizeof(wchar_t), 0);
    rc = RegQueryValueExW(key, name.c_str(), nullptr, &type, data.data(), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        error = (DWORD)rc;
        return false;
    }

    out.exists = true;
    out.type = type;
    if (type == REG_DWORD) {
        if (size < sizeof(DWORD)) {
            out.exists = false;
            return true;
        }
        out.dword = *reinterpret_cast<const DWORD*>(data.data());
    } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
        out.text.assign(reinterpret_cast<const wchar_t*>(data.data()), size / sizeof(wchar_t));
        while (!out.text.empty() && out.text.back() == L'\0') out.text.pop_back();
    }
    return true;
}

bool WriteValue(const Target& target, std::wstring& error, DWORD& code) {
    error.clear();
    code = 0;
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(target.root, target.key.c_str(), 0, nullptr, 0,
                              KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        code = (DWORD)rc;
        error = WinErrorText(code);
        return false;
    }
    if (target.type == REG_DWORD) {
        rc = RegSetValueExW(key, target.name.c_str(), 0, REG_DWORD,
                            reinterpret_cast<const BYTE*>(&target.dword), sizeof(DWORD));
    } else {
        rc = RegSetValueExW(key, target.name.c_str(), 0, target.type,
                            reinterpret_cast<const BYTE*>(target.text.c_str()),
                            (DWORD)((target.text.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        code = (DWORD)rc;
        error = WinErrorText(code);
        return false;
    }
    return true;
}

// Removes a value that did not exist before the tweak ran. A key or value that is
// already gone counts as restored.
bool DeleteValue(HKEY root, const std::wstring& keyPath, const std::wstring& name,
                 std::wstring& error, DWORD& code) {
    error.clear();
    code = 0;
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(root, keyPath.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) return true;
    if (rc != ERROR_SUCCESS) {
        code = (DWORD)rc;
        error = WinErrorText(code);
        return false;
    }
    rc = RegDeleteValueW(key, name.c_str());
    RegCloseKey(key);
    if (rc == ERROR_FILE_NOT_FOUND) return true;
    if (rc != ERROR_SUCCESS) {
        code = (DWORD)rc;
        error = WinErrorText(code);
        return false;
    }
    return true;
}

bool SameValue(const Current& current, const Target& target) {
    if (!current.exists) return false;
    if (target.type == REG_DWORD) return current.type == REG_DWORD && current.dword == target.dword;
    if (current.type != REG_SZ && current.type != REG_EXPAND_SZ) return false;
    return current.text == target.text;
}

// ------------------------------------------------------------- the journal
// One string per saved value, named <tweak>|<root>|<key>|<value>. The data records the
// old value and its type, or ABSENT when there was no value at all.
std::wstring Encode(const Current& current) {
    if (!current.exists) return L"ABSENT";
    if (current.type == REG_DWORD) return L"DWORD:" + std::to_wstring(current.dword);
    if (current.type == REG_SZ) return L"SZ:" + current.text;
    if (current.type == REG_EXPAND_SZ) return L"EXPAND:" + current.text;
    return L"";  // a type this page does not know how to put back is left alone
}

bool JournalWrite(const std::wstring& entry, const std::wstring& encoded) {
    Target target;
    target.root = HKEY_CURRENT_USER;
    target.key = kJournalKey;
    target.name = entry;
    target.type = REG_SZ;
    target.text = encoded;
    std::wstring ignored;
    DWORD code = 0;
    return WriteValue(target, ignored, code);
}

bool JournalRead(const std::wstring& entry, std::wstring& encoded) {
    Current current;
    DWORD error = 0;
    if (!ReadValue(HKEY_CURRENT_USER, kJournalKey, entry, current, error) || !current.exists)
        return false;
    encoded = current.text;
    return true;
}

void JournalDelete(const std::wstring& entry) {
    std::wstring ignored;
    DWORD code = 0;
    DeleteValue(HKEY_CURRENT_USER, kJournalKey, entry, ignored, code);
}

std::vector<std::wstring> JournalNames(const std::wstring& prefix) {
    std::vector<std::wstring> names;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kJournalKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS)
        return names;
    for (DWORD index = 0;; ++index) {
        wchar_t name[1024] = {};
        DWORD size = 1023;
        DWORD type = 0;
        LONG rc = RegEnumValueW(key, index, name, &size, nullptr, &type, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        std::wstring value(name, size);
        if (value.rfind(prefix, 0) == 0) names.push_back(value);
    }
    RegCloseKey(key);
    return names;
}

std::wstring EntryName(const std::wstring& id, const Target& target) {
    return id + L"|" + (target.root == HKEY_LOCAL_MACHINE ? L"HKLM" : L"HKCU") + L"|" +
           target.key + L"|" + target.name;
}

bool RestoreEncoded(const Target& target, const std::wstring& encoded, std::wstring& error,
                    DWORD& code) {
    if (encoded == L"ABSENT")
        return DeleteValue(target.root, target.key, target.name, error, code);
    if (encoded.rfind(L"DWORD:", 0) == 0) {
        Target value = target;
        value.type = REG_DWORD;
        value.dword = (DWORD)wcstoul(encoded.c_str() + 6, nullptr, 10);
        return WriteValue(value, error, code);
    }
    if (encoded.rfind(L"SZ:", 0) == 0) {
        Target value = target;
        value.type = REG_SZ;
        value.text = encoded.substr(3);
        return WriteValue(value, error, code);
    }
    if (encoded.rfind(L"EXPAND:", 0) == 0) {
        Target value = target;
        value.type = REG_EXPAND_SZ;
        value.text = encoded.substr(7);
        return WriteValue(value, error, code);
    }
    error = L"unreadable journal entry";
    code = ERROR_INVALID_DATA;
    return false;
}

struct RestoreStats {
    int restored = 0;
    int failed = 0;
    std::wstring firstError;
    bool adminNeeded = false;
};

// Puts back every value saved under one prefix (a tweak id). The entries are removed
// only after their value is back, so a refused write can be retried later.
RestoreStats RestoreEntries(const std::wstring& prefix) {
    RestoreStats stats;
    for (const auto& entry : JournalNames(prefix)) {
        std::wstring encoded;
        if (!JournalRead(entry, encoded)) {
            ++stats.failed;
            if (stats.firstError.empty()) stats.firstError = L"an entry could not be read";
            continue;
        }
        size_t first = entry.find(L'|');
        size_t second = first == std::wstring::npos ? std::wstring::npos : entry.find(L'|', first + 1);
        size_t last = entry.rfind(L'|');
        if (first == std::wstring::npos || second == std::wstring::npos ||
            last == std::wstring::npos || last <= second) {
            JournalDelete(entry);  // not one of ours; do not leave it in the way
            continue;
        }
        Target target;
        target.root = entry.substr(first + 1, second - first - 1) == L"HKLM" ? HKEY_LOCAL_MACHINE
                                                                             : HKEY_CURRENT_USER;
        target.key = entry.substr(second + 1, last - second - 1);
        target.name = entry.substr(last + 1);

        std::wstring error;
        DWORD code = 0;
        if (RestoreEncoded(target, encoded, error, code)) {
            JournalDelete(entry);
            ++stats.restored;
        } else {
            ++stats.failed;
            if (code == ERROR_ACCESS_DENIED) stats.adminNeeded = true;
            if (stats.firstError.empty())
                stats.firstError = target.name + L": " + (error.empty() ? WinErrorText(code) : error);
        }
    }
    return stats;
}

// -------------------------------------------------------------- the targets
void AddDword(std::vector<Target>& targets, const wchar_t* key, const wchar_t* name, DWORD value) {
    Target target;
    target.key = key;
    target.name = name;
    target.type = REG_DWORD;
    target.dword = value;
    targets.push_back(target);
}

void AddText(std::vector<Target>& targets, const wchar_t* key, const wchar_t* name,
             const wchar_t* text) {
    Target target;
    target.key = key;
    target.name = name;
    target.type = REG_SZ;
    target.text = text;
    targets.push_back(target);
}

// Interfaces that actually hold an IPv4 configuration. The pseudo-interfaces Windows
// keeps for tunnels and loopback are left alone.
std::vector<std::wstring> NagleInterfaces() {
    std::vector<std::wstring> guids;
    HKEY interfaces = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kInterfacesKey, 0, KEY_READ | KEY_WOW64_64KEY,
                      &interfaces) != ERROR_SUCCESS)
        return guids;

    for (DWORD index = 0;; ++index) {
        wchar_t guid[256] = {};
        DWORD size = 255;
        LONG rc = RegEnumKeyExW(interfaces, index, guid, &size, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;

        std::wstring key = std::wstring(kInterfacesKey) + L"\\" + guid;
        Current dhcp;
        Current fixedAddress;
        DWORD error = 0;
        bool hasDhcp = ReadValue(HKEY_LOCAL_MACHINE, key, L"DhcpIPAddress", dhcp, error) &&
                       dhcp.exists && !dhcp.text.empty() && dhcp.text != L"0.0.0.0";
        bool hasFixed = ReadValue(HKEY_LOCAL_MACHINE, key, L"IPAddress", fixedAddress, error) &&
                        fixedAddress.exists;
        if (hasDhcp || hasFixed) guids.push_back(guid);
    }
    RegCloseKey(interfaces);
    std::sort(guids.begin(), guids.end());
    return guids;
}

std::vector<Target> TargetsFor(const std::wstring& id) {
    std::vector<Target> targets;
    if (id == L"nagle") {
        for (const auto& guid : NagleInterfaces()) {
            const std::wstring key = std::wstring(kInterfacesKey) + L"\\" + guid;
            AddDword(targets, key.c_str(), L"TcpAckFrequency", 1);
            AddDword(targets, key.c_str(), L"TCPNoDelay", 1);
        }
    } else if (id == L"throttle") {
        AddDword(targets, kSystemProfileKey, L"NetworkThrottlingIndex", 0xFFFFFFFF);
        AddDword(targets, kSystemProfileKey, L"SystemResponsiveness", 0);
    } else if (id == L"games") {
        AddDword(targets, kGamesKey, L"GPU Priority", 8);
        AddDword(targets, kGamesKey, L"Priority", 6);
        AddText(targets, kGamesKey, L"Scheduling Category", L"High");
        AddText(targets, kGamesKey, L"SFIO Priority", L"High");
    } else if (id == L"qos") {
        AddDword(targets, kPschedKey, L"NonBestEffortLimit", 0);
    }
    return targets;
}

// ------------------------------------------------------------- one tweak
NetOpResult ApplyOne(const TweakDef& def) {
    NetOpResult result;
    std::vector<Target> targets = TargetsFor(def.id);
    if (targets.empty()) {
        result.ok = false;
        result.message = def.title + L": no configured adapter was found to change.";
        return result;
    }

    for (const auto& target : targets) {
        Current current;
        DWORD error = 0;
        if (!ReadValue(target.root, target.key, target.name, current, error)) {
            ++result.skipped;
            result.ok = false;
            if (result.message.empty())
                result.message = target.name + L": " + WinErrorText(error);
            continue;
        }
        if (SameValue(current, target)) {
            ++result.skipped;
            continue;
        }
        // Unsupported types (binary or multi-string) are never overwritten: the page
        // could not put them back.
        std::wstring encoded = Encode(current);
        if (encoded.empty()) {
            ++result.skipped;
            result.ok = false;
            if (result.message.empty()) result.message = target.name + L": unsupported value type";
            continue;
        }

        const std::wstring entry = EntryName(def.id, target);
        std::wstring existing;
        const bool hadEntry = JournalRead(entry, existing);
        if (!hadEntry && !JournalWrite(entry, encoded)) {
            ++result.skipped;
            result.ok = false;
            if (result.message.empty())
                result.message = target.name + L": the old value could not be saved";
            continue;
        }

        std::wstring writeError;
        DWORD writeCode = 0;
        if (!WriteValue(target, writeError, writeCode)) {
            if (!hadEntry) JournalDelete(entry);  // nothing changed, nothing to undo
            ++result.skipped;
            result.ok = false;
            if (writeCode == ERROR_ACCESS_DENIED) result.adminNeeded = true;
            if (result.message.empty())
                result.message = target.name + L": " +
                                 (writeError.empty() ? WinErrorText(writeCode) : writeError);
            continue;
        }
        ++result.changed;
    }

    if (result.ok && result.changed == 0)
        result.message = def.title + L": already in gaming shape.";
    else if (result.ok)
        result.message = def.title + L": " + Plural(result.changed, L"value", L"values") +
                         L" set. A restart makes Windows read them.";
    return result;
}

// Switching off a value that was already in gaming shape when WinSuite first looked at
// it. The value is journalled first and then removed, so Windows is back at its own
// default and Revert still has exactly what to put back - no "off" value is guessed at,
// and a value another tool chose is never lost.
NetOpResult TurnOffOne(const TweakDef& def) {
    NetOpResult result;
    std::vector<Target> targets = TargetsFor(def.id);
    if (targets.empty()) {
        result.ok = false;
        result.message = def.title + L": no configured adapter was found to change.";
        return result;
    }

    for (const auto& target : targets) {
        Current current;
        DWORD error = 0;
        if (!ReadValue(target.root, target.key, target.name, current, error)) {
            ++result.skipped;
            result.ok = false;
            if (result.message.empty())
                result.message = target.name + L": " + WinErrorText(error);
            continue;
        }
        if (!current.exists) {
            ++result.skipped;  // already at the Windows default
            continue;
        }
        std::wstring encoded = Encode(current);
        if (encoded.empty()) {
            ++result.skipped;
            result.ok = false;
            if (result.message.empty()) result.message = target.name + L": unsupported value type";
            continue;
        }

        const std::wstring entry = EntryName(def.id, target);
        std::wstring existing;
        const bool hadEntry = JournalRead(entry, existing);
        if (!hadEntry && !JournalWrite(entry, encoded)) {
            ++result.skipped;
            result.ok = false;
            if (result.message.empty())
                result.message = target.name + L": the old value could not be saved";
            continue;
        }

        std::wstring deleteError;
        DWORD deleteCode = 0;
        if (!DeleteValue(target.root, target.key, target.name, deleteError, deleteCode)) {
            if (!hadEntry) JournalDelete(entry);
            ++result.skipped;
            result.ok = false;
            if (deleteCode == ERROR_ACCESS_DENIED) result.adminNeeded = true;
            if (result.message.empty())
                result.message = target.name + L": " +
                                 (deleteError.empty() ? WinErrorText(deleteCode) : deleteError);
            continue;
        }
        ++result.changed;
    }

    if (result.ok && result.changed == 0)
        result.message = def.title + L": already at the Windows default.";
    else if (result.ok)
        result.message = def.title + L": " + Plural(result.changed, L"value", L"values") +
                         L" back to the Windows default. Revert restores what was there.";
    else if (result.changed > 0)
        result.message = def.title + L": " + Plural(result.changed, L"value", L"values") +
                         L" reset; " + result.message;
    return result;
}

NetOpResult RevertOne(const TweakDef& def) {
    NetOpResult result;
    RestoreStats stats = RestoreEntries(def.id + L"|");
    result.changed = (uint32_t)stats.restored;
    result.skipped = (uint32_t)stats.failed;
    result.adminNeeded = stats.adminNeeded;
    result.ok = stats.failed == 0;
    if (stats.restored == 0 && stats.failed == 0) {
        result.message = def.title + L": nothing was saved here, so nothing was put back.";
    } else if (stats.failed > 0) {
        result.message = def.title + L": " + Plural((size_t)stats.restored, L"value", L"values") +
                         L" put back, " + std::to_wstring(stats.failed) + L" refused - " +
                         stats.firstError;
    } else {
        result.message = def.title + L": " + Plural((size_t)stats.restored, L"value", L"values") +
                         L" put back. A restart makes Windows read them again.";
    }
    return result;
}

}  // namespace

// ------------------------------------------------------------------- public
std::vector<NetTweakView> LoadNetTweaks() {
    std::vector<NetTweakView> views;
    for (const auto& def : Catalog()) {
        NetTweakView view;
        view.id = def.id;
        view.title = def.title;
        view.summary = def.summary;
        view.writes = def.writes;

        std::vector<Target> targets = TargetsFor(def.id);
        view.targetCount = (int)targets.size();
        view.available = !targets.empty();

        int matching = 0;
        for (const auto& target : targets) {
            Current current;
            DWORD error = 0;
            if (!ReadValue(target.root, target.key, target.name, current, error)) {
                view.available = false;
                view.state = target.name + L": " + WinErrorText(error);
                break;
            }
            if (SameValue(current, target)) ++matching;
        }
        view.applied = view.available && matching == (int)targets.size();
        view.journaled = !JournalNames(def.id + L"|").empty();

        if (!view.available && view.state.empty()) {
            view.state = def.id == L"nagle" ? L"No configured adapter found"
                                            : L"Nothing to change on this PC";
        } else if (view.available) {
            if (def.id == L"nagle")
                view.state = Plural(targets.size() / 2, L"adapter", L"adapters");
            else
                view.state = Plural(targets.size(), L"value", L"values");
            if (view.applied && !view.journaled)
                view.state += L" - all in gaming shape, set elsewhere";
            else if (view.applied)
                view.state += L" - all in gaming shape";
            else if (view.journaled)
                view.state += L" - changed here, revert available";
            else
                view.state += L" - Windows defaults";
        }
        views.push_back(view);
    }
    return views;
}

NetOpResult ApplyNetTweak(const std::wstring& id) {
    const TweakDef* def = FindDef(id);
    if (!def) {
        NetOpResult result;
        result.ok = false;
        result.message = L"Unknown tweak.";
        return result;
    }
    return ApplyOne(*def);
}

NetOpResult TurnOffNetTweak(const std::wstring& id) {
    const TweakDef* def = FindDef(id);
    if (!def) {
        NetOpResult result;
        result.ok = false;
        result.message = L"Unknown tweak.";
        return result;
    }
    return TurnOffOne(*def);
}

NetOpResult RevertNetTweak(const std::wstring& id) {
    const TweakDef* def = FindDef(id);
    if (!def) {
        NetOpResult result;
        result.ok = false;
        result.message = L"Unknown tweak.";
        return result;
    }
    return RevertOne(*def);
}

NetOpResult ApplyNetGamingProfile() {
    NetOpResult result;
    int tweaksChanged = 0;
    for (const auto& def : Catalog()) {
        NetOpResult one = ApplyOne(def);
        result.changed += one.changed;
        result.skipped += one.skipped;
        result.adminNeeded = result.adminNeeded || one.adminNeeded;
        if (one.changed > 0) ++tweaksChanged;
        if (!one.ok) {
            result.ok = false;
            if (result.message.empty()) result.message = one.message;
        }
    }

    std::wstring headline;
    if (result.changed > 0)
        headline = Plural(result.changed, L"value", L"values") + L" set across " +
                   Plural((size_t)tweaksChanged, L"tweak", L"tweaks") +
                   L". A restart makes Windows read them.";
    else if (result.ok)
        headline = L"The gaming profile was already in place - nothing needed changing.";
    else
        headline = L"Nothing could be changed.";

    result.message = result.ok ? headline : headline + L" " + result.message;
    return result;
}

// One press that switches off every tweak, rather than the four presses the rows need.
// Each value is saved to the journal before it is cleared, exactly like a single Turn off,
// so Revert all can still put back what a tweak had before this suite touched it.
NetOpResult RestoreNetDefaults() {
    NetOpResult result;
    int tweaksReset = 0;
    for (const auto& def : Catalog()) {
        // A tweak with nothing on this PC to change is not a failure: Nagle has no values
        // when no adapter is configured, and that must not make the press look broken.
        if (TargetsFor(def.id).empty()) continue;
        NetOpResult one = TurnOffOne(def);
        result.changed += one.changed;
        result.skipped += one.skipped;
        result.adminNeeded = result.adminNeeded || one.adminNeeded;
        if (one.changed > 0) ++tweaksReset;
        if (!one.ok) {
            result.ok = false;
            if (result.message.empty()) result.message = one.message;
        }
    }

    std::wstring headline;
    if (result.changed > 0)
        headline = Plural(result.changed, L"value", L"values") + L" reset across " +
                   Plural((size_t)tweaksReset, L"tweak", L"tweaks") +
                   L" - Windows' own defaults are in force again. Revert all puts back what "
                   L"was there before.";
    else if (result.ok)
        headline = L"All four tweaks were already at the Windows default.";
    else
        headline = L"Nothing could be reset.";

    result.message = result.ok ? headline : headline + L" " + result.message;
    return result;
}

NetOpResult RevertNetGamingProfile() {
    NetOpResult result;
    for (const auto& def : Catalog()) {
        RestoreStats stats = RestoreEntries(def.id + L"|");
        result.changed += (uint32_t)stats.restored;
        result.skipped += (uint32_t)stats.failed;
        result.adminNeeded = result.adminNeeded || stats.adminNeeded;
        if (stats.failed > 0) {
            result.ok = false;
            if (result.message.empty()) result.message = stats.firstError;
        }
    }

    if (result.changed == 0 && result.ok)
        result.message = L"Nothing here was changed by WinSuite, so there is nothing to put back.";
    else if (result.ok)
        result.message = Plural(result.changed, L"value", L"values") +
                         L" put back. A restart makes Windows read them again.";
    else
        result.message = Plural(result.changed, L"value", L"values") + L" put back; " +
                         result.message;
    return result;
}

bool NetJournalRoundTrip(std::wstring& detail) {
    const std::wstring testKey = L"Software\\ZanpharsWinSuite\\SelfTest\\NetGaming";
    const std::wstring probe = L"Probe";
    RegDeleteTreeW(HKEY_CURRENT_USER, testKey.c_str());

    Target original;
    original.root = HKEY_CURRENT_USER;
    original.key = testKey;
    original.name = probe;
    original.type = REG_DWORD;
    original.dword = 4242;
    std::wstring error;
    DWORD code = 0;
    if (!WriteValue(original, error, code)) {
        detail = error;
        return false;
    }

    Current saved;
    if (!ReadValue(HKEY_CURRENT_USER, testKey, probe, saved, code) || !saved.exists) {
        detail = L"the test value could not be read back";
        RegDeleteTreeW(HKEY_CURRENT_USER, testKey.c_str());
        return false;
    }
    const std::wstring entry = EntryName(L"selftest", original);
    if (!JournalWrite(entry, Encode(saved))) {
        detail = L"the journal entry could not be written";
        RegDeleteTreeW(HKEY_CURRENT_USER, testKey.c_str());
        return false;
    }
    Target gaming = original;
    gaming.dword = 1;
    if (!WriteValue(gaming, error, code)) {
        detail = L"the test value could not be changed";
        RegDeleteTreeW(HKEY_CURRENT_USER, testKey.c_str());
        return false;
    }

    RestoreStats stats = RestoreEntries(L"selftest|");
    Current after;
    const bool readBack =
        ReadValue(HKEY_CURRENT_USER, testKey, probe, after, code) && after.exists &&
        after.type == REG_DWORD && after.dword == 4242;
    const bool clean = JournalNames(L"selftest|").empty();
    RegDeleteTreeW(HKEY_CURRENT_USER, testKey.c_str());

    bool ok = stats.restored == 1 && stats.failed == 0 && readBack && clean;
    detail = ok ? L"4242 -> 1 -> 4242, journal empty"
                : L"restored " + std::to_wstring(stats.restored) + L", value back: " +
                      (readBack ? L"yes" : L"no") + L", journal clear: " +
                      (clean ? L"yes" : L"no");
    return ok;
}

// ------------------------------------------------------------ latency check
// Four echoes with a one-second timeout, resolved over IPv4. The page keeps one result
// from before the tweaks and one from after, and compares their averages. Nothing here
// depends on the UI, so the page runs it on a worker and reports back by message.
LatencyResult MeasureLatency(const std::wstring& host, int pings) {
    LatencyResult out;
    out.host = host;
    if (pings < 1) pings = 1;
    if (pings > 10) pings = 10;

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        out.error = L"Winsock could not start.";
        return out;
    }

    ADDRINFOW hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* info = nullptr;
    if (GetAddrInfoW(host.c_str(), nullptr, &hints, &info) != 0 || !info) {
        out.error = L"\"" + host + L"\" did not resolve to an IPv4 address.";
        if (info) FreeAddrInfoW(info);
        WSACleanup();
        return out;
    }
    const IPAddr address = ((sockaddr_in*)info->ai_addr)->sin_addr.S_un.S_addr;
    FreeAddrInfoW(info);

    wchar_t text[64] = {};
    IN_ADDR in = {};
    in.S_un.S_addr = address;
    if (InetNtopW(AF_INET, &in, text, 64)) out.address = text;

    HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) {
        out.error = L"ICMP could not be opened: " + WinErrorText(GetLastError());
        WSACleanup();
        return out;
    }

    const WORD payload = 16;
    std::vector<char> request(payload, 'z');
    std::vector<unsigned char> reply(sizeof(ICMP_ECHO_REPLY) + payload + 8);
    int received = 0, minMs = 0, maxMs = 0;
    long long total = 0;
    for (int i = 0; i < pings; ++i) {
        DWORD replies = IcmpSendEcho(icmp, address, request.data(), payload, nullptr,
                                     reply.data(), (DWORD)reply.size(), 1000);
        if (replies > 0) {
            const ICMP_ECHO_REPLY* echo = (const ICMP_ECHO_REPLY*)reply.data();
            if (echo->Status == IP_SUCCESS) {
                const int ms = (int)echo->RoundTripTime;
                if (received == 0 || ms < minMs) minMs = ms;
                if (ms > maxMs) maxMs = ms;
                total += ms;
                ++received;
            }
        }
        if (i + 1 < pings) Sleep(120);
    }
    IcmpCloseHandle(icmp);
    WSACleanup();

    out.sent = pings;
    out.received = received;
    if (received == 0) {
        out.error = L"No reply from " + out.address + L" - the host may be down or block pings.";
        return out;
    }

    out.minMs = minMs;
    out.maxMs = maxMs;
    out.avgMs = (int)(total / received);
    out.ok = true;
    return out;
}

// The figure the before/after pair exists for: both averages as one span, "11 ms to
// 14 ms", with the difference beside it. Packet loss only gets a mention when it moved,
// because a note that it did not change is noise on a status line.
std::wstring LatencyComparison(const LatencyResult& before, const LatencyResult& after) {
    if (!before.ok || !after.ok) return std::wstring();

    std::wstring text = std::to_wstring(before.avgMs) + L" ms to " +
                        std::to_wstring(after.avgMs) + L" ms";
    const int delta = after.avgMs - before.avgMs;
    if (delta < 0)
        text += L" - " + std::to_wstring(-delta) + L" ms lower on average";
    else if (delta > 0)
        text += L" - " + std::to_wstring(delta) + L" ms higher on average";
    else
        text += L" - no change in the average";

    const int lossBefore =
        before.sent > 0 ? (before.sent - before.received) * 100 / before.sent : 0;
    const int lossAfter = after.sent > 0 ? (after.sent - after.received) * 100 / after.sent : 0;
    if (lossBefore != lossAfter)
        text += lossAfter < lossBefore ? L" - less packet loss too" : L" - but more packet loss";
    return text;
}
