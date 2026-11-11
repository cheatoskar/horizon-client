#include "test_host.h"

#include <windows.h>
#include <string.h>

#include <utility>

namespace testhost {

bool online = false;
bool actionsAllowed = true;
int gameState = HORIZON_STATE_SOLO_RACE;
uint32_t hostFlags = 0;
std::vector<Panel> panels;
std::vector<std::string> playedMaps;
int menuReturns = 0;

namespace {

std::string g_gameFolder, g_dataFolder;
std::wstring g_dataFolderW;
std::vector<std::pair<HorizonCallback, void*>> g_queue;

int32_t HORIZON_CALL t_is_online(void) { return online ? 1 : 0; }
int32_t HORIZON_CALL t_actions_allowed(HorizonPluginHandle) { return actionsAllowed ? 1 : 0; }
int32_t HORIZON_CALL t_get_game_state(void) { return gameState; }
int32_t HORIZON_CALL t_menu_open(void) { return 1; }
const char* HORIZON_CALL t_data_folder(HorizonPluginHandle) { return g_dataFolder.c_str(); }
const wchar_t* HORIZON_CALL t_data_folder_w(HorizonPluginHandle) { return g_dataFolderW.c_str(); }
void HORIZON_CALL t_run_on_game_thread(HorizonPluginHandle, HorizonCallback cb, void* user) { g_queue.push_back({cb, user}); }
HorizonResult HORIZON_CALL t_play_map_file(HorizonPluginHandle, const char* path, uint32_t) {
    playedMaps.push_back(path ? path : "");
    return HORIZON_OK;
}
HorizonResult HORIZON_CALL t_return_to_menu(HorizonPluginHandle) {
    menuReturns++;
    return HORIZON_OK;
}
HorizonResult HORIZON_CALL t_register_panel(HorizonPluginHandle, const HorizonPanelDef* d) {
    if (!d || !d->key || !d->title || !d->draw) return HORIZON_ERR_INVALID_ARG;
    for (Panel& p : panels)
        if (p.key == d->key) {
            p.title = d->title;
            p.draw = d->draw;
            p.user = d->user;
            return HORIZON_OK;
        }
    panels.push_back(Panel{d->key, d->title, d->draw, d->user});
    return HORIZON_OK;
}
void HORIZON_CALL t_unregister_panel(HorizonPluginHandle, const char*) {}
int32_t HORIZON_CALL t_panel_visible(HorizonPluginHandle, const char*) { return 1; }
void HORIZON_CALL t_set_host_flag(HorizonPluginHandle, uint32_t, int32_t) {}
uint32_t HORIZON_CALL t_host_flags(void) { return hostFlags; }

} // namespace

const char* gameName(uint32_t game) {
    switch (game) {
    case HORIZON_GAME_ESWC: return "TrackMania Nations ESWC";
    case HORIZON_GAME_SUNRISE: return "TrackMania Sunrise";
    case HORIZON_GAME_ORIGINAL: return "TrackMania Original";
    case HORIZON_GAME_TMUF: return "TrackMania United Forever";
    default: return "TrackMania Nations Forever";
    }
}

void setup(uint32_t game, const std::string& gameFolder) {
    stubhost::setup();
    g_gameFolder = gameFolder;
    g_dataFolder = gameFolder + "horizon\\data\\scripts\\";
    g_dataFolderW.assign(g_dataFolder.begin(), g_dataFolder.end());
    stubhost::gameInfo.game = game;
    stubhost::gameInfo.game_name = gameName(game);
    stubhost::gameInfo.game_folder = g_gameFolder.c_str();
    online = false;
    actionsAllowed = true;
    gameState = HORIZON_STATE_SOLO_RACE;
    hostFlags = 0;
    panels.clear();
    playedMaps.clear();
    menuReturns = 0;
    g_queue.clear();
}

HorizonHostApi api() {
    HorizonHostApi h = stubhost::api();
    h.abi_minor = 2;
    h.is_online = t_is_online;
    h.actions_allowed = t_actions_allowed;
    h.get_game_state = t_get_game_state;
    h.menu_open = t_menu_open;
    h.data_folder = t_data_folder;
    h.data_folder_w = t_data_folder_w;
    h.run_on_game_thread = t_run_on_game_thread;
    h.play_map_file = t_play_map_file;
    h.return_to_menu = t_return_to_menu;
    h.register_panel = t_register_panel;
    h.unregister_panel = t_unregister_panel;
    h.panel_visible = t_panel_visible;
    h.set_host_flag = t_set_host_flag;
    h.host_flags = t_host_flags;
    return h;
}

void runQueued() {
    auto q = std::move(g_queue);
    g_queue.clear();
    for (auto& c : q) c.first(c.second);
}

const std::string& logs() { return stubhost::logs; }
void clearLogs() { stubhost::logs.clear(); }

} // namespace testhost
