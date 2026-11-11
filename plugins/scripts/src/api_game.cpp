// Game namespace: state, online, the running map, and the two game-control calls (game-control
// permission, offline only, never with Competition Patch). Nothing here touches input, timing or physics.
#include <windows.h>
#include <string.h>

#include <string>

#include "angelscript.h"
#include "engine.h"

namespace hs {
namespace {

const HorizonHostApi* host() {
    Manager* m = currentManager();
    return m ? m->host() : nullptr;
}

int gameState() {
    if (!requirePerm(PermReadGame, "Game::GetState")) return 0;
    const HorizonHostApi* h = host();
    return h && h->get_game_state ? h->get_game_state() : 0;
}

bool isOnline() {
    const HorizonHostApi* h = host();
    return h && h->is_online && h->is_online() != 0;
}

bool inRace() {
    if (!requirePerm(PermReadGame, "Game::InRace")) return false;
    const HorizonHostApi* h = host();
    const int st = h && h->get_game_state ? h->get_game_state() : 0;
    return st == HORIZON_STATE_SOLO_RACE || st == HORIZON_STATE_NET_RACE;
}

MapData* currentMap() {
    if (!requirePerm(PermReadGame, "Game::CurrentMap")) return nullptr;
    Manager* m = currentManager();
    return m && m->hasMap ? &m->map : nullptr;
}

std::string gameName() {
    Manager* m = currentManager();
    switch (m ? m->game : 0) {
    case HORIZON_GAME_ESWC: return "eswc";
    case HORIZON_GAME_SUNRISE: return "sunrise";
    case HORIZON_GAME_ORIGINAL: return "original";
    case HORIZON_GAME_TMNF: return "tmnf";
    case HORIZON_GAME_TMUF: return "tmuf";
    default: return "";
    }
}

std::string gameTitle() {
    const HorizonHostApi* h = host();
    const HorizonGameInfo* gi = h && h->game_info ? h->game_info() : nullptr;
    return gi && gi->game_name ? gi->game_name : "";
}

bool isForever() {
    Manager* m = currentManager();
    return m && (m->game & HORIZON_GAME_FOREVER);
}

bool canControl() {
    Script* s = currentScript();
    return s && (s->manifest.perms & PermGameControl) && s->gameControlNow;
}

// Checks everything a game-control call needs; sets an exception for a missing permission, returns
// false (with a log line) when it is only not allowed right now.
bool controlAllowed(const char* what) {
    if (!requirePerm(PermGameControl, what)) return false;
    Manager* m = currentManager();
    Script* s = currentScript();
    const HorizonHostApi* h = host();
    if (!m || !s || !h) return false;
    if (!s->gameControlNow || !h->actions_allowed(h->plugin) || h->is_online()) {
        m->scriptLog(*s, HORIZON_LOG_WARN, std::string(what) + " is not allowed now (online, Competition Patch or the host's run gate)");
        return false;
    }
    return true;
}

struct PlayRequest {
    std::string path;
    std::string script;
};

void HORIZON_CALL runPlay(void* user) {
    PlayRequest* r = static_cast<PlayRequest*>(user);
    Manager* m = currentManager();
    const HorizonHostApi* h = m ? m->host() : nullptr;
    if (h) {
        const HorizonResult res = h->play_map_file(h->plugin, r->path.c_str(), 0);
        h->log(h->plugin, res == HORIZON_OK ? HORIZON_LOG_INFO : HORIZON_LOG_WARN, "[%s] PlayMapFile(%s): %d", r->script.c_str(), r->path.c_str(), res);
    }
    delete r;
}

void HORIZON_CALL runMenu(void* user) {
    std::string* id = static_cast<std::string*>(user);
    Manager* m = currentManager();
    const HorizonHostApi* h = m ? m->host() : nullptr;
    if (h) {
        const HorizonResult res = h->return_to_menu(h->plugin);
        h->log(h->plugin, res == HORIZON_OK ? HORIZON_LOG_INFO : HORIZON_LOG_WARN, "[%s] ReturnToMenu: %d", id->c_str(), res);
    }
    delete id;
}

bool endsWithNoCase(const std::string& s, const char* suf) {
    const size_t n = strlen(suf);
    return s.size() >= n && _stricmp(s.c_str() + s.size() - n, suf) == 0;
}

// Queued for the end of the frame (the host's safe point), not run inside the script's callback.
bool playMapFile(const std::string& path) {
    if (!controlAllowed("Game::PlayMapFile")) return false;
    if (path.empty() || path.size() > 1024 || !(endsWithNoCase(path, ".gbx"))) {
        scriptError("Game::PlayMapFile needs a .Gbx map file");
        return false;
    }
    const HorizonHostApi* h = host();
    PlayRequest* r = new PlayRequest{path, currentScript()->id};
    h->run_on_game_thread(h->plugin, runPlay, r);
    return true;
}

bool returnToMenu() {
    if (!controlAllowed("Game::ReturnToMenu")) return false;
    const HorizonHostApi* h = host();
    h->run_on_game_thread(h->plugin, runMenu, new std::string(currentScript()->id));
    return true;
}

} // namespace

#define R(x)                                                                                                                                         \
    do {                                                                                                                                             \
        if ((x) < 0) return false;                                                                                                                   \
    } while (0)

bool registerGameApi(asIScriptEngine* e) {
    R(e->SetDefaultNamespace("Game"));
    R(e->RegisterEnum("State"));
    const struct {
        const char* name;
        int value;
    } states[] = {{"Unknown", HORIZON_STATE_UNKNOWN}, {"MainMenu", HORIZON_STATE_MAIN_MENU}, {"SoloRace", HORIZON_STATE_SOLO_RACE},
                  {"EndRace", HORIZON_STATE_END_RACE}, {"NetRace", HORIZON_STATE_NET_RACE},     {"Editor", HORIZON_STATE_EDITOR},
                  {"Replay", HORIZON_STATE_REPLAY},    {"Loading", HORIZON_STATE_LOADING}};
    for (const auto& s : states) R(e->RegisterEnumValue("State", s.name, s.value));

    // Game::Map: owned by the plugin, one instance updated in place (no reference counting)
    R(e->RegisterObjectType("Map", 0, asOBJ_REF | asOBJ_NOCOUNT));
    R(e->RegisterObjectProperty("Map", "const string name", asOFFSET(MapData, name)));
    R(e->RegisterObjectProperty("Map", "const string author", asOFFSET(MapData, author)));
    R(e->RegisterObjectProperty("Map", "const string uid", asOFFSET(MapData, uid)));
    R(e->RegisterObjectProperty("Map", "const string path", asOFFSET(MapData, path)));
    R(e->RegisterObjectProperty("Map", "const string environment", asOFFSET(MapData, environment)));
    R(e->RegisterObjectProperty("Map", "const int authorTime", asOFFSET(MapData, authorTime)));
    R(e->RegisterObjectProperty("Map", "const int goldTime", asOFFSET(MapData, goldTime)));
    R(e->RegisterObjectProperty("Map", "const int silverTime", asOFFSET(MapData, silverTime)));
    R(e->RegisterObjectProperty("Map", "const int bronzeTime", asOFFSET(MapData, bronzeTime)));
    R(e->RegisterObjectProperty("Map", "const int authorScore", asOFFSET(MapData, authorScore)));
    R(e->RegisterObjectProperty("Map", "const bool isStunts", asOFFSET(MapData, isStunts)));

    R(e->RegisterGlobalFunction("State GetState()", asFUNCTION(gameState), asCALL_CDECL));
    R(e->RegisterGlobalFunction("bool IsOnline()", asFUNCTION(isOnline), asCALL_CDECL));
    R(e->RegisterGlobalFunction("bool InRace()", asFUNCTION(inRace), asCALL_CDECL));
    R(e->RegisterGlobalFunction("Map@ CurrentMap()", asFUNCTION(currentMap), asCALL_CDECL));
    R(e->RegisterGlobalFunction("string Name()", asFUNCTION(gameName), asCALL_CDECL));
    R(e->RegisterGlobalFunction("string Title()", asFUNCTION(gameTitle), asCALL_CDECL));
    R(e->RegisterGlobalFunction("bool IsForever()", asFUNCTION(isForever), asCALL_CDECL));
    R(e->RegisterGlobalFunction("bool CanControl()", asFUNCTION(canControl), asCALL_CDECL));
    R(e->RegisterGlobalFunction("bool PlayMapFile(const string &in path)", asFUNCTION(playMapFile), asCALL_CDECL));
    R(e->RegisterGlobalFunction("bool ReturnToMenu()", asFUNCTION(returnToMenu), asCALL_CDECL));
    R(e->SetDefaultNamespace(""));
    return true;
}

} // namespace hs
