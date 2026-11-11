#include "gate.h"

#include "horizon_plugin.h"

namespace hs {

Gate decide(const Manifest& m, const GateInput& in) {
    Gate g;
    if (!(m.games & in.game)) {
        g.state = GateState::WrongGame;
        return g;
    }
    if ((m.perms & PermGameControl) && in.cpLoaded && (in.game & HORIZON_GAME_FOREVER)) {
        g.state = GateState::BlockedCp;
        return g;
    }
    if (in.online && m.online == OnlineMode::SoloOnly) {
        g.state = GateState::PausedOnline;
        return g;
    }
    g.gameControl = (m.perms & PermGameControl) && !in.online && !in.cpLoaded && in.hostAllows;
    return g;
}

const char* gateText(GateState s) {
    switch (s) {
    case GateState::WrongGame: return "Not made for this game.";
    case GateState::PausedOnline: return "Paused while online (solo-only).";
    case GateState::BlockedCp: return "Off: Competition Patch is loaded and this script asks for game control.";
    default: return "";
    }
}

} // namespace hs
