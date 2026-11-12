// Controller fix: bindings for a pad that was registered late, exactly as the game gives them to a pad that is
// there at start (ESWC, Sunrise and Original have the same code; static analysis 2026-11-12, see hp_bind.cpp).
//
// What the game does at start (ESWC addresses, Sunrise/Original in the tables of hp_bind.cpp):
//   - CTrackMania::Start stores CTrackManiaRace::InputSetDefaultsBindings (0x6e6560, cdecl(config, device, n)) in
//     the app (+0x258). It calls InputDoBindings(config, 0, device, n) (0x6d12d0): for a pad (device kind 2) it
//     binds Reset/Respawn/Accelerate/Brake/Steer/Horn to the pad's buttons and X axis, like for keyboards.
//   - The defaults pass (0x7171d0, thiscall app, config) walks keyboards, mice and pads of the input port and
//     calls that callback for every device the bindings config has no binding for yet (config+0x2c list of
//     device ids). It runs for every profile's config when the profiles are loaded, so a pad that is there at
//     start gets default bindings, and a pad that was bound before keeps the saved ones (bindings are saved in
//     the profile per device id = hash of the DirectInput instance GUID).
//   - Race start (0x6d5770) builds the race's binding table from the current config (0x716fb0, thiscall app):
//     race+0x144; and an "all objects" table (race+0x148) that maps every pad's buttons 0-3, POV and X/Y axis
//     to the menu actions (pause menu, dialogs).
// For a pad registered late the module calls the same defaults pass for the current config (the pad gets the
// game's defaults if the profile has none for it, saved bindings stay untouched), then appends the pad's
// bindings to the running race's table and its menu entries to every "all objects" table, which is what the
// game's own table code writes for a pad that is there when the table is built. Nothing else is changed.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace hp {

struct CodeSig;
using LogFn = void (*)(void* user, int level, const char* text);

struct BindTable {
    uintptr_t appGlobal;     // CTrackMania* (the app)
    uint32_t appDefaultsCb;  // app field: InputSetDefaultsBindings
    uint32_t appPort;        // app field: input port
    uint32_t appRace;        // app field: current race (0 outside a race)
    uintptr_t setDefaults;   // CTrackManiaRace::InputSetDefaultsBindings (what app+appDefaultsCb holds)
    uintptr_t defaultsPass;  // thiscall(app; config), ret 4: defaults for every device without bindings
    uintptr_t currentConfig; // thiscall(app) -> CInputBindingsConfig* the race uses
    uint32_t raceApp, racePort, raceConfigTable, raceAllTable;
    uintptr_t configTableVtbl; // binding table built from a config
    uintptr_t allTableVtbl;    // "all objects" table
    uintptr_t allActions[5];   // globals: action descs of the "all objects" table for pads (see kAllPad)
    uintptr_t actionList;      // globals: the race action descs in the game's order (log names)
    const CodeSig* code;
    int codeCount;
};

const BindTable& eswcBind();
const BindTable& sunriseBind();
const BindTable& originalBind();

// a binding: action desc, device object
struct BindPair {
    uint32_t action;
    uint32_t object;
};

// the pad entries the game's "all objects" builder adds per pad: device object (kind, index) -> allActions[slot]
struct AllPadEntry {
    uint32_t kind, index, slot;
};
extern const AllPadEntry kAllPad[10];

// name of an action by its position in the game's action list ("Accelerate"), "action N" after the first ten
std::string actionName(int position);

// Appends bindings for the last device index of a binding table (the late pad): what the game's AddBinding writes
// for the last device. Refuses (returns false, *why set) unless the table has a range for devIndex that is the
// last range, empty, and at the end of the bindings. reserve8 = the game's Reserve of {count, ptr, cap} arrays.
bool appendToTable(uint8_t* table, uint32_t devIndex, const BindPair* pairs, int n, uintptr_t reserve8, const char** why);

class Binder {
public:
    Binder(const BindTable* t, uintptr_t exeBase, uintptr_t reserve8, LogFn log, void* logUser);
    bool verify(); // every code signature once; false = bindings stay off (the pad is still registered)
    // the game registered a pad at devIndex of the port: give it the bindings it would get at start
    void apply(uint8_t* port, uint32_t devIndex, const std::wstring& name);

    // what the last apply did (tests, log)
    struct Result {
        bool ok = false;
        bool saved = false;   // the profile had bindings for this pad
        int configPairs = 0;  // the pad's bindings in the config after the defaults pass
        int raceAdded = 0;    // appended to the running race's table
        int menuAdded = 0;    // appended to "all objects" tables
        int menuTables = 0;
    };
    const Result& last() const { return last_; }

private:
    uintptr_t rb(uintptr_t link) const { return base_ - 0x400000 + link; }
    void logf(int level, const char* fmt, ...);

    const BindTable* t_;
    uintptr_t base_;
    uintptr_t reserve8_;
    LogFn log_;
    void* logUser_;
    bool verified_ = false;
    bool failed_ = false;
    Result last_;
};

} // namespace hp
