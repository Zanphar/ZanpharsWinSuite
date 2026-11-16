// ============================================================================
//  turbo.h — the Turbo Boost engine.
//
//  Applies a set of reversible resource tweaks (power plan, service parking,
//  Game DVR registry values, an instant RAM pass), captures "what was there
//  before" into persisted restore records, and can undo everything. A worker
//  thread keeps memory/CPU instrumentation current and optionally releases
//  RAM automatically when load runs high.
// ============================================================================

#pragma once
#include "common.h"
#include "config.h"

namespace stc {

// Optional tweak groups (checkbox rows on the dashboard).
enum TweakBits : unsigned {
    MaskPowerPlan  = 1u << 0,
    MaskServices   = 1u << 1,
    MaskGameDVR    = 1u << 2,
    MaskRamOnBoost = 1u << 3,
    MaskAll        = 15u,
};

// A memory-heavy image as shown in the process list.
struct ProcItem {
    std::wstring       Name;        // executable name
    unsigned long long WorkingSet = 0;
};

// Point-in-time instrumentation handed to the UI.
struct MemoryInfo {
    unsigned long long Total  = 0;    // installed RAM, bytes
    unsigned long long Avail  = 0;    // free RAM, bytes
    double             LoadPercent = 0.0;
    double             CpuPercent  = 0.0;
};

struct Status {
    bool     Elevated    = false;
    bool     Turbo       = false;   // boost is active
    unsigned TweakMask   = 0;
    unsigned ParkedCount = 0;       // services recorded as parked
    unsigned long long LastFreedBytes = 0;  // total bytes freed via release
    bool     AutoRelease = false;
    MemoryInfo           Mem;
    std::vector<ProcItem> Procs;
};

class Engine {
 public:
    static Engine& Instance();

    // Loads options and persisted boost state. When a boost was active at
    // exit it is re-applied (idempotently, keeping the original restore
    // records). Call once before anything else.
    bool Prepare(const DataDirs& dirs, HWND notify, bool elevated);
    void Shutdown();

    // Re-attaches the refresh-notification window (after the UI exists).
    void SetNotify(HWND window);

    // Thread-safe point-in-time view for the UI.
    Status Snapshot();

    // Configures option switches without applying them.
    void SetOptions(unsigned tweakMask, bool autoRelease);

    // Applies the configured tweak groups. Idempotent: already applied
    // groups keep their original restore records; newly ticked groups are
    // applied in place. Returns an empty string on success, otherwise a
    // user-facing error/problem message.
    std::wstring Boost();

    // Undoes every recorded tweak and turns the boost off.
    std::wstring TurboOff();

    // Runs a memory-release pass right now. Returns an empty string on
    // success, otherwise a user-facing error message.
    std::wstring FreeMemory();

 private:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace stc
