// When a script may run and what it may do: the online gate (solo-only scripts pause online), the
// Competition Patch rule (Forever + CP loaded: scripts with game-control stay off, reads stay allowed)
// and the game list. Pure: the plugin passes the facts in.
#pragma once

#include <stdint.h>

#include "manifest.h"

namespace hs {

struct GateInput {
    uint32_t game = 0;          // HORIZON_GAME_* bit of the running game
    bool online = false;        // host->is_online()
    bool cpLoaded = false;      // Competition Patch module loaded (Forever)
    bool hostAllows = true;     // host->actions_allowed(plugin) (host's own online/CP run gate)
};

enum class GateState {
    Run,           // runs normally
    WrongGame,     // not made for this game: never loaded
    PausedOnline,  // solo-only script while online: no callbacks until offline again
    BlockedCp,     // asks for game-control while Competition Patch is loaded: never loaded
};

struct Gate {
    GateState state = GateState::Run;
    bool gameControl = false;   // Game::PlayMapFile / ReturnToMenu allowed right now
};

Gate decide(const Manifest& m, const GateInput& in);
const char* gateText(GateState s); // one short sentence for the Scripts tab

} // namespace hs
