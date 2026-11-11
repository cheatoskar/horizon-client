// Event dispatch, the current map, online and game-state changes as events.
#include "host/host.h"

#include <string>

namespace horizon {
namespace {
struct MapStore {
    std::string path, uid, name, author, environment;
    HorizonMapInfo info = {};
    bool valid = false;
} g_map;
bool g_lastOnline = false;
int g_lastState = -1;
bool g_eventsReady = false;
} // namespace

void eventsInit() {
    g_lastOnline = false;
    g_lastState = -1;
    g_eventsReady = true;
}

void setCurrentMap(const HorizonMapInfo* map) {
    if (!map) {
        g_map.valid = false;
        return;
    }
    g_map.path = map->path ? map->path : "";
    g_map.uid = map->uid ? map->uid : "";
    g_map.name = map->name ? map->name : "";
    g_map.author = map->author ? map->author : "";
    g_map.environment = map->environment ? map->environment : "";
    g_map.info = *map;
    g_map.info.size = sizeof(HorizonMapInfo);
    g_map.info.path = g_map.path.c_str();
    g_map.info.uid = g_map.uid.c_str();
    g_map.info.name = g_map.name.c_str();
    g_map.info.author = g_map.author.c_str();
    g_map.info.environment = g_map.environment.c_str();
    g_map.valid = true;
}

const HorizonMapInfo* currentMap() { return g_map.valid ? &g_map.info : nullptr; }

bool isOnlineNow() { return game().isOnline() != 0; }

bool runGate() { return state().competitionPatch && game().raceRunning(); }

void dispatchEvent(HorizonEvent& e) {
    e.size = sizeof(HorizonEvent);
    e.is_online = isOnlineNow() ? 1u : 0u;
    if (!e.map) e.map = currentMap();
    if (!e.engine_race) e.engine_race = game().engine(HORIZON_ENGINE_RACE);
    if (!e.engine_challenge) e.engine_challenge = game().engine(HORIZON_ENGINE_CHALLENGE);
    switch (e.type) {
    case HORIZON_EVENT_MAP_LOADED:
        HLOG("event: map loaded \"%s\" (%s)", e.map && e.map->name ? e.map->name : "?", e.map && e.map->uid ? e.map->uid : "");
        break;
    case HORIZON_EVENT_RACE_START: HLOG("event: race start"); break;
    case HORIZON_EVENT_RESPAWN: HLOG("event: respawn (%d in this run)", e.respawns); break;
    case HORIZON_EVENT_CHECKPOINT: HLOG("event: checkpoint %d at %d ms", e.checkpoint_index, e.time_ms); break;
    case HORIZON_EVENT_FINISH: HLOG("event: finish %d ms%s, respawns %d, mode %u", e.time_ms, e.is_pb ? " (new best)" : "", e.respawns, e.play_mode); break;
    case HORIZON_EVENT_RACE_END: HLOG("event: race end (%s, replay %p)", e.end_screen ? "solo end screen" : "network", e.engine_replay); break;
    case HORIZON_EVENT_MENU_ENTERED: HLOG("event: menu"); break;
    case HORIZON_EVENT_REPLAY_SAVED: HLOG("event: replay saved by %s: %s", e.source ? e.source : "?", e.path ? e.path : ""); break;
    case HORIZON_EVENT_ONLINE_CHANGED: HLOG("event: %s", e.is_online ? "online" : "offline"); break;
    case HORIZON_EVENT_GAME_STATE: HLOG("event: game state %s", gameStateText(static_cast<int>(e.game_state))); break;
    default: break;
    }
    pluginsDeliver(e);
}

void eventsFrame() {
    if (!g_eventsReady) return;
    const bool online = isOnlineNow();
    if (online != g_lastOnline) {
        g_lastOnline = online;
        HorizonEvent e = {};
        e.type = HORIZON_EVENT_ONLINE_CHANGED;
        dispatchEvent(e);
    }
    const int s = game().gameState();
    if (s != g_lastState) {
        const int before = g_lastState;
        g_lastState = s;
        HorizonEvent e = {};
        e.type = HORIZON_EVENT_GAME_STATE;
        e.game_state = static_cast<uint32_t>(s);
        dispatchEvent(e);
        if (s == HORIZON_STATE_MAIN_MENU && before != -1) {
            HorizonEvent m = {};
            m.type = HORIZON_EVENT_MENU_ENTERED;
            dispatchEvent(m);
        }
    }
}

} // namespace horizon
