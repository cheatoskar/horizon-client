// Test host for Horizon Scripts: the Node Explorer's stub host (settings map, logs, fake engine graph)
// plus what the script engine needs on top: ABI 1.2 (panels, host flags), a switchable online state,
// queued game-thread calls, recorded game-control calls and a game folder of the test's choice.
#pragma once

#include <string>
#include <vector>

#include "horizon_plugin.h"
#include "stub_host.h"

namespace testhost {

struct Panel {
    std::string key, title;
    HorizonPanelDrawFn draw = nullptr;
    void* user = nullptr;
};

extern bool online;
extern bool actionsAllowed;
extern int gameState;
extern uint32_t hostFlags;
extern std::vector<Panel> panels;
extern std::vector<std::string> playedMaps;
extern int menuReturns;

// game: HORIZON_GAME_*; gameFolder with trailing backslash; dataFolder = gameFolder\horizon\data\scripts\.
void setup(uint32_t game, const std::string& gameFolder);
HorizonHostApi api();
// Runs the queued run_on_game_thread calls (the host does it at the end of a frame).
void runQueued();
// stubhost::logs since the last clearLogs().
const std::string& logs();
void clearLogs();
const char* gameName(uint32_t game);

} // namespace testhost
