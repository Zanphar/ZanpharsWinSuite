// ============================================================================
//  harden.h — the hardening engine: the catalog of checks, the audit that
//  reads them without touching anything, and the apply/revert pair that keeps
//  a journal of whatever it overwrites.
//
//  Every check is one setting that either closes an attack path, removes work
//  the machine does for nothing, or helps Windows put itself back on its feet.
//  Nothing here runs a command line or a shell: the engine talks to the
//  registry and the service manager directly, so a check cannot be talked into
//  doing something it does not name.
// ============================================================================

#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

namespace hw {

// ---------------------------------------------------------------------------
//  The catalog
// ---------------------------------------------------------------------------

enum class Group { Security, Performance, Reliability };
enum class Kind { Registry, Service };

// One entry in the catalog. Registry checks name a hive, a key, a value and the
// data the hardened state holds; the service check has no registry target
// because it goes through the service manager instead.
struct Check {
    const wchar_t* id;               // stable: keys the journal and the settings
    Group          group;
    const wchar_t* name;             // what the row says
    const wchar_t* why;              // one line: what it buys and what it costs
    Kind           kind;
    HKEY           root;             // registry checks only
    const wchar_t* key;
    const wchar_t* value;
    DWORD          type;             // REG_DWORD or REG_SZ
    DWORD          dword;            // REG_DWORD payload
    const wchar_t* text;             // REG_SZ payload
    bool           adminOnly;        // HKLM write, so it needs elevation
    bool           absentIsHardened; // Windows' own default already does this
};

const std::vector<Check>& Checks();
const wchar_t* GroupName(Group group);

// ---------------------------------------------------------------------------
//  State of one check on this machine
// ---------------------------------------------------------------------------

struct State {
    bool hardened   = false;
    bool needsAdmin = false;   // open, and this process cannot close it
    bool journalled = false;   // an original value is saved, so Revert has work
    std::wstring current;      // what the setting reads now, for the tooltip
};

bool IsElevated();

// ---------------------------------------------------------------------------
//  Engine
// ---------------------------------------------------------------------------

class Engine {
 public:
    State              Audit(const Check& check) const;
    std::vector<State> AuditAll() const;

    bool Apply(const Check& check, std::wstring* error);
    bool Revert(const Check& check, std::wstring* error);

    // What a batch did, for the caller to word however it likes.
    struct Batch {
        int          changed = 0;
        int          skipped = 0;   // already hardened / no saved original
        std::wstring refused;       // one " name - reason." per refusal
    };

    // One row per call to Apply/Revert, and the only place the batch skipping
    // rules live: a row already in the wanted state is counted rather than
    // written again, and a revert leaves alone a row this tool has no original
    // for. The window passes the ticked rows; nothing else loops over them.
    Batch ApplyChecks(const std::vector<const Check*>& checks);
    Batch RevertChecks(const std::vector<const Check*>& checks);

    // Number of registry or service writes this instance has made. The audit
    // must never move it; the self-test asserts exactly that.
    int Writes() const { return writes_; }

 private:
    mutable int writes_ = 0;
};

// ---------------------------------------------------------------------------
//  Self-test
// ---------------------------------------------------------------------------

enum class Outcome { Pass, Fail, Skip };

struct TestResult {
    const wchar_t* name;
    Outcome        outcome;
    std::wstring   detail;   // the failure reason, or a note on a skip
};

// Runs the checks that need no administrator rights and change nothing that
// outlives the call: catalog sanity, the journal round-trip on a scratch key,
// and the service-action encoding.
std::vector<TestResult> RunSelfTest();

namespace detail {
// Failure actions travel through the journal as bytes, so they are encoded and
// decoded on the way in and out; the self-test round-trips both directions.
std::vector<BYTE> EncodeFailureActions(
    DWORD resetSeconds, const std::vector<std::pair<DWORD, DWORD>>& actions);
bool DecodeFailureActions(const std::vector<BYTE>& blob, DWORD* resetSeconds,
                          std::vector<std::pair<DWORD, DWORD>>* actions);
}  // namespace detail

}  // namespace hw
