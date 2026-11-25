// ============================================================================
//  catalog.cpp — the settings Home Policy knows, and the registry engine that
//  reads and writes them.
//
//  The table is the program. Everything the window shows, filters, applies and
//  backs up comes from kPolicies; the only code that knows about a particular
//  setting is the row itself.
// ============================================================================

#include "catalog.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>

namespace hp {
namespace {

constexpr DWORD kReadAccess = KEY_READ | KEY_WOW64_64KEY;
// Creating a key needs KEY_CREATE_SUB_KEY on the parent as well as KEY_SET_VALUE
// on the key itself; without it RegCreateKeyEx reports Access denied even for
// the caller's own hive.
constexpr DWORD kWriteAccess = KEY_SET_VALUE | KEY_CREATE_SUB_KEY | KEY_WOW64_64KEY;

// Which hive a path lives in, spelled the way the registry editor spells it.
const wchar_t* RootName(HKEY root) {
    if (root == HKEY_CURRENT_USER) return L"HKEY_CURRENT_USER";
    if (root == HKEY_CLASSES_ROOT) return L"HKEY_CLASSES_ROOT";
    return L"HKEY_LOCAL_MACHINE";
}

std::wstring Describe(LSTATUS status) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(status), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text;
    if (length > 0 && buffer) {
        text.assign(buffer, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
            text.pop_back();
    }
    if (buffer) LocalFree(buffer);
    if (text.empty()) text = L"Windows error " + std::to_wstring(status);
    return text;
}

bool ReadDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD* out) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key, 0, kReadAccess, &handle) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(sizeof(DWORD));
    DWORD data = 0;
    const LSTATUS status =
        RegQueryValueExW(handle, name, nullptr, &type, reinterpret_cast<BYTE*>(&data), &size);
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS || type != REG_DWORD || size < sizeof(DWORD)) return false;
    *out = data;
    return true;
}

// ---------------------------------------------------------------------------
//  The catalog
//
//  "Disabled" removes the value for almost every row, because that is what the
//  official editor does when a policy is set to Disabled and the policy has no
//  explicit off-data: the component falls back to its own default. The update
//  deferral rows are the exception - they write a delay pair, and Disabled
//  writes zeroes so the delay is plainly off rather than half-configured.
// ---------------------------------------------------------------------------

const std::vector<Policy> kPolicies = {
    // ---------------------------------------------------- Windows Update (1)
    // Microsoft scopes this to devices whose updates are installed on a schedule
    // (AuOptions 4) and warns in its own documentation that the policy "doesn't
    // work exactly as per description", so the row says so on the card.
    {1, L"Never restart while I am signed in",
     L"Updates install, but Windows waits for you to save your work instead of rebooting under it.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU",
     {{L"NoAutoRebootWithLoggedOnUsers", 1, 0, true}}, 1, L"Immediately",
     L"Only with scheduled updates"},
    {1, L"Keep driver updates out of Windows Update",
     L"Monthly quality updates still install, without the driver bundle that can replace a working "
     L"graphics or network driver.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate",
     {{L"ExcludeWUDriversInQualityUpdate", 1, 0, true}}, 1, L"Restart",
     L"Not documented for Home"},
    {1, L"Hold back a new Windows version for 180 days",
     L"A feature update - a new Windows version - waits half a year while it settles. Security "
     L"updates keep arriving in the meantime.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate",
     {{L"DeferFeatureUpdates", 1, 0, true},
      {L"DeferFeatureUpdatesPeriodInDays", 180, 0, true}},
     2, L"Restart", L"Home ignores this"},
    {1, L"Hold back monthly quality updates by 7 days",
     L"Security and reliability fixes arrive a week late, which is usually enough for a bad update "
     L"to be pulled first.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate",
     {{L"DeferQualityUpdates", 1, 0, true},
      {L"DeferQualityUpdatesPeriodInDays", 7, 0, true}},
     2, L"Restart", L"Home ignores this"},

    // ------------------------------------------------- Privacy & telemetry (2)
    // The AllowTelemetry row is deliberately absent: Microsoft documents 0 on
    // Home as equivalent to 1, so the row could not do anything here.
    {2, L"Turn off the advertising ID",
     L"Apps stop seeing a per-user ID they can use to profile you across programs.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\AdvertisingInfo",
     {{L"DisabledByGroupPolicy", 1, 0, true}}, 1, L"Sign out", nullptr},
    {2, L"Stop Windows asking me for feedback",
     L"No more 'How likely are you to recommend Windows' prompts after an update.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection",
     {{L"DoNotShowFeedbackNotifications", 1, 0, true}}, 1, L"Immediately", nullptr},
    // Both values are policy "on" data at 0: PublishUserActivities=0 is the
    // documented "activities aren't allowed to be published", and
    // UploadUserActivities=0 the matching ban on uploading them. The row used to
    // be titled "Keep my activity history on this PC", which described the
    // opposite of what it writes - nothing is kept once publishing stops.
    {2, L"Turn off my activity history",
     L"Windows stops recording the apps and files you open and stops sending that history to your "
     L"Microsoft account, so continue-where-you-left-off has nothing to offer.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System",
     {{L"PublishUserActivities", 0, 1, true},
      {L"UploadUserActivities", 0, 1, true}},
     2, L"Immediately", nullptr},
    {2, L"Turn off clipboard sync between devices",
     L"Copying text on this PC no longer sends it to your other machines, and theirs does not "
     L"arrive here.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System",
     {{L"AllowCrossDeviceClipboard", 0, 1, true}}, 1, L"Sign out", nullptr},
    {2, L"Turn off location for every app",
     L"The location service is switched off machine-wide, so no app can ask it where you are.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\LocationAndSensors",
     {{L"DisableLocation", 1, 0, true}}, 1, L"Sign out", nullptr},
    {2, L"Turn off Windows Error Reporting",
     L"A crashed program is not packaged and sent to Microsoft; the local event log still records "
     L"the crash.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Error Reporting",
     {{L"Disabled", 1, 0, true}}, 1, L"Restart", nullptr},
    {2, L"Turn off tips written from my diagnostic data",
     L"Windows stops using what it collects about this PC to word suggestions and tips for it.",
     HKEY_CURRENT_USER, L"SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent",
     {{L"DisableTailoredExperiencesWithDiagnosticData", 1, 0, true}}, 1, L"Sign out", nullptr},
    {2, L"Turn off suggested apps and promoted content",
     L"Windows stops installing promoted apps on its own and stops showing suggestions in Settings "
     L"and the Start menu.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent",
     {{L"DisableWindowsConsumerFeatures", 1, 0, true}}, 1, L"Sign out", nullptr},
    {2, L"Turn off Windows tips and welcome screens",
     L"Skips the full-screen 'what's new' pages that appear after an update.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent",
     {{L"DisableSoftLanding", 1, 0, true}}, 1, L"Sign out", nullptr},
    {2, L"Turn off speech and handwriting personalisation",
     L"Dictation and inking stop building a personal dictionary from what you type and say.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\InputPersonalization",
     {{L"AllowInputPersonalization", 0, 1, true}}, 1, L"Sign out", nullptr},

    // ----------------------------------------- Start, search & taskbar (3)
    {3, L"Turn off web results in Start search",
     L"Searching the Start menu looks only at this PC - no Bing answers, no web suggestions.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search",
     {{L"DisableWebSearch", 1, 0, true},
      {L"ConnectedSearchUseWeb", 0, 1, true}},
     2, L"Restart Explorer", nullptr},
    {3, L"Turn off Cortana",
     L"The assistant is switched off machine-wide. Newer Windows 11 builds no longer ship Cortana "
     L"at all, so this may have nothing left to turn off.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search",
     {{L"AllowCortana", 0, 1, true}}, 1, L"Restart", L"Deprecated by Microsoft"},
    {3, L"Turn off Search highlights",
     L"The changing picture and 'on this day' cards in the search box are removed, leaving plain "
     L"results.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search",
     {{L"EnableDynamicContentInWSB", 0, 1, true}}, 1, L"Immediately", nullptr},
    {3, L"Turn off the Widgets board",
     L"The weather-and-news panel on the taskbar is disabled, including the feed it downloads.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Dsh",
     {{L"AllowNewsAndInterests", 0, 1, true}}, 1, L"Restart Explorer", nullptr},
    {3, L"Turn off Windows Copilot",
     L"Removes the Copilot button and its side panel on builds that still read this policy; the "
     L"current Copilot app is not covered by it.",
     HKEY_CURRENT_USER, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsCopilot",
     {{L"TurnOffWindowsCopilot", 1, 0, true}}, 1, L"Restart Explorer", L"Deprecated by Microsoft"},

    // ------------------------------------------------------ File Explorer (4)
    {4, L"Stop keeping a history of recent documents",
     L"File Explorer stops recording which files you opened, so the recent list stays empty.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
     {{L"NoRecentDocsHistory", 1, 0, true}}, 1, L"Sign out", nullptr},
    {4, L"Turn off thumbnail caching",
     L"Thumbnails are rebuilt when needed instead of being stored in every folder, which leaves "
     L"less hidden data on disk.",
     HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
     {{L"NoThumbnailCache", 1, 0, true}}, 1, L"Restart Explorer", nullptr},
    {4, L"Turn off window shaking",
     L"Aero Shake - grabbing a title bar and shaking it to minimise everything else - is switched "
     L"off.",
     HKEY_CURRENT_USER, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Explorer",
     {{L"NoWindowMinimizingShortcuts", 1, 0, true}}, 1, L"Restart Explorer", nullptr},
    {4, L"Turn off the Windows key shortcuts",
     L"Win+E, Win+R, Win+S and the rest stop responding, which keeps a full-screen game from "
     L"jumping to the desktop.",
     HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
     {{L"NoWinKeys", 1, 0, true}}, 1, L"Sign out", nullptr},
    {4, L"Turn off Store suggestions in Open with",
     L"When a file has no program for it, Windows offers the Store less often and shows the plain "
     L"'choose an app' list.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Explorer",
     {{L"NoUseStoreOpenWith", 1, 0, true}}, 1, L"Restart", nullptr},

    // ------------------------------------------------------- System & apps (5)
    {5, L"Turn off Game DVR and background recording",
     L"Stops the capture service that costs frames in every game, including ones you never record.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR",
     {{L"AllowGameDVR", 0, 1, true}}, 1, L"Sign out", nullptr},
    {5, L"Turn off OneDrive sync",
     L"The OneDrive sync client stops running, so the folder stays a folder. Files already on disk "
     L"are untouched; work and school accounts stop syncing too.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\OneDrive",
     {{L"DisableFileSyncNGSC", 1, 0, true}}, 1, L"Restart", nullptr},
    {5, L"Stop background apps for everyone",
     L"Store apps stop running when they are not on screen, which is where idle battery and "
     L"network use comes from.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy",
     {{L"LetAppsRunInBackground", 2, 0, true}}, 1, L"Sign out", nullptr},

    // ----------------------------------------------------------- Security (6)
    {6, L"Block potentially unwanted apps",
     L"Defender's middle setting: bundled toolbars, adware and 'cleaners' are blocked instead of "
     L"merely reported.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows Defender",
     {{L"PUAProtection", 1, 0, true}}, 1, L"Immediately", nullptr},
    {6, L"Turn off LLMNR name lookup",
     L"Local names resolve through DNS only, closing the spoofing path that answers LLMNR requests "
     L"before the real machine does.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\DNSClient",
     {{L"EnableMulticast", 0, 1, true}}, 1, L"Restart", nullptr},
    {6, L"Record PowerShell script blocks in the event log",
     L"Every script block is logged before it runs, so what a script did can be read back later.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\PowerShell\\ScriptBlockLogging",
     {{L"EnableScriptBlockLogging", 1, 0, true}}, 1, L"Immediately", nullptr},
    {6, L"Turn off Windows Script Host",
     L".vbs and .js files stop running as programs, which removes the favourite delivery method "
     L"for email and USB malware.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Script Host\\Settings",
     {{L"Enabled", 0, 1, true}}, 1, L"Immediately", nullptr},
    {6, L"Turn off Remote Assistance invitations",
     L"Nobody can offer to connect to this PC through Remote Assistance, and no invitation can be "
     L"sent from it.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\Terminal Services",
     {{L"fAllowToGetHelp", 0, 1, true}}, 1, L"Immediately", nullptr},
    {6, L"Turn off AutoPlay for every drive",
     L"A USB stick, phone or mapped drive no longer offers to launch what is on it when plugged in.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
     {{L"NoDriveTypeAutoRun", 0xFF, 0, true}}, 1, L"Sign out", nullptr},

    // --------------------------------------------------------- Microsoft Edge (7)
    {7, L"Skip Edge's first-run tour",
     L"Edge opens straight to your start page instead of the multi-page welcome flow.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Edge",
     {{L"HideFirstRunExperience", 1, 0, true}}, 1, L"Restart Edge", nullptr},
    {7, L"Turn off the Edge sidebar",
     L"The side panel with its built-in tools and links is removed from the toolbar.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Edge",
     {{L"HubsSidebarEnabled", 0, 1, true}}, 1, L"Restart Edge", nullptr},
    {7, L"Turn off the Edge shopping assistant",
     L"Price-comparison popups and coupon prompts stop appearing on shopping pages.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Edge",
     {{L"EdgeShoppingAssistantEnabled", 0, 1, true}}, 1, L"Restart Edge", nullptr},
    // ShowRecommendationsEnabled covers the coach marks, flyouts and banners that
    // suggest a feature; it does not touch the promotional tabs Edge can open by
    // itself, so the summary no longer claims it does.
    {7, L"Turn off Edge feature recommendations",
     L"Edge stops showing the coach marks, flyouts and banners that suggest a feature or explain "
     L"how to use the browser.",
     HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Edge",
     {{L"ShowRecommendationsEnabled", 0, 1, true}}, 1, L"Immediately", nullptr},
};

const std::vector<const wchar_t*> kCategories = {
    L"All settings",
    L"Windows Update",
    L"Privacy & telemetry",
    L"Start, search & taskbar",
    L"File Explorer",
    L"System & apps",
    L"Security",
    L"Microsoft Edge",
};

}  // namespace

// ---------------------------------------------------------------------------
//  Catalog access
// ---------------------------------------------------------------------------

const std::vector<Policy>& Policies() { return kPolicies; }
const std::vector<const wchar_t*>& Categories() { return kCategories; }

const wchar_t* TriName(Tri state) {
    switch (state) {
        case Tri::Enabled: return L"Enabled";
        case Tri::Disabled: return L"Disabled";
        case Tri::Custom: return L"Custom";
        default: return L"Not configured";
    }
}

std::wstring PathText(const Policy& policy) {
    std::wstring text = std::wstring(RootName(policy.root)) + L"\\" + policy.key;
    for (int i = 0; i < policy.valueCount; ++i)
        text += (i == 0 ? L"  -  " : L", ") + std::wstring(policy.values[i].name);
    return text;
}

Tri Classify(bool anyPresent, bool allOn, bool allOff) {
    if (!anyPresent) return Tri::NotConfigured;
    if (allOn) return Tri::Enabled;
    if (allOff) return Tri::Disabled;
    return Tri::Custom;
}

Tri Read(const Policy& policy) {
    bool any = false;
    bool allOn = true;
    bool allOff = true;
    for (int i = 0; i < policy.valueCount; ++i) {
        const PolicyValue& value = policy.values[i];
        DWORD data = 0;
        if (!ReadDword(policy.root, policy.key, value.name, &data)) {
            allOn = false;
            allOff = false;
            continue;
        }
        any = true;
        if (data != value.whenOn) allOn = false;
        if (!value.hasOff || data != value.whenOff) allOff = false;
    }
    return Classify(any, allOn, allOff);
}

bool Write(const Policy& policy, Tri want, std::wstring* error) {
    if (want == Tri::Custom) {
        if (error) *error = L"a custom value has to be changed in Registry Editor";
        return false;
    }

    // Not configured only ever deletes, so a missing key is already the wanted
    // state and must not be created just to be left empty.
    if (want == Tri::NotConfigured) {
        HKEY handle = nullptr;
        const LSTATUS opened = RegOpenKeyExW(policy.root, policy.key, 0, kWriteAccess, &handle);
        if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return true;
        if (opened != ERROR_SUCCESS) {
            if (error) *error = Describe(opened);
            return false;
        }
        bool ok = true;
        for (int i = 0; i < policy.valueCount; ++i) {
            const LSTATUS removed = RegDeleteValueW(handle, policy.values[i].name);
            if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
                ok = false;
                if (error) *error = std::wstring(policy.values[i].name) + L": " + Describe(removed);
            }
        }
        RegCloseKey(handle);
        return ok;
    }

    HKEY handle = nullptr;
    LSTATUS status = RegCreateKeyExW(policy.root, policy.key, 0, nullptr, 0, kWriteAccess, nullptr,
                                     &handle, nullptr);
    if (status != ERROR_SUCCESS) {
        if (error) *error = Describe(status);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < policy.valueCount && ok; ++i) {
        const PolicyValue& value = policy.values[i];
        const bool remove = want == Tri::Disabled && !value.hasOff;
        if (remove) {
            const LSTATUS removed = RegDeleteValueW(handle, value.name);
            if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
                ok = false;
                if (error) *error = std::wstring(value.name) + L": " + Describe(removed);
            }
            continue;
        }
        const DWORD data = want == Tri::Enabled ? value.whenOn : value.whenOff;
        status = RegSetValueExW(handle, value.name, 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&data), sizeof data);
        if (status != ERROR_SUCCESS) {
            ok = false;
            if (error) *error = std::wstring(value.name) + L": " + Describe(status);
        }
    }
    RegCloseKey(handle);
    return ok;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof elevation;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// ---------------------------------------------------------------------------
//  Backup
// ---------------------------------------------------------------------------

bool WriteBackup(const std::wstring& path, int* valuesWritten, std::wstring* error) {
    std::wstring text;
    text += L"Windows Registry Editor Version 5.00\r\n\r\n";
    text += L"; Home Policy backup - the policy values this tool manages, as they\r\n";
    text += L"; read at the time of writing. Open the file to put them back; a value\r\n";
    text += L"; that is not listed here was not configured.\r\n\r\n";

    int written = 0;
    std::wstring lastKey;
    for (const Policy& policy : kPolicies) {
        std::wstring rows;
        for (int i = 0; i < policy.valueCount; ++i) {
            DWORD data = 0;
            if (!ReadDword(policy.root, policy.key, policy.values[i].name, &data)) continue;
            wchar_t line[96] = {};
            swprintf_s(line, L"\"%s\"=dword:%08x\r\n", policy.values[i].name, data);
            rows += line;
            ++written;
        }
        if (rows.empty()) continue;
        const std::wstring header = std::wstring(L"[") + RootName(policy.root) + L"\\" + policy.key + L"]\r\n";
        if (header != lastKey) {
            text += header;
            lastKey = header;
        }
        text += rows + L"\r\n";
    }

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = Describe(GetLastError());
        return false;
    }
    const unsigned char bom[2] = {0xFF, 0xFE};
    DWORD wrote = 0;
    bool ok = WriteFile(file, bom, 2, &wrote, nullptr) != FALSE;
    if (ok) {
        const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
        ok = WriteFile(file, text.c_str(), bytes, &wrote, nullptr) != FALSE && wrote == bytes;
    }
    if (!ok && error) *error = Describe(GetLastError());
    CloseHandle(file);
    if (valuesWritten) *valuesWritten = written;
    return ok;
}

// ---------------------------------------------------------------------------
//  Console mode helpers
// ---------------------------------------------------------------------------

namespace {

std::wstring ToLower(std::wstring text) {
    for (wchar_t& ch : text) ch = static_cast<wchar_t>(towlower(ch));
    return text;
}

}  // namespace

std::vector<int> FindPolicies(const std::wstring& text) {
    std::vector<int> matches;
    const std::wstring needle = ToLower(text);
    if (needle.empty()) return matches;

    // An exact title wins outright, however many other titles contain it.
    for (size_t i = 0; i < kPolicies.size(); ++i)
        if (ToLower(kPolicies[i].title) == needle) return {static_cast<int>(i)};

    for (size_t i = 0; i < kPolicies.size(); ++i)
        if (ToLower(kPolicies[i].title).find(needle) != std::wstring::npos)
            matches.push_back(static_cast<int>(i));
    return matches;
}

bool ParseTri(const std::wstring& text, Tri* out) {
    const std::wstring lower = ToLower(text);
    if (lower == L"enabled" || lower == L"enable" || lower == L"on") {
        *out = Tri::Enabled;
        return true;
    }
    if (lower == L"disabled" || lower == L"disable" || lower == L"off") {
        *out = Tri::Disabled;
        return true;
    }
    if (lower == L"not-configured" || lower == L"notconfigured" || lower == L"not configured" ||
        lower == L"reset" || lower == L"default") {
        *out = Tri::NotConfigured;
        return true;
    }
    return false;
}

bool ReadFirstValue(const Policy& policy, DWORD* data) {
    if (policy.valueCount < 1) return false;
    return ReadDword(policy.root, policy.key, policy.values[0].name, data);
}

// ---------------------------------------------------------------------------
//  Handing staged choices to the elevated run
// ---------------------------------------------------------------------------

std::wstring EncodeStagedSpec(const std::vector<int>& indices,
                              const std::vector<Tri>& wanted) {
    std::wstring spec;
    for (size_t i = 0; i < indices.size() && i < wanted.size(); ++i) {
        const Tri state = wanted[i];
        if (state != Tri::NotConfigured && state != Tri::Enabled && state != Tri::Disabled) continue;
        if (!spec.empty()) spec += L';';
        spec += std::to_wstring(indices[i]) + L':' + std::to_wstring(static_cast<int>(state));
    }
    return spec;
}

int ApplyStagedSpec(const std::vector<Policy>& catalog, const std::wstring& spec,
                    std::wstring* firstError, std::wstring* firstTitle) {
    int changed = 0;
    size_t at = 0;
    while (at < spec.size()) {
        const size_t end = spec.find(L';', at);
        const std::wstring item =
            spec.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
        at = end == std::wstring::npos ? spec.size() : end + 1;

        const size_t colon = item.find(L':');
        if (colon == std::wstring::npos) continue;
        const std::wstring indexText = item.substr(0, colon);
        const std::wstring stateText = item.substr(colon + 1);
        if (indexText.empty() || stateText.empty()) continue;

        wchar_t* stop = nullptr;
        const long index = wcstol(indexText.c_str(), &stop, 10);
        if (!stop || *stop != L'\0') continue;
        stop = nullptr;
        const long stateValue = wcstol(stateText.c_str(), &stop, 10);
        if (!stop || *stop != L'\0') continue;
        if (index < 0 || index >= static_cast<long>(catalog.size())) continue;

        const Tri want = static_cast<Tri>(stateValue);
        if (want != Tri::NotConfigured && want != Tri::Enabled && want != Tri::Disabled) continue;

        std::wstring error;
        if (Write(catalog[static_cast<size_t>(index)], want, &error)) {
            ++changed;
        } else if (firstError && firstError->empty()) {
            *firstError = error;
            if (firstTitle) *firstTitle = catalog[static_cast<size_t>(index)].title;
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
//  Self-test
// ---------------------------------------------------------------------------

std::vector<TestResult> RunSelfTest() {
    std::vector<TestResult> results;

    // --- the table itself: no duplicate targets, every field filled in.
    {
        std::vector<std::wstring> targets;
        std::wstring problem;
        for (const Policy& policy : kPolicies) {
            if (!policy.title || !*policy.title || !policy.summary || !*policy.summary) {
                problem = L"a row has no title or summary";
                break;
            }
            if (!policy.key || !*policy.key) {
                problem = std::wstring(L"\"") + policy.title + L"\" has no registry key";
                break;
            }
            if (policy.category < 1 || policy.category >= static_cast<int>(kCategories.size())) {
                problem = std::wstring(L"\"") + policy.title + L"\" has no real category";
                break;
            }
            if (policy.valueCount < 1 || policy.valueCount > 3) {
                problem = std::wstring(L"\"") + policy.title + L"\" has an odd value count";
                break;
            }
            if (!policy.effect || !*policy.effect) {
                problem = std::wstring(L"\"") + policy.title + L"\" does not say when it applies";
                break;
            }
            for (int i = 0; i < policy.valueCount; ++i) {
                if (!policy.values[i].name || !*policy.values[i].name) {
                    problem = std::wstring(L"\"") + policy.title + L"\" has an unnamed value";
                    break;
                }
                const std::wstring target = PathText(policy) + L"|" + std::to_wstring(i);
                if (std::find(targets.begin(), targets.end(),
                              std::wstring(RootName(policy.root)) + L"\\" + policy.key + L"\\" +
                                  policy.values[i].name) != targets.end()) {
                    problem = std::wstring(L"\"") + policy.title + L"\" writes a value twice";
                    break;
                }
                targets.push_back(std::wstring(RootName(policy.root)) + L"\\" + policy.key + L"\\" +
                                  policy.values[i].name);
            }
            if (!problem.empty()) break;
        }
        results.push_back({L"the catalog is complete and has no duplicate values", problem.empty(),
                           problem});
    }

    // --- the three-state mapping, without touching the registry.
    {
        std::wstring problem;
        if (Classify(false, false, false) != Tri::NotConfigured) problem = L"absent values are not 'not configured'";
        else if (Classify(true, true, false) != Tri::Enabled) problem = L"on-data is not 'enabled'";
        else if (Classify(true, false, true) != Tri::Disabled) problem = L"off-data is not 'disabled'";
        else if (Classify(true, false, false) != Tri::Custom) problem = L"a third value is not 'custom'";
        results.push_back({L"a policy reads back as not configured, enabled, disabled or custom",
                           problem.empty(), problem});
    }

    // --- write, read and remove a synthetic policy under a scratch key, so the
    //     engine is proven end to end without touching a real Windows setting.
    {
        const Policy scratch = {
            1, L"self-test", L"a scratch row",
            HKEY_CURRENT_USER, L"Software\\ZanpharsWinSuite\\HomePolicy\\selftest",
            {{L"OnOff", 3, 1, true}, {L"Other", 7, 2, true}}, 2, L"Immediately", nullptr};

        std::wstring problem;
        std::wstring error;
        if (!Write(scratch, Tri::Enabled, &error)) problem = L"enable failed: " + error;
        else if (Read(scratch) != Tri::Enabled) problem = L"enabled state did not read back";
        else if (!Write(scratch, Tri::Disabled, &error)) problem = L"disable failed: " + error;
        else if (Read(scratch) != Tri::Disabled) problem = L"disabled state did not read back";
        else if (!Write(scratch, Tri::NotConfigured, &error)) problem = L"reset failed: " + error;
        else if (Read(scratch) != Tri::NotConfigured) problem = L"reset state did not read back";
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ZanpharsWinSuite");
        results.push_back({L"a policy round-trips through the registry and can be reset",
                           problem.empty(), problem});
    }

    // --- console mode: a title resolves to exactly one policy, and the three
    //     state words are understood.
    {
        std::wstring problem;
        const std::vector<int> widgets = FindPolicies(L"widgets board");
        Tri state = Tri::Custom;
        if (widgets.size() != 1) problem = L"a partial title did not resolve to one setting";
        else if (FindPolicies(L"Turn off the Widgets board").size() != 1)
            problem = L"an exact title did not resolve on its own";
        else if (!ParseTri(L"disabled", &state) || state != Tri::Disabled)
            problem = L"'disabled' was not understood";
        else if (!ParseTri(L"not-configured", &state) || state != Tri::NotConfigured)
            problem = L"'not-configured' was not understood";
        else if (ParseTri(L"bogus", &state)) problem = L"a nonsense state was accepted";
        else if (!FindPolicies(L"no such policy").empty()) problem = L"a nonsense title matched";
        results.push_back({L"the console mode resolves a title and a state", problem.empty(), problem});
    }

    // --- the handoff: what the unelevated window stages is encoded, parsed and
    //     written by the same code the elevated copy runs.
    {
        const std::vector<Policy> scratch = {
            {1, L"self-test handoff", L"a scratch row", HKEY_CURRENT_USER,
             L"Software\\ZanpharsWinSuite\\HomePolicy\\handoff",
             {{L"One", 1, 0, true}, {L"Two", 1, 0, true}}, 2, L"Immediately", nullptr}};

        std::wstring problem;
        const std::wstring spec = EncodeStagedSpec({0}, {Tri::Enabled});
        if (spec != L"0:1") problem = L"the staged list encoded as \"" + spec + L"\"";
        else if (ApplyStagedSpec(scratch, spec, nullptr, nullptr) != 1)
            problem = L"the staged list did not apply";
        else if (Read(scratch[0]) != Tri::Enabled) problem = L"the value it wrote did not read back";
        else if (ApplyStagedSpec(scratch, L"7:1;x:2;0:9", nullptr, nullptr) != 0)
            problem = L"a malformed staged list wrote something";
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ZanpharsWinSuite");
        results.push_back({L"staged choices survive the handoff and land in the registry",
                           problem.empty(), problem});
    }

    // --- two rows of the catalog agree with their own descriptions.
    {
        std::wstring problem;
        if (kPolicies.size() < 30) problem = L"only " + std::to_wstring(kPolicies.size()) + L" policies";
        else if (PathText(kPolicies.front()).find(L"HKEY_LOCAL_MACHINE") == std::wstring::npos)
            problem = L"the path text lost its hive name";
        results.push_back({L"the catalog carries at least thirty settings", problem.empty(), problem});
    }

    return results;
}

}  // namespace hp
