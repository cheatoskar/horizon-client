// A fake engine object graph for testing the Node Explorer without a game.
//
// It lays out CMwClassInfo / SMwParamInfo records and objects (vtable with `mov eax, imm32; ret` stubs)
// exactly like the 32-bit games do (tm-modding-knowledge modding/reflection.md), in memory of this
// process. The class list head lives in a global that a never-called function stores with the same
// instruction pattern as the games' registration code, so the vendored reflect module's scan for an
// unknown exe (reflect::init with a timestamp it does not know) finds it, in the test exe and in the
// plugin DLL alike.
#pragma once

#include <stdint.h>

namespace fake {

struct Graph {
    uintptr_t app = 0;        // CTrackMania
    uintptr_t race = 0;       // CTrackManiaRace
    uintptr_t challenge = 0;  // CGameCtnChallenge
    uintptr_t player = 0;     // App.CurrentPlayground.GameTerminals[0].ControlledPlayer
    uintptr_t guardPage = 0;  // a PAGE_GUARD page some members point into (must stay guarded)
    int classCount = 0;
};

// Builds (or rebuilds) the graph. forever: TMNF type numbers, else ESWC/Sunrise numbers.
const Graph& build(bool forever);
// True while the guard page still has PAGE_GUARD set (nobody touched it).
bool guardIntact();

} // namespace fake
