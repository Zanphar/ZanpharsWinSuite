// ============================================================================
//  catalog.h — the policy catalog and the registry engine behind Home Policy.
//
//  Windows Home has no gpedit.msc, but most policies the official editor writes
//  are plain registry values under HKLM\SOFTWARE\Policies (or the matching
//  CurrentVersion\Policies keys), and the components that read them are the
//  same components on Home. This catalog names those values, reads them back
//  and writes them - nothing else. No policy file, no service, no command line.
//
//  Every entry is data: a title, one plain sentence, the hive/key/value it owns
//  and the data for the two states. Adding a setting is a table row, not code.
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
#include <vector>

namespace hp {

// ---------------------------------------------------------------------------
//  The catalog
// ---------------------------------------------------------------------------

// One registry value owned by a policy. Almost every policy writes one DWORD;
// a few (the update-deferral pair) write two, so both change together.
struct PolicyValue {
    const wchar_t* name;
    DWORD          whenOn;
    DWORD          whenOff;   // only meaningful when hasOff
    bool           hasOff;    // false: "Disabled" removes the value instead
};

struct Policy {
    int            category;   // index into Categories(), 1-based
    const wchar_t* title;      // what the window shows
    const wchar_t* summary;    // one sentence, no jargon
    HKEY           root;       // HKEY_LOCAL_MACHINE or HKEY_CURRENT_USER
    const wchar_t* key;
    PolicyValue    values[3];
    int            valueCount;
    const wchar_t* effect;     // when it takes hold
    const wchar_t* caveat;     // null, or the honest tag shown on the card
};

const std::vector<Policy>& Policies();
const std::vector<const wchar_t*>& Categories();   // [0] is "All settings"

// ---------------------------------------------------------------------------
//  State of one policy on this machine
// ---------------------------------------------------------------------------

enum class Tri { NotConfigured, Enabled, Disabled, Custom };

// The mapping the reader uses, exposed so the self-test can prove it without
// touching the registry: no value at all is "not configured", every value at
// its on-data is enabled, every value at its off-data is disabled, anything
// else is "custom" (someone set a third value by hand).
Tri Classify(bool anyPresent, bool allOn, bool allOff);

Tri Read(const Policy& policy);

// Writes the requested state. NotConfigured removes the values (and leaves an
// empty key alone - deleting it would be tidier but can race the component
// reading it). Custom is not writable and reports why.
bool Write(const Policy& policy, Tri want, std::wstring* error);

bool IsElevated();

// ---------------------------------------------------------------------------
//  Handing staged choices to the elevated run
//
//  Home Policy opens without elevation, so the machine-wide rows can only be
//  written by the elevated copy it launches from Apply. The staged choices
//  travel as "index:state;index:state" on the command line - the parser lives
//  here, next to the engine, so the selftest can exercise it on a scratch key.
// ---------------------------------------------------------------------------

std::wstring EncodeStagedSpec(const std::vector<int>& indices,
                              const std::vector<Tri>& wanted);

// ---------------------------------------------------------------------------
//  Console mode helpers
// ---------------------------------------------------------------------------

// Every policy whose title contains `text`, case-insensitively. An exact title
// match is returned on its own, so "Turn off the Widgets board" is never
// ambiguous with a longer title that happens to contain it.
std::vector<int> FindPolicies(const std::wstring& text);

// "enabled" / "on", "disabled" / "off", "not-configured" / "default".
bool ParseTri(const std::wstring& text, Tri* out);

// The DWORD at the policy's first value, for reporting what the write left
// behind. False when the value is absent.
bool ReadFirstValue(const Policy& policy, DWORD* data);

// Applies every entry of `spec` to `catalog`. Returns how many were written and
// names the first failure, if any.
int ApplyStagedSpec(const std::vector<Policy>& catalog, const std::wstring& spec,
                    std::wstring* firstError, std::wstring* firstTitle);

// ---------------------------------------------------------------------------
//  Backup
// ---------------------------------------------------------------------------

// Writes a UTF-16 .reg file containing every catalog value that is present
// right now, so a machine can be put back the way it was found. Returns the
// number of values written.
bool WriteBackup(const std::wstring& path, int* valuesWritten, std::wstring* error);

// ---------------------------------------------------------------------------
//  Command-line support
// ---------------------------------------------------------------------------

struct TestResult {
    const wchar_t* name;
    bool           passed;
    std::wstring   detail;
};

std::vector<TestResult> RunSelfTest();

const wchar_t* TriName(Tri state);
std::wstring   PathText(const Policy& policy);

}  // namespace hp
