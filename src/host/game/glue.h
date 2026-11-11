// gamectl (legacy-randomizer's game control module) results as Horizon results.
#pragma once

#include "gamectl/gamectl.h"
#include "horizon_plugin.h"

namespace horizon {

inline HorizonResult fromGamectl(gamectl::Result r) {
    switch (r) {
    case gamectl::Result::Ok: return HORIZON_OK;
    case gamectl::Result::NotSupported: return HORIZON_ERR_NOT_SUPPORTED;
    case gamectl::Result::WrongState: return HORIZON_ERR_WRONG_STATE;
    case gamectl::Result::FileError: return HORIZON_ERR_INVALID_ARG;
    default: return HORIZON_ERR_FAILED;
    }
}

inline HorizonGameState fromGamectl(gamectl::State s) {
    switch (s) {
    case gamectl::State::Loading: return HORIZON_STATE_LOADING;
    case gamectl::State::MainMenu: return HORIZON_STATE_MAIN_MENU;
    case gamectl::State::SoloRace: return HORIZON_STATE_SOLO_RACE;
    case gamectl::State::NetRace: return HORIZON_STATE_NET_RACE;
    case gamectl::State::Editor: return HORIZON_STATE_EDITOR;
    default: return HORIZON_STATE_UNKNOWN;
    }
}

} // namespace horizon
