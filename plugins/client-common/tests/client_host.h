// Test host for the client mods (WR Hunter, Input Display, TMX Browser): the whole HorizonHostApi up to
// ABI 1.3 without a game. A fake engine world in this process's memory (race, local player, player info
// with its checkpoint list, the car, a challenge with its UID in a CMwId table that the plugins find by
// the same instruction pattern as in the games), a fake TM Horizon bridge with TMX fixtures, recorded
// toasts and game-control calls.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "horizon_plugin.h"

namespace chost {

struct World {
    uintptr_t app = 0, race = 0, challenge = 0, player = 0, info = 0, gameMobil = 0, vehicle = 0;
    uintptr_t cpItems = 0;     // the player info's checkpoint list items {time, score}
    uintptr_t ghost = 0;       // one opponent ghost in race.RaceGhosts
    bool forever = true;
};

struct Panel {
    std::string key, title;
    uint32_t flags = 0;
    HorizonPanelDrawFn draw = nullptr;
    void* user = nullptr;
};

struct Toast {
    std::string title, text;
    uint32_t kind = 0;
};

struct BridgeCall {
    std::string method, path;
};

extern World world;
extern std::map<std::string, std::string> settings;
extern std::vector<Panel> panels;
extern std::vector<Toast> toasts;
extern std::vector<BridgeCall> bridgeCalls;
extern std::vector<std::string> playedMaps;
extern std::string logs;
extern bool online;
extern int gameState;
extern int bridgeStateValue;
extern bool menuIsOpen;
extern HorizonStyle style;

// game: HORIZON_GAME_*; dataRoot with trailing backslash (the plugin's data folder is dataRoot + id + "\").
void setup(uint32_t game, const std::string& dataRoot, const std::string& pluginId);
HorizonHostApi api();
// Runs queued game-thread calls and delivers the bridge answers (end of a frame).
void endFrame();
const char* gameName(uint32_t game);

// ---- the fake world (memory laid out with the plugins' default offsets)
void buildWorld(bool forever, const char* uid, const wchar_t* mapName);
void setRun(int state, int timeMs, const std::vector<int>& cps);   // RaceState, clock, checkpoint list
void setCar(float steer, float gas, float brake, int speedKmh, int gear, float rpm);
void setGhost(int raceTime, const std::vector<int>& cps);
void clearRace();                                                   // menus: no race object

// ---- events to the plugin
HorizonMapInfo mapInfo(const char* name, const char* uid, int authorTime);
HorizonEvent event(uint32_t type);

// ---- the fake bridge: fixture answers for the docs/client-bridge.md paths
// A small .Replay.Gbx (uncompressed body) with the given UID, race time and checkpoint times.
std::string makeReplay(const std::string& uid, int raceTime, const std::vector<int>& cps, const std::string& driver);
// Handler override (return the HTTP status, set type and body); nullptr = the built-in fixtures.
extern std::function<int(const std::string& method, const std::string& path, std::string* type, std::string* body)> bridgeHandler;
int fixtureAnswer(const std::string& method, const std::string& path, std::string* type, std::string* body);

} // namespace chost
