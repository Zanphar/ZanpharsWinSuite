// ============================================================================
//  config.h — persisted application settings and boost bookkeeping.
// ============================================================================

#pragma once
#include "common.h"
#include <string>
#include <vector>
#include <filesystem>

namespace stc {

// ---------------------------------------------------------------------------
// User options (the [Options] section).
// ---------------------------------------------------------------------------

struct Config {
    unsigned TweakMask   = 0;    // bitmask of applied tweak groups
    bool     AutoRelease = false; // free RAM automatically at high load
};

// ---------------------------------------------------------------------------
// What needs to be undone when the boost is switched off. Captured while a
// tweak group is applied and persisted so the boost survives a restart.
// ---------------------------------------------------------------------------

struct ServiceRecord {
    std::wstring Name;              // service key name, e.g. "WSearch"
    DWORD        StartType = 0;     // original start type
    bool         WasRunning = false;
};

struct RegRecord {
    std::wstring Key;               // full registry key
    std::wstring Value;             // value name
    DWORD        Type = 0;          // original type
    std::wstring Data;              // original data (string form)
    bool         Existed = false;   // absent values are deleted on restore
};

struct BoostRestore {
    std::wstring PowerPlan;         // active plan GUID before the boost ('' = none)
    std::vector<ServiceRecord> Services;
    std::vector<RegRecord>     Regs;

    bool Empty() const {
        return PowerPlan.empty() && Services.empty() && Regs.empty();
    }
};

// Prepared data locations handed to the rest of the application.
struct DataDirs {
    std::filesystem::path data;   // %ProgramData%\SystemTurbo
    std::filesystem::path ini;    // data / systemturbo.ini
};

// Resolves (and creates) the per-machine data directory.
DataDirs ResolveDataDirs();

Config   LoadConfig(const DataDirs& dirs);
void     SaveConfig(const DataDirs& dirs, const Config& cfg);

BoostRestore         LoadBoost(const DataDirs& dirs);
void                 SaveBoost(const DataDirs& dirs, const BoostRestore& br);
void                 ClearBoost(const DataDirs& dirs);
bool                 LoadBoostActive(const DataDirs& dirs);
void                 SaveBoostActive(const DataDirs& dirs, bool active);
unsigned long long   LoadLastFreed(const DataDirs& dirs);       // bytes
void                 SaveLastFreed(const DataDirs& dirs, unsigned long long bytes);

}  // namespace stc
