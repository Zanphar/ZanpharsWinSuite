// gameboost.cpp - the Game Booster page's tweak engine.
//
// The system profile (power plan, parked services, Game DVR) still lives in boost.cpp;
// this file covers everything else. Each value is journalled with its type, or recorded
// as absent, before it is written, so a revert puts back exactly what was there. The
// advanced entries are deliberately individual-only: the one-click profile never touches
// them.
#include "extras.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "core.h"

namespace {

const wchar_t* kJournalKey = L"Software\\ZanpharsWinSuite\\GameBooster";
const wchar_t* kMouseKey = L"Control Panel\\Mouse";
const wchar_t* kWindowMetricsKey = L"Control Panel\\Desktop\\WindowMetrics";
const wchar_t* kPersonalizeKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
const wchar_t* kGameBarKey = L"Software\\Microsoft\\GameBar";
const wchar_t* kGameConfigKey = L"System\\GameConfigStore";
const wchar_t* kGraphicsKey = L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers";
const wchar_t* kThrottlingKey = L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling";
const wchar_t* kPriorityKey = L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl";

struct Target {
    HKEY root = HKEY_CURRENT_USER;
    std::wstring key;
    std::wstring name;
    DWORD type = REG_DWORD;
    DWORD dword = 0;
    std::wstring text;
    // Some settings are on unless Windows is told otherwise, so a missing value means the
    // "on" state and switching off has to write an explicit off value. Deleting those
    // would leave the feature running while the page reported it off.
    bool absentIsOn = false;
    bool hasOff = false;
    DWORD offDword = 0;
};

struct Current {
    bool exists = false;
    DWORD type = 0;
    DWORD dword = 0;
    std::wstring text;
};

struct Def {
    std::wstring id;
    std::wstring title;
    std::wstring summary;
    std::wstring how;      // when the change takes hold
    bool inProfile = false;  // the safe set the profile button applies
    std::vector<Target> targets;
};

void AddDword(std::vector<Target>& out, HKEY root, const wchar_t* key, const wchar_t* name,
              DWORD value) {
    Target target;
    target.root = root;
    target.key = key;
    target.name = name;
    target.type = REG_DWORD;
    target.dword = value;
    out.push_back(target);
}

void AddText(std::vector<Target>& out, HKEY root, const wchar_t* key, const wchar_t* name,
             const wchar_t* text) {
    Target target;
    target.root = root;
    target.key = key;
    target.name = name;
    target.type = REG_SZ;
    target.text = text;
    out.push_back(target);
}

std::vector<Target> TargetsFor(const std::wstring& id) {
    std::vector<Target> targets;
    if (id == L"gamemode") {
        AddDword(targets, HKEY_CURRENT_USER, kGameBarKey, L"AutoGameModeEnabled", 1);
        AddDword(targets, HKEY_CURRENT_USER, kGameBarKey, L"AllowAutoGameMode", 1);
        // Game Mode is on out of the box, so no value means on and switching it off has to
        // write 0 rather than remove the value.
        for (auto& target : targets) {
            target.absentIsOn = true;
            target.hasOff = true;
            target.offDword = 0;
        }
    } else if (id == L"mouseaccel") {
        AddText(targets, HKEY_CURRENT_USER, kMouseKey, L"MouseSpeed", L"0");
        AddText(targets, HKEY_CURRENT_USER, kMouseKey, L"MouseThreshold1", L"0");
        AddText(targets, HKEY_CURRENT_USER, kMouseKey, L"MouseThreshold2", L"0");
    } else if (id == L"fullscreen") {
        AddDword(targets, HKEY_CURRENT_USER, kGameConfigKey, L"GameDVR_FSEBehaviorMode", 2);
        AddDword(targets, HKEY_CURRENT_USER, kGameConfigKey, L"GameDVR_HonorUserFSEBehaviorMode", 1);
        AddDword(targets, HKEY_CURRENT_USER, kGameConfigKey,
                 L"GameDVR_DXGIHonorFSEWindowsCompatible", 1);
        AddDword(targets, HKEY_CURRENT_USER, kGameConfigKey, L"GameDVR_EFSEFeatureFlags", 0);
    } else if (id == L"visuals") {
        AddText(targets, HKEY_CURRENT_USER, kWindowMetricsKey, L"MinAnimate", L"0");
        AddDword(targets, HKEY_CURRENT_USER, kPersonalizeKey, L"EnableTransparency", 0);
    } else if (id == L"hags") {
        AddDword(targets, HKEY_LOCAL_MACHINE, kGraphicsKey, L"HwSchMode", 2);
    } else if (id == L"powerthrottle") {
        AddDword(targets, HKEY_LOCAL_MACHINE, kThrottlingKey, L"PowerThrottlingOff", 1);
    } else if (id == L"priority") {
        AddDword(targets, HKEY_LOCAL_MACHINE, kPriorityKey, L"Win32PrioritySeparation", 38);
    }
    return targets;
}

const std::vector<Def>& Catalog() {
    static const std::vector<Def> defs = {
        {L"gamemode", L"Game Mode on",
         L"Holds background work and driver installs while a game runs.",
         L"Next game launch", true, TargetsFor(L"gamemode")},
        {L"mouseaccel", L"Mouse acceleration off",
         L"1:1 pointer movement. Games on raw input already ignore it.",
         L"Applies immediately", true, TargetsFor(L"mouseaccel")},
        {L"fullscreen", L"Fullscreen optimisations off",
         L"Exclusive fullscreen instead of the borderless flip model; alt-tab may slow.",
         L"Next launch of a game", true, TargetsFor(L"fullscreen")},
        {L"visuals", L"Visual effects off",
         L"No window animations or transparency. Transparency returns at next sign-in.",
         L"Animations now, transparency next sign-in", true, TargetsFor(L"visuals")},
        {L"hags", L"GPU scheduling (HAGS)",
         L"The GPU schedules its own work. Needs a supported GPU and a reboot; can help or "
         L"hurt frame pacing.",
         L"Needs a restart", false, TargetsFor(L"hags")},
        {L"powerthrottle", L"Power throttling off",
         L"Stops Windows capping background CPU. More power draw and heat; needs a restart.",
         L"Needs a restart", false, TargetsFor(L"powerthrottle")},
        {L"priority", L"Win32 priority separation 38",
         L"Longer fixed CPU slices for games. The benefit is disputed; revert if it feels "
         L"worse.",
         L"Needs a restart", false, TargetsFor(L"priority")},
    };
    return defs;
}

const Def* FindDef(const std::wstring& id) {
    for (const auto& def : Catalog())
        if (def.id == id) return &def;
    return nullptr;
}

std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

// --------------------------------------------------------------- raw values
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
    if (!current.exists) return target.absentIsOn;
    if (target.type == REG_DWORD) return current.type == REG_DWORD && current.dword == target.dword;
    if (current.type != REG_SZ && current.type != REG_EXPAND_SZ) return false;
    return current.text == target.text;
}

// ------------------------------------------------------------- the journal
std::wstring Encode(const Current& current) {
    if (!current.exists) return L"ABSENT";
    if (current.type == REG_DWORD) return L"DWORD:" + std::to_wstring(current.dword);
    if (current.type == REG_SZ) return L"SZ:" + current.text;
    if (current.type == REG_EXPAND_SZ) return L"EXPAND:" + current.text;
    return L"";
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
        size_t second =
            first == std::wstring::npos ? std::wstring::npos : entry.find(L'|', first + 1);
        size_t last = entry.rfind(L'|');
        if (first == std::wstring::npos || second == std::wstring::npos ||
            last == std::wstring::npos || last <= second) {
            JournalDelete(entry);
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

// --------------------------------------------------- immediate side effects
// Windows caches pointer acceleration and window animation in the session, so writing the
// registry alone would not be felt until the next sign-in. Both are safe to broadcast.
void SyncMouseAcceleration() {
    Current threshold1, threshold2, speed;
    DWORD error = 0;
    int values[3] = {0, 0, 0};
    if (ReadValue(HKEY_CURRENT_USER, kMouseKey, L"MouseThreshold1", threshold1, error) &&
        threshold1.exists)
        values[0] = _wtoi(threshold1.text.c_str());
    if (ReadValue(HKEY_CURRENT_USER, kMouseKey, L"MouseThreshold2", threshold2, error) &&
        threshold2.exists)
        values[1] = _wtoi(threshold2.text.c_str());
    if (ReadValue(HKEY_CURRENT_USER, kMouseKey, L"MouseSpeed", speed, error) && speed.exists)
        values[2] = _wtoi(speed.text.c_str());
    SystemParametersInfoW(SPI_SETMOUSE, 0, values, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

void SyncWindowAnimation() {
    Current animate;
    DWORD error = 0;
    bool on = true;
    if (ReadValue(HKEY_CURRENT_USER, kWindowMetricsKey, L"MinAnimate", animate, error) &&
        animate.exists)
        on = _wtoi(animate.text.c_str()) != 0;
    SystemParametersInfoW(SPI_SETANIMATION, 0, &on, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet",
                        SMTO_ABORTIFHUNG, 200, &ignored);
}

int CountApplied(const Def& def, bool* available) {
    std::vector<Target> targets = def.targets;
    if (available) *available = !targets.empty();
    int matching = 0;
    for (const auto& target : targets) {
        Current current;
        DWORD error = 0;
        if (!ReadValue(target.root, target.key, target.name, current, error)) continue;
        if (SameValue(current, target)) ++matching;
    }
    return matching;
}

NetOpResult ApplyOne(const Def& def) {
    NetOpResult result;
    if (def.targets.empty()) {
        result.ok = false;
        result.message = def.title + L": nothing to change.";
        return result;
    }
    for (const auto& target : def.targets) {
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
            if (!hadEntry) JournalDelete(entry);
            ++result.skipped;
            result.ok = false;
            if (writeCode == ERROR_ACCESS_DENIED) result.adminNeeded = true;
            if (result.message.empty())
                result.message =
                    target.name + L": " + (writeError.empty() ? WinErrorText(writeCode) : writeError);
            continue;
        }
        ++result.changed;
    }

    if (result.changed > 0 && def.id == L"mouseaccel") SyncMouseAcceleration();
    if (result.changed > 0 && def.id == L"visuals") SyncWindowAnimation();

    if (result.ok && result.changed == 0)
        result.message = def.title + L": already in gaming shape.";
    else if (result.ok)
        result.message = def.title + L": " + Plural(result.changed, L"value", L"values") +
                         L" set.";
    return result;
}

// Switching off a value that was already in place when WinSuite first looked at it.
// The value is journalled first, then either removed - so Windows is back at its own
// default - or set to the row's explicit off value when the default is "on". Revert puts
// back exactly what was there either way.
NetOpResult TurnOffOne(const Def& def) {
    NetOpResult result;
    if (def.targets.empty()) {
        result.ok = false;
        result.message = def.title + L": nothing to change.";
        return result;
    }
    int writtenOff = 0;
    for (const auto& target : def.targets) {
        Current current;
        DWORD error = 0;
        if (!ReadValue(target.root, target.key, target.name, current, error)) {
            ++result.skipped;
            result.ok = false;
            if (result.message.empty())
                result.message = target.name + L": " + WinErrorText(error);
            continue;
        }
        if (!current.exists && !target.hasOff) {
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
        std::wstring writeError;
        DWORD writeCode = 0;
        bool written = false;
        if (target.hasOff) {
            Target off = target;
            off.type = REG_DWORD;
            off.dword = target.offDword;
            off.text.clear();
            written = WriteValue(off, writeError, writeCode);
            if (written) ++writtenOff;
        } else {
            written = DeleteValue(target.root, target.key, target.name, writeError, writeCode);
        }
        if (!written) {
            if (!hadEntry) JournalDelete(entry);
            ++result.skipped;
            result.ok = false;
            if (writeCode == ERROR_ACCESS_DENIED) result.adminNeeded = true;
            if (result.message.empty())
                result.message =
                    target.name + L": " + (writeError.empty() ? WinErrorText(writeCode) : writeError);
            continue;
        }
        ++result.changed;
    }

    if (result.changed > 0 && def.id == L"mouseaccel") SyncMouseAcceleration();
    if (result.changed > 0 && def.id == L"visuals") SyncWindowAnimation();

    if (result.ok && result.changed == 0)
        result.message = def.title + L": already at the Windows default.";
    else if (result.ok)
        result.message = def.title + L": " + Plural(result.changed, L"value", L"values") +
                         (writtenOff > 0 ? L" switched off."
                                         : L" back to the Windows default.");
    else if (result.changed > 0)
        result.message = def.title + L": " + Plural(result.changed, L"value", L"values") +
                         L" reset; " + result.message;
    return result;
}

NetOpResult RevertOne(const Def& def) {
    NetOpResult result;
    RestoreStats stats = RestoreEntries(def.id + L"|");
    result.changed = (uint32_t)stats.restored;
    result.skipped = (uint32_t)stats.failed;
    result.adminNeeded = stats.adminNeeded;
    result.ok = stats.failed == 0;
    if (def.id == L"mouseaccel") SyncMouseAcceleration();
    if (def.id == L"visuals") SyncWindowAnimation();

    if (stats.restored == 0 && stats.failed == 0)
        result.message = def.title + L": nothing was saved here, so nothing was put back.";
    else if (stats.failed > 0)
        result.message = def.title + L": " + Plural((size_t)stats.restored, L"value", L"values") +
                         L" put back, " + std::to_wstring(stats.failed) + L" refused - " +
                         stats.firstError;
    else
        result.message = def.title + L": " + Plural((size_t)stats.restored, L"value", L"values") +
                         L" put back.";
    return result;
}

}  // namespace

// ------------------------------------------------------------------- public
std::vector<GameTweakView> LoadGameTweaks() {
    std::vector<GameTweakView> views;
    for (const auto& def : Catalog()) {
        GameTweakView view;
        view.id = def.id;
        view.title = def.title;
        view.summary = def.summary;
        view.how = def.how;
        view.inProfile = def.inProfile;
        view.valueCount = (int)def.targets.size();
        for (const auto& target : def.targets)
            if (target.root == HKEY_LOCAL_MACHINE) view.needsAdmin = true;

        bool available = true;
        int matching = CountApplied(def, &available);
        view.available = available;
        view.applied = available && matching == (int)def.targets.size();
        view.journaled = !JournalNames(def.id + L"|").empty();
        views.push_back(view);
    }
    return views;
}

NetOpResult ApplyGameTweak(const std::wstring& id) {
    const Def* def = FindDef(id);
    if (!def) {
        NetOpResult result;
        result.ok = false;
        result.message = L"Unknown tweak.";
        return result;
    }
    return ApplyOne(*def);
}

NetOpResult RevertGameTweak(const std::wstring& id) {
    const Def* def = FindDef(id);
    if (!def) {
        NetOpResult result;
        result.ok = false;
        result.message = L"Unknown tweak.";
        return result;
    }
    return RevertOne(*def);
}

NetOpResult TurnOffGameTweak(const std::wstring& id) {
    const Def* def = FindDef(id);
    if (!def) {
        NetOpResult result;
        result.ok = false;
        result.message = L"Unknown tweak.";
        return result;
    }
    return TurnOffOne(*def);
}

NetOpResult ApplyGameTweaks(bool includeAdvanced) {
    NetOpResult result;
    int applied = 0;
    for (const auto& def : Catalog()) {
        if (!includeAdvanced && !def.inProfile) continue;
        NetOpResult one = ApplyOne(def);
        result.changed += one.changed;
        result.skipped += one.skipped;
        result.adminNeeded = result.adminNeeded || one.adminNeeded;
        if (one.changed > 0) ++applied;
        if (!one.ok) {
            result.ok = false;
            if (result.message.empty()) result.message = one.message;
        }
    }
    if (result.ok && result.changed == 0)
        result.message = L"The gaming tweaks were already in place - nothing needed changing.";
    else if (result.ok)
        result.message = Plural(result.changed, L"value", L"values") + L" set across " +
                         Plural((size_t)applied, L"tweak", L"tweaks") + L".";
    else if (result.changed > 0)
        result.message = Plural(result.changed, L"value", L"values") + L" set; " + result.message;
    return result;
}

NetOpResult RevertGameTweaks() {
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
    // The background services are part of the same promise: Revert all puts back every
    // service the page parked, along with every saved registry value.
    NetOpResult services = RestoreGameServices();
    result.changed += services.changed;
    result.skipped += services.skipped;
    result.adminNeeded = result.adminNeeded || services.adminNeeded;
    if (!services.ok) {
        result.ok = false;
        if (result.message.empty()) result.message = services.message;
    }

    SyncMouseAcceleration();
    SyncWindowAnimation();

    if (result.changed == 0 && result.ok)
        result.message = L"No gaming tweak was changed by WinSuite, so there is nothing to put back.";
    else if (result.ok)
        result.message = Plural(result.changed, L"value", L"values") + L" put back.";
    else
        result.message = Plural(result.changed, L"value", L"values") + L" put back; " +
                         result.message;
    return result;
}

bool GameJournalRoundTrip(std::wstring& detail) {
    const std::wstring testKey = L"Software\\ZanpharsWinSuite\\SelfTest\\GameBooster";
    const std::wstring probe = L"Probe";
    RegDeleteTreeW(HKEY_CURRENT_USER, testKey.c_str());

    Target original;
    original.root = HKEY_CURRENT_USER;
    original.key = testKey;
    original.name = probe;
    original.type = REG_SZ;
    original.text = L"17";
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
    gaming.type = REG_DWORD;
    gaming.dword = 1;
    if (!WriteValue(gaming, error, code)) {
        detail = L"the test value could not be changed";
        RegDeleteTreeW(HKEY_CURRENT_USER, testKey.c_str());
        return false;
    }

    RestoreStats stats = RestoreEntries(L"selftest|");
    Current after;
    const bool readBack = ReadValue(HKEY_CURRENT_USER, testKey, probe, after, code) &&
                          after.exists && after.type == REG_SZ && after.text == L"17";
    const bool clean = JournalNames(L"selftest|").empty();
    RegDeleteTreeW(HKEY_CURRENT_USER, testKey.c_str());

    bool ok = stats.restored == 1 && stats.failed == 0 && readBack && clean;
    detail = ok ? L"SZ 17 -> DWORD 1 -> SZ 17, journal empty"
                : L"restored " + std::to_wstring(stats.restored) + L", value back: " +
                      (readBack ? L"yes" : L"no") + L", journal clear: " +
                      (clean ? L"yes" : L"no");
    return ok;
}

// ------------------------------------------------------- background services
// The long list of services a gaming session does not need. Each row stops its services and
// parks them at Manual - not Disabled, so Windows can still start one when something
// genuinely asks for it - after saving the start type, running state and delayed-autostart
// flag, so Revert puts each one back exactly. No row is part of the one-click profile: what
// is safe to park depends on the machine, so each one is the user's own call.
namespace {

struct ServiceDef {
    const wchar_t* id;
    std::wstring title;
    const wchar_t* summary;
    std::vector<const wchar_t*> services;
    bool critical = false;
};

const std::vector<ServiceDef>& ServiceCatalog() {
    static const std::vector<ServiceDef> defs = {
        {L"telemetry", L"Connected User Experiences and Telemetry",
         L"Windows' telemetry uploader and the diagnostics pipeline behind it.",
         {L"DiagTrack"}, false},
        {L"touchkeyboard", L"Touch Keyboard and Handwriting Panel",
         L"Pen, touch and handwriting input. Keep it if this PC has a touch screen.",
         {L"TabletInputService"}, false},
        {L"netlogon", L"Netlogon",
         L"Domain sign-in. Leave it running on a work or school PC.", {L"Netlogon"},
         true},
        {L"xboxlive", L"Xbox Live services",
         L"Xbox sign-in, cloud saves, networking and controller support.",
         {L"XblAuthManager", L"XblGameSave", L"XboxNetApiSvc", L"XboxGipSvc"}, false},
        {L"imaging", L"Windows Image Acquisition",
         L"Scanners and still cameras. Keep it if you scan or import photos.", {L"StiSvc"},
         false},
        {L"geolocation", L"Geolocation Service",
         L"Location for apps and the weather and clock widgets.", {L"lfsvc"}, false},
        {L"compatassistant", L"Program Compatibility Assistant",
         L"Watches for programs that need a compatibility shim.", {L"PcaSvc"}, false},
        {L"wappush", L"Device Management WAP Push Message Routing",
         L"Carries mobile-operator configuration messages. Unused on a desktop.",
         {L"dmwappushservice"}, false},
        {L"retaildemo", L"Retail Demo Service",
         L"Store demo mode. Only a display unit in a shop needs it.", {L"RetailDemo"}, false},
        {L"errorreporting", L"Windows Error Reporting",
         L"Sends crash reports and can raise the crash dialogs.", {L"WerSvc"}, false},
        {L"spooler", L"Print Spooler",
         L"Printing and print-to-PDF. Keep it while a printer is in use.", {L"Spooler"},
         false},
        {L"certificate", L"Certificate Propagation",
         L"Copies certificates from a smart card into the session.", {L"CertPropSvc"},
         false},
        {L"enterpriseapp", L"Enterprise App Management",
         L"Installs and manages apps supplied by a workplace.", {L"EntAppSvc"}, false},
        {L"biometric", L"Windows Biometric Service",
         L"Fingerprint and face sign-in. Keep it if Windows Hello uses it.", {L"WbioSrvc"},
         false},
        {L"insider", L"Windows Insider Service",
         L"Windows Insider Preview builds and their feedback channel.", {L"wisvc"}, false},
        {L"connectionbroker", L"Network Connection Broker",
         L"Lets apps and the Store hold connections open in the background.",
         {L"NcbService"}, false},
        {L"remotedesktop", L"Remote Desktop Configuration and Services",
         L"Hosting Remote Desktop sessions. Off means this PC cannot be reached remotely.",
         {L"SessionEnv", L"UmRdpService", L"TermService"}, true},
        {L"portabledevice", L"Portable Device Enumerator",
         L"Detects phones and media players over MTP.", {L"WPDBusEnum"}, false},
        {L"parentalcontrol", L"Parental Control",
         L"Family Safety time limits and app restrictions.", {L"WpcMonSvc"}, false},
        {L"connectnow", L"Windows Connect Now",
         L"Wireless setup carried on a USB flash drive (WCN).", {L"wcncsvc"}, false},
        {L"smartcard", L"Smart Card and Related Services",
         L"Smart card readers, and the policy that locks the session when a card is pulled.",
         {L"SCardSvr", L"ScDeviceEnum", L"SCPolicySvc"}, false},
        {L"eventlog", L"Windows Event Log",
         L"Every component logs through it. Windows normally refuses to stop it, and "
         L"diagnostics stop working without it.",
         {L"EventLog"}, true},
        {L"remoteregistry", L"Remote Registry",
         L"Lets another machine read this PC's registry over the network.",
         {L"RemoteRegistry"}, false},
        {L"phone", L"Phone Service",
         L"Telephony and modem line state. Unused without a modem or dial-up.", {L"PhoneSvc"},
         false},
        {L"maps", L"Downloaded Maps Manager",
         L"Offline maps for the Maps app. Started again when a map is opened.",
         {L"MapsBroker"}, false},
        {L"alljoyn", L"AllJoyn Router Service",
         L"AllJoyn (IoT) device discovery. Almost nothing uses it.", {L"AJRouter"}, false},
        {L"fax", L"Fax",
         L"Sending and receiving faxes over a fax modem.", {L"Fax"}, false},
        {L"secondarylogon", L"Secondary Logon",
         L"Run as different user. Some installers and support tools rely on it.",
         {L"seclogon"}, false},
    };
    return defs;
}

const ServiceDef* FindServiceDef(const std::wstring& id) {
    for (const auto& def : ServiceCatalog())
        if (id == def.id) return &def;
    return nullptr;
}

// The journal entry for one service of one row: "svc|<row>|<service>". Every other prefix
// in the Game Booster journal starts with a row id, so nothing else can match these.
std::wstring ServiceJournalEntry(const std::wstring& id, const std::wstring& name) {
    return L"svc|" + id + L"|" + name;
}

std::wstring EncodeServiceState(DWORD start, bool running, bool delayed) {
    return std::to_wstring(start) + L"|" + (running ? L"1" : L"0") + L"|" +
           (delayed ? L"1" : L"0");
}

bool DecodeServiceState(const std::wstring& text, DWORD& start, bool& running, bool& delayed) {
    const size_t first = text.find(L'|');
    const size_t second = first == std::wstring::npos ? std::wstring::npos
                                                      : text.find(L'|', first + 1);
    if (first == std::wstring::npos || second == std::wstring::npos) return false;
    start = (DWORD)wcstoul(text.c_str(), nullptr, 10);
    running = text.substr(first + 1, second - first - 1) == L"1";
    delayed = text.substr(second + 1) == L"1";
    return true;
}

struct ServiceState {
    DWORD start = SERVICE_DEMAND_START;
    bool running = false;
    bool delayed = false;
    bool present = false;
};

ServiceState QueryState(SC_HANDLE service) {
    ServiceState state;
    DWORD needed = 0;
    if (QueryServiceConfigW(service, nullptr, 0, &needed) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return state;
    std::vector<BYTE> buffer(needed);
    auto* config = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
    if (!QueryServiceConfigW(service, config, needed, &needed)) return state;
    state.start = config->dwStartType;

    SERVICE_DELAYED_AUTO_START_INFO delayed = {};
    if (QueryServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                             reinterpret_cast<LPBYTE>(&delayed), sizeof(delayed), &needed))
        state.delayed = delayed.fDelayedAutostart != FALSE;

    SERVICE_STATUS_PROCESS status = {};
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status),
                             sizeof(status), &needed))
        state.running = status.dwCurrentState == SERVICE_RUNNING ||
                        status.dwCurrentState == SERVICE_START_PENDING;
    state.present = true;
    return state;
}

std::wstring StartLabel(const ServiceState& state) {
    std::wstring label;
    if (state.start == SERVICE_AUTO_START)
        label = state.delayed ? L"Automatic (delayed)" : L"Automatic";
    else
        label = ServiceStartTypeLabel(state.start);
    return label + (state.running ? L", running" : L", stopped");
}

bool RefusedByRights(DWORD code) {
    return code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD;
}

// One service's park. The record it creates is dropped again when the service did not
// actually change, so a refused press never leaves a row offering Revert for a change that
// never happened.
enum class ParkResult { Parked, AlreadyParked, Missing, Refused };

ParkResult ParkOneService(SC_HANDLE scm, const std::wstring& id, const wchar_t* name, DWORD& code,
                          std::wstring& why) {
    code = 0;
    why.clear();
    SC_HANDLE service = OpenServiceW(
        scm, name,
        SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP);
    if (!service) {
        code = GetLastError();
        if (code == ERROR_SERVICE_DOES_NOT_EXIST || code == ERROR_INVALID_NAME)
            return ParkResult::Missing;
        why = std::wstring(name) + L": " + WinErrorText(code);
        return ParkResult::Refused;
    }
    const ServiceState state = QueryState(service);
    if (!state.present) {
        CloseServiceHandle(service);
        code = ERROR_SERVICE_DOES_NOT_EXIST;
        return ParkResult::Missing;
    }
    if (state.start == SERVICE_DEMAND_START && !state.running) {
        CloseServiceHandle(service);
        return ParkResult::AlreadyParked;
    }

    const std::wstring entry = ServiceJournalEntry(id, name);
    std::wstring existing;
    const bool hadEntry = JournalRead(entry, existing);
    if (!hadEntry &&
        !JournalWrite(entry, EncodeServiceState(state.start, state.running, state.delayed))) {
        CloseServiceHandle(service);
        why = std::wstring(name) + L": the original start type could not be saved";
        return ParkResult::Refused;
    }

    bool startChanged = false, stopped = false;
    if (state.start != SERVICE_DEMAND_START) {
        if (ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_DEMAND_START,
                                 SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr))
            startChanged = true;
        else
            code = GetLastError();
    }
    if (code == 0 && state.running) {
        SERVICE_STATUS status = {};
        if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
            stopped = true;
            // It is parked either way; waiting only makes the row read Stopped on the next
            // repaint instead of Stopping.
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
        } else {
            code = GetLastError();
            if (code == ERROR_SERVICE_NOT_ACTIVE) code = 0;
        }
    }
    CloseServiceHandle(service);
    if (code != 0) {
        // When the start type was already Manual and the stop was what failed, the record
        // stays: it is the only thing that remembers the original state.
        if (!hadEntry && !startChanged && !stopped) JournalDelete(entry);
        why = std::wstring(name) + L": " + WinErrorText(code);
        return ParkResult::Refused;
    }
    return ParkResult::Parked;
}

enum class RestoreResult { Restored, Dropped, Failed };

// Puts back one saved service and drops its record only once the service is really back: a
// record lost on a refused write would leave it parked with nothing saying what it was.
RestoreResult RestoreOneService(SC_HANDLE scm, const std::wstring& entry, std::wstring& name,
                                DWORD& code, std::wstring& why) {
    code = 0;
    why.clear();
    name.clear();
    std::wstring encoded;
    if (!JournalRead(entry, encoded)) {
        why = L"an entry could not be read";
        return RestoreResult::Failed;
    }
    const size_t last = entry.rfind(L'|');
    if (last != std::wstring::npos) name = entry.substr(last + 1);
    DWORD start = SERVICE_DEMAND_START;
    bool running = false, delayed = false;
    if (name.empty() || !DecodeServiceState(encoded, start, running, delayed)) {
        JournalDelete(entry);
        return RestoreResult::Dropped;
    }

    SC_HANDLE service = OpenServiceW(
        scm, name.c_str(),
        SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START);
    if (!service) {
        code = GetLastError();
        why = name + L": " + WinErrorText(code);
        return RestoreResult::Failed;
    }
    bool ok = ChangeServiceConfigW(service, SERVICE_NO_CHANGE, start, SERVICE_NO_CHANGE, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != FALSE;
    if (!ok) code = GetLastError();
    if (ok && start == SERVICE_AUTO_START) {
        SERVICE_DELAYED_AUTO_START_INFO info = {};
        info.fDelayedAutostart = delayed ? TRUE : FALSE;
        if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &info)) {
            code = GetLastError();
            ok = false;
        }
    }
    // A start Windows declines leaves the service stopped at its original start type, which
    // is what the machine looked like any time that service was not running.
    if (ok && running) StartServiceW(service, 0, nullptr);
    CloseServiceHandle(service);
    if (!ok) {
        why = name + L": " + WinErrorText(code);
        return RestoreResult::Failed;
    }
    JournalDelete(entry);
    return RestoreResult::Restored;
}

}  // namespace

std::vector<GameServiceView> LoadGameServices() {
    std::vector<GameServiceView> views;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    for (const auto& def : ServiceCatalog()) {
        GameServiceView view;
        view.id = def.id;
        view.title = def.title;
        view.summary = def.summary;
        view.critical = def.critical;

        std::wstring names, firstState;
        for (const wchar_t* name : def.services) {
            ServiceState state;
            if (scm) {
                SC_HANDLE service =
                    OpenServiceW(scm, name, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
                if (service) {
                    state = QueryState(service);
                    CloseServiceHandle(service);
                }
            }
            if (!state.present) continue;  // not on this PC: the row simply skips it
            ++view.installed;
            if (state.running || state.start == SERVICE_AUTO_START) ++view.active;
            if (!names.empty()) names += L", ";
            names += name;
            if (firstState.empty()) firstState = StartLabel(state);
        }

        if (view.installed == 0)
            view.state = L"not installed on this PC";
        else if (view.installed == 1)
            view.state = names + L" - " + firstState;
        else
            view.state = names + L" - " + std::to_wstring(view.active) + L" of " +
                         std::to_wstring(view.installed) + L" running or automatic";
        // Off means every one of them is stopped and none starts by itself, which is also
        // how most of this list arrives on a fresh PC - those rows read as already done.
        view.applied = view.installed > 0 && view.active == 0;
        view.journaled = !JournalNames(L"svc|" + view.id + L"|").empty();
        views.push_back(view);
    }
    if (scm) CloseServiceHandle(scm);
    return views;
}

NetOpResult TurnOffGameService(const std::wstring& id) {
    NetOpResult result;
    const ServiceDef* def = FindServiceDef(id);
    if (!def) {
        result.ok = false;
        result.message = L"Unknown service row.";
        return result;
    }
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        const DWORD rc = GetLastError();
        result.ok = false;
        result.adminNeeded = RefusedByRights(rc);
        result.message = def->title + L": " + WinErrorText(rc);
        return result;
    }

    int parked = 0, refused = 0;
    for (const wchar_t* name : def->services) {
        DWORD code = 0;
        std::wstring why;
        switch (ParkOneService(scm, id, name, code, why)) {
            case ParkResult::Parked:
                ++parked;
                break;
            case ParkResult::Missing:  // not on this PC: the row simply skips it
            case ParkResult::AlreadyParked:
                break;
            case ParkResult::Refused:
                ++refused;
                if (RefusedByRights(code)) result.adminNeeded = true;
                if (result.message.empty()) result.message = why;
                break;
        }
    }
    CloseServiceHandle(scm);

    result.changed = (uint32_t)parked;
    result.skipped = (uint32_t)refused;
    result.ok = refused == 0;
    if (parked == 0 && refused == 0)
        result.message = def->title + L": already stopped and Manual - nothing to change.";
    else if (refused == 0)
        result.message = def->title + L": " + Plural((size_t)parked, L"service", L"services") +
                         L" stopped and set to Manual.";
    else if (parked == 0)
        result.message = def->title + L": Windows refused - " + result.message;
    else
        result.message = def->title + L": " + Plural((size_t)parked, L"service", L"services") +
                         L" stopped; the rest was refused - " + result.message;
    if (result.adminNeeded) result.message += L" Restart as administrator and try again.";
    return result;
}

NetOpResult RevertGameService(const std::wstring& id) {
    NetOpResult result;
    const ServiceDef* def = FindServiceDef(id);
    if (!def) {
        result.ok = false;
        result.message = L"Unknown service row.";
        return result;
    }
    const std::vector<std::wstring> entries = JournalNames(L"svc|" + id + L"|");
    if (entries.empty()) {
        result.message = def->title + L": nothing was saved here, so nothing was put back.";
        return result;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        const DWORD rc = GetLastError();
        result.ok = false;
        result.adminNeeded = RefusedByRights(rc);
        result.message = def->title + L": " + WinErrorText(rc);
        return result;
    }

    int restored = 0, failed = 0;
    for (const auto& entry : entries) {
        DWORD code = 0;
        std::wstring name, why;
        switch (RestoreOneService(scm, entry, name, code, why)) {
            case RestoreResult::Restored:
                ++restored;
                break;
            case RestoreResult::Dropped:  // an unreadable record: nothing left to put back
                break;
            case RestoreResult::Failed:
                ++failed;
                if (RefusedByRights(code)) result.adminNeeded = true;
                if (result.message.empty()) result.message = why;
                break;
        }
    }
    CloseServiceHandle(scm);

    result.changed = (uint32_t)restored;
    result.skipped = (uint32_t)failed;
    result.ok = failed == 0;
    if (failed == 0 && restored > 0)
        result.message = def->title + L": " + Plural((size_t)restored, L"service", L"services") +
                         L" put back exactly as they were.";
    else if (failed == 0)
        result.message = def->title + L": nothing was saved here, so nothing was put back.";
    else
        result.message = def->title + L": " + Plural((size_t)restored, L"service", L"services") +
                         L" put back, " + std::to_wstring(failed) + L" refused - " +
                         result.message;
    if (result.adminNeeded) result.message += L" Restart as administrator and try again.";
    return result;
}

NetOpResult RestoreGameServices() {
    NetOpResult total;
    for (const auto& def : ServiceCatalog()) {
        if (JournalNames(L"svc|" + std::wstring(def.id) + L"|").empty()) continue;
        NetOpResult one = RevertGameService(def.id);
        total.changed += one.changed;
        total.skipped += one.skipped;
        total.adminNeeded = total.adminNeeded || one.adminNeeded;
        if (!one.ok) {
            total.ok = false;
            if (total.message.empty()) total.message = one.message;
        }
    }
    return total;
}

bool GameServiceJournalRoundTrip(std::wstring& detail) {
    DWORD start = SERVICE_AUTO_START;
    bool running = false, delayed = false;
    const std::wstring encoded = EncodeServiceState(SERVICE_DEMAND_START, true, false);
    const bool decoded = DecodeServiceState(encoded, start, running, delayed) &&
                         start == SERVICE_DEMAND_START && running && !delayed;
    const bool named =
        ServiceJournalEntry(L"xboxlive", L"XblGameSave") == L"svc|xboxlive|XblGameSave";
    const bool rejected = !DecodeServiceState(L"nonsense", start, running, delayed);
    const bool ok = decoded && named && rejected;
    detail = ok ? L"manual, running kept; entry \"svc|row|service\""
                : std::wstring(decoded ? L"" : L"a saved state did not read back; ") +
                      (named ? L"" : L"the entry name is wrong; ") +
                      (rejected ? L"" : L"a broken record was accepted");
    return ok;
}

// The park-and-restore cycle the rows use, run against one named service. The elevated
// service test calls it on a scratch service of its own, so the path that stops a real
// service, journals it and puts it back is exercised end to end rather than by inspection.
bool GameServiceParkRoundTrip(const std::wstring& serviceName, std::wstring& detail) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        detail = L"the service manager could not be opened: " + WinErrorText(GetLastError());
        return false;
    }

    DWORD before = 0;
    {
        SC_HANDLE service = OpenServiceW(scm, serviceName.c_str(), SERVICE_QUERY_CONFIG);
        if (!service) {
            detail = serviceName + L": " + WinErrorText(GetLastError());
            CloseServiceHandle(scm);
            return false;
        }
        const ServiceState state = QueryState(service);
        CloseServiceHandle(service);
        if (!state.present) {
            detail = serviceName + L" is not installed";
            CloseServiceHandle(scm);
            return false;
        }
        before = state.start;
    }

    const std::wstring id = L"selftest";
    const std::wstring entry = ServiceJournalEntry(id, serviceName);
    DWORD code = 0;
    std::wstring why;
    const ParkResult parked = ParkOneService(scm, id, serviceName.c_str(), code, why);

    DWORD afterPark = 0;
    bool parkedOk = parked == ParkResult::Parked;
    if (parkedOk) {
        SC_HANDLE service = OpenServiceW(scm, serviceName.c_str(), SERVICE_QUERY_CONFIG);
        parkedOk = service != nullptr;
        if (service) {
            const ServiceState state = QueryState(service);
            parkedOk = state.present && state.start == SERVICE_DEMAND_START && !state.running;
            CloseServiceHandle(service);
        }
    }
    std::wstring saved;
    const bool journaled = JournalRead(entry, saved) && !saved.empty();

    DWORD restoreCode = 0;
    std::wstring restoredName, restoreWhy;
    const RestoreResult restored =
        journaled ? RestoreOneService(scm, entry, restoredName, restoreCode, restoreWhy)
                  : RestoreResult::Failed;
    {
        SC_HANDLE service = OpenServiceW(scm, serviceName.c_str(), SERVICE_QUERY_CONFIG);
        if (service) {
            const ServiceState state = QueryState(service);
            afterPark = state.present ? state.start : 0;
            CloseServiceHandle(service);
        }
    }
    const bool back = restored == RestoreResult::Restored && afterPark == before;
    const bool clean = JournalNames(L"svc|" + id + L"|").empty();
    CloseServiceHandle(scm);

    const bool ok = parkedOk && journaled && back && clean;
    if (!parkedOk)
        detail = L"the service was not left at Manual: " + (why.empty() ? L"no reason given" : why);
    else if (!journaled)
        detail = L"the original state was not saved before the change";
    else if (!back)
        detail = L"the original start type was not restored: " +
                 (restoreWhy.empty() ? WinErrorText(restoreCode) : restoreWhy);
    else if (!clean)
        detail = L"the saved record survived the restore";
    else
        detail = L"Manual and stopped, then back to start type " + std::to_wstring(before) +
                 L" with the record cleared";
    return ok;
}
