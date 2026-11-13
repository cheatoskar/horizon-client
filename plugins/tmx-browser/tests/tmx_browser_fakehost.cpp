// TMX Browser against the fake host (plugins\client-common\tests\client_host): loads the real DLL, drives it
// through its test hook with a fake TMX (fake_tmx.h, no network), draws the panels in an ImGui window and checks
// the TMX requests, results, thumbnails and their disk cache, Play (legacy: download + play_map_file; Forever:
// TMX's tmtp link handed to the game), the online/race gates and "Play later", the map file check, files,
// favourites across Shutdown/Init and texture release. PASS/FAIL per check, exit code = failures.
// Usage: tmx_browser_fakehost.exe [game ...]   (eswc sunrise original tmnf tmuf; default all five)
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "client_host.h"
#include "fake_tmx.h"
#include "imgui.h"
#include "tmx_logic.h"

typedef int32_t(__cdecl* HookFn)(const char*, char*, int32_t);
typedef const HorizonPluginInfo*(__cdecl* InfoFn)(void);
typedef int32_t(__cdecl* InitFn)(const HorizonHostApi*);
typedef void(__cdecl* ShutdownFn)(void);
typedef void(__cdecl* SetEnabledFn)(int32_t);

static int g_fail = 0, g_pass = 0;
static std::string g_game;
static void check(bool ok, const std::string& name) {
    printf("%s [%s] %s\n", ok ? "PASS" : "FAIL", g_game.c_str(), name.c_str());
    if (ok) g_pass++;
    else g_fail++;
}

static HMODULE g_dll;
static InfoFn pInfo;
static InitFn pInit;
static ShutdownFn pShutdown;
static SetEnabledFn pSetEnabled;
static HookFn pHook;
static faketmx::TestNetFn pTestNet;
static HorizonHostApi g_api;

static std::string hook(const char* cmd) {
    char buf[2048] = "";
    pHook(cmd, buf, sizeof(buf));
    return buf;
}
static int field(const std::string& st, const char* key) {
    const std::string k = std::string(key) + "=";
    const size_t p = st.find(" " + k);
    const size_t at = st.compare(0, k.size(), k) == 0 ? 0 : (p == std::string::npos ? std::string::npos : p + 1);
    return at == std::string::npos ? -9999 : atoi(st.c_str() + at + k.size());
}
static std::string strField(const std::string& st, const char* key) {
    const std::string k = std::string(key) + "=\"";
    const size_t p = st.find(k);
    if (p == std::string::npos) return "";
    const size_t e = st.find('"', p + k.size());
    return st.substr(p + k.size(), e == std::string::npos ? std::string::npos : e - p - k.size());
}
static bool fileExists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static void frame(const char* panelKey, int times = 1) {
    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < times; i++) {
        io.DisplaySize = ImVec2(1600, 900);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(100, 50));
        ImGui::SetNextWindowSize(ImVec2(1200, 800));
        ImGui::Begin("TMX Browser test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        for (const chost::Panel& p : chost::panels)
            if (p.key == panelKey) p.draw(p.user);
        ImGui::End();
        ImGui::Render();
        chost::endFrame();
    }
}

static void runGame(uint32_t game, const char* code, const std::string& root) {
    g_game = code;
    const bool forever = (game & HORIZON_GAME_FOREVER) != 0;
    const std::string host = game == HORIZON_GAME_ESWC ? "nations.tm-exchange.com" : game == HORIZON_GAME_SUNRISE ? "sunrise.tm-exchange.com"
                             : game == HORIZON_GAME_ORIGINAL ? "original.tm-exchange.com" : game == HORIZON_GAME_TMNF ? "tmnf.exchange" : "tmuf.exchange";
    const std::string dataRoot = root + code + "\\";
    system(("rd /s /q \"" + dataRoot + "\" 2>nul").c_str());
    tmx::createDirs(dataRoot);
    chost::setup(game, dataRoot, "tmx-browser");
    g_api = chost::api();
    const std::string dataFolder = dataRoot + "tmx-browser\\";
    faketmx::state() = faketmx::State();
    pTestNet(&faketmx::fetch, &faketmx::link);

    const HorizonPluginInfo* info = pInfo();
    check(info && std::string(info->id) == "tmx-browser" && (info->games & game) && (info->flags & HORIZON_FLAG_ONLINE_OK) && info->abi == HORIZON_ABI, "Info: id, games, online flag");
    check(pInit(&g_api) == 0, "Init");
    check(chost::panels.size() == 2 && chost::panels[0].key == "browse" && chost::panels[1].key == "favourites" && (chost::panels[0].flags & HORIZON_PANEL_MENU_PAGE), "two menu-page panels registered");

    // ---- search: straight to the game's exchange, no TM Horizon
    std::string st = hook("search a");
    const std::string& u0 = faketmx::state().urls.empty() ? std::string() : faketmx::state().urls.back();
    check(u0.compare(0, 8 + host.size() + 12, "https://" + host + "/api/tracks?") == 0 && u0.find("&name=a") != std::string::npos && u0.find("inunlimiter=0") != std::string::npos &&
              u0.find("HasThumbnail%2CImages") != std::string::npos,
          "search goes to " + host + ": " + u0);
    check(chost::bridgeCalls.empty(), "no TM Horizon bridge request");
    check(field(st, "loading") == 1, "loading while the answer is out");
    st = hook("state");
    check(field(st, "results") == 12 && field(st, "more") == 1 && field(st, "loading") == 0, "12 results parsed, more = 1");

    // ---- thumbnails: lazy, a few per frame, decoded off the game thread, cached on disk
    const int pics = forever ? 12 : 11; // old sites: map 1011 has no screenshot
    for (int i = 0; i < 8; i++) frame("browse");
    st = hook("state");
    check(field(st, "thumbs") == pics && field(st, "created") == pics, "thumbnails created: " + std::to_string(field(st, "thumbs")) + " of " + std::to_string(pics));
    check(faketmx::count("/trackshow/") == pics, "one picture request per card with a picture");
    check(faketmx::count(forever ? "/thumbnail" : "/image/1") == pics, forever ? "Forever: the map's own small thumbnail" : "old site: the screenshot (image/1)");
    check(!forever == (field(st, "noThumb") == 0 && faketmx::count("/trackshow/1011/") == 0), "a map without a picture is not asked for");
    check(fileExists(dataFolder + "thumbs\\" + (game == HORIZON_GAME_ESWC ? "tmn" : game == HORIZON_GAME_SUNRISE ? "tms" : game == HORIZON_GAME_ORIGINAL ? "tmo" : code) + "-1000.img"),
          "picture cached on disk");
    const int picCalls = faketmx::count("/trackshow/");
    frame("browse", 3);
    check(faketmx::count("/trackshow/") == picCalls, "no repeated picture requests once loaded");

    // ---- paging (TMX cursor: after=<last id>)
    hook("more");
    st = hook("state");
    check(field(st, "results") == 24 && field(st, "more") == 0 && field(st, "page") == 2, "More appends page 2 (24 results)");
    check(faketmx::state().urls.size() > 0 && faketmx::count("&after=1011") == 1, "page 2 asks with after=1011");
    frame("browse", 2);

    // ---- detail
    st = hook("select 2");
    check(strField(st, "playblock").empty() && st.find("detail=browse:1001") != std::string::npos, "select 2 opens the detail of map 1001");
    frame("browse", 2);
    chost::playedMaps.clear();
    chost::toasts.clear();

    const std::string folder = forever ? dataRoot + "user\\Tracks\\Challenges\\Downloaded\\_Horizon\\" : dataRoot + "game\\GameData\\Tracks\\Challenges\\Downloaded\\_Horizon\\";
    const std::string expect = folder + "Speedway Echo (1001).Challenge.Gbx";
    if (!forever) {
        // ---- play: download, check, save, then play_map_file two frames later
        hook("play");
        st = hook("state");
        check(field(st, "starting") >= 1 && chost::playedMaps.empty() && strField(st, "msg") == "Loading map...",
              "map saved, the start waits for the next frames (Loading map...)");
        hook("pump");
        hook("pump");
        check(chost::playedMaps.size() == 1 && chost::playedMaps[0] == expect, "play_map_file called with " + (chost::playedMaps.empty() ? std::string("nothing") : chost::playedMaps[0]));
        std::string bytes;
        tmx::readFile(expect, bytes);
        check(bytes.compare(0, 3, "GBX") == 0 && bytes.size() >= 8 && bytes.compare(bytes.size() - 4, 4, "1001") == 0, "map file written with the downloaded bytes");
        check(!chost::toasts.empty() && chost::toasts.back().kind == HORIZON_TOAST_OK, "success toast");
        check(strField(hook("state"), "msg") == "Map started", "status: Map started");
        check(faketmx::count("/trackplay/") == 0 && faketmx::state().links.empty(), "no tmtp link on this game");
    } else {
        // ---- play on Forever: TMX's play link, handed to the game; no download, no play_map_file
        hook("play");
        st = hook("state");
        st = hook("state");
        const std::string want = std::string("tmtp:///:tmx:download:") + (game == HORIZON_GAME_TMUF ? "tmu" : "tmnf") + "?token=tok1001%3d%3d";
        check(faketmx::count("/trackplay/1001") == 1, "asks TMX for the play link of 1001");
        check(faketmx::state().links.size() == 1 && faketmx::state().links[0] == want, "link handed to the game: " + (faketmx::state().links.empty() ? std::string("none") : faketmx::state().links[0]));
        check(chost::playedMaps.empty() && faketmx::count("/trackgbx/") == 0, "no own download, no play_map_file");
        check(field(st, "links") == 1 && strField(st, "msg") == "Opened in the game", "status: Opened in the game");
    }

    // ---- refused while online: nothing is sent, "Play later" waits for the menu
    const size_t played = chost::playedMaps.size();
    const size_t links = faketmx::state().links.size();
    const size_t requests = faketmx::state().urls.size();
    chost::online = true;
    hook("play");
    st = hook("state");
    check(chost::playedMaps.size() == played && faketmx::state().urls.size() == requests && faketmx::state().links.size() == links, "online: play refused at once, no request");
    check(strField(st, "playblock") == "Leave the server first", "online: Leave the server first");
    hook("later");
    hook("pump");
    hook("pump");
    st = hook("state");
    check(field(st, "later") == 1 && chost::playedMaps.size() == played && faketmx::state().links.size() == links, "Play later: waits while online");
    hook("save");
    st = hook("state");
    if (!forever) check(fileExists(expect), "online: Save works");
    chost::online = false;
    for (int i = 0; i < 4; i++) hook("pump");
    st = hook("state");
    check(field(st, "later") == 0 && (forever ? faketmx::state().links.size() == links + 1 : chost::playedMaps.size() == played + 1), "Play later: starts back in the main menu");

    // ---- in a race
    chost::gameState = HORIZON_STATE_SOLO_RACE;
    const size_t played2 = chost::playedMaps.size();
    if (!forever) {
        // legacy: Play leaves the race (return_to_menu once the file is saved) and starts the map in the menu
        const int returns = chost::menuReturns;
        hook("play");
        hook("pump");
        hook("pump");
        st = hook("state");
        check(chost::menuReturns == returns + 1 && chost::playedMaps.size() == played2 && field(st, "later") == 1,
              "in a race: Play leaves the race once (" + std::to_string(chost::menuReturns - returns) + " return_to_menu)");
        check(strField(st, "msg") == "Leaving the race...", "status: Leaving the race...");
        chost::gameState = HORIZON_STATE_MAIN_MENU;
        for (int i = 0; i < 4; i++) hook("pump");
        check(chost::playedMaps.size() == played2 + 1 && chost::menuReturns == returns + 1 && field(hook("state"), "later") == 0,
              "in a race: the map starts back in the menu");
        chost::gameState = HORIZON_STATE_SOLO_RACE;
    } else {
        hook("play");
        hook("pump");
        check(chost::playedMaps.size() == played2 && strField(hook("state"), "playblock") == "Leave the race first", "in a race: Leave the race first");
    }
    const size_t played3 = chost::playedMaps.size();
    hook("later");
    hook("cancel");
    chost::gameState = HORIZON_STATE_MAIN_MENU;
    for (int i = 0; i < 3; i++) hook("pump");
    check(chost::playedMaps.size() == played3 && field(hook("state"), "later") == 0, "Cancel: nothing starts later");
    hook("save");
    st = hook("state");
    check(strField(st, "msg") == "Saved" && fileExists(expect), "Save writes the map");

    // ---- a download that is not a map is refused
    hook("back");
    hook("select 14"); // map 1013: TMX answers a page
    hook("save");
    st = hook("state");
    check(strField(st, "msg") == "Not a map file" && !fileExists(folder + "Speedway Echo (1013).Challenge.Gbx") && !fileExists(folder + "Speedway Echo (1013).Challenge.Gbx"),
          "not a map file: refused, nothing written (" + strField(st, "msg") + ")");
    hook("back");

    // ---- an error answer
    faketmx::state().forceStatus = 429;
    hook("search zz");
    check(strField(hook("state"), "err") == "Too many requests", "429: Too many requests");
    faketmx::state().forceStatus = 0;

    // ---- random opens the detail
    hook("random");
    st = hook("state");
    check(st.find("detail=browse:1007") != std::string::npos && faketmx::state().urls.back().find("count=100") != std::string::npos &&
              faketmx::state().urls.back().find("name=") == std::string::npos,
          "Random: 100 maps with the filters, opens one");
    hook("back");

    // ---- favourites
    hook("search a");
    hook("state");
    frame("browse", 2);
    hook("fav 1");
    hook("fav 2");
    st = hook("fav 3");
    check(field(st, "favs") == 3, "three favourites");
    check(fileExists(dataFolder + "favourites.json"), "favourites.json written");
    frame("favourites", 3);
    st = hook("favselect 1");
    check(st.find("detail=favourites:1002") != std::string::npos, "favourites list is newest first");
    frame("favourites", 2);
    hook("fav");
    check(field(hook("state"), "favs") == 2, "toggle off");
    hook("fav");
    check(field(hook("state"), "favs") == 3, "toggle on again");
    hook("back");

    // ---- Shutdown releases textures, favourites survive, pictures come from the disk cache
    const int created = field(hook("state"), "created");
    pShutdown();
    st = hook("state");
    check(field(st, "thumbs") == 0 && field(st, "released") == created && created > 0, "Shutdown released all " + std::to_string(created) + " textures");
    check(chost::panels.empty(), "panels unregistered");
    chost::playedMaps.clear();
    chost::toasts.clear();
    check(pInit(&g_api) == 0, "Init again");
    st = hook("state");
    check(field(st, "favs") == 3 && field(st, "results") == 0, "favourites loaded again, search state clean");
    const int before = faketmx::count("/trackshow/");
    frame("favourites", 5);
    st = hook("state");
    check(field(st, "thumbs") == 3 && field(st, "cache") == 3 && faketmx::count("/trackshow/") == before, "favourite cards: 3 pictures from the disk cache, no request");

    // ---- late answers after Shutdown are dropped
    hook("search a");
    pShutdown();
    check(pInit(&g_api) == 0, "Init once more");
    st = hook("state");
    check(field(st, "results") == 0 && chost::toasts.empty(), "answer of a request before Shutdown is dropped");

    // ---- switching off
    hook("search a");
    hook("state");
    frame("browse", 6);
    check(field(hook("state"), "thumbs") > 0, "textures loaded before switching off");
    pSetEnabled(0);
    st = hook("state");
    check(field(st, "thumbs") == 0 && field(st, "released") == field(st, "created"), "switching off releases the textures");
    frame("browse", 2);
    check(field(hook("state"), "thumbs") == 0, "off: panels draw nothing");
    pSetEnabled(1);

    // ---- settings
    hook("form name=hello author=bob tags=0,3 diff=1 len=3 awards=7");
    check(chost::settings["SearchName"] == "hello" && chost::settings["SearchTags"] == "9" && chost::settings["SearchDiff"] == "1" && chost::settings["SearchLen"] == "3" && chost::settings["SearchAwards"] == "7",
          "search form values saved in the settings");
    hook("search");
    check(faketmx::state().urls.back().find("&name=hello&author=bob&tag=0,3&difficulty=1&authortimemin=60000&authortimemax=120000") != std::string::npos,
          "form values reach the request: " + faketmx::state().urls.back());
    st = hook("state");
    check(field(st, "results") == 0 || st.find("err=\"\"") != std::string::npos, "awards filter applied by the plugin");
    pShutdown();
    check(pInit(&g_api) == 0, "Init (settings load)");
    hook("search");
    check(faketmx::state().urls.back().find("&name=hello&author=bob&tag=0,3") != std::string::npos, "form values restored from the settings");
    frame("browse", 3);
    hook("grid large");
    frame("browse", 2);
    check(chost::settings["GridLarge"] == "1", "grid size saved");
    pShutdown();
}

int main(int argc, char** argv) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    // the DLL: build\plugins\tmx-browser\tmx-browser.dll relative to the repo root (cwd)
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    const std::string dllPath = std::string(cwd) + "\\build\\plugins\\tmx-browser\\tmx-browser.dll";
    g_dll = LoadLibraryA(dllPath.c_str());
    if (!g_dll) {
        printf("FAIL cannot load %s (error %lu)\n", dllPath.c_str(), GetLastError());
        return 1;
    }
    pInfo = reinterpret_cast<InfoFn>(GetProcAddress(g_dll, "HorizonPlugin_Info"));
    pInit = reinterpret_cast<InitFn>(GetProcAddress(g_dll, "HorizonPlugin_Init"));
    pShutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(g_dll, "HorizonPlugin_Shutdown"));
    pSetEnabled = reinterpret_cast<SetEnabledFn>(GetProcAddress(g_dll, "HorizonPlugin_SetEnabled"));
    pHook = reinterpret_cast<HookFn>(GetProcAddress(g_dll, "HorizonClient_TestHook"));
    pTestNet = reinterpret_cast<faketmx::TestNetFn>(GetProcAddress(g_dll, "HorizonClient_TestNet"));
    if (!pInfo || !pInit || !pShutdown || !pSetEnabled || !pHook || !pTestNet) {
        printf("FAIL missing exports\n");
        return 1;
    }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 900);
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    io.Fonts->Build();

    const std::string root = std::string(cwd) + "\\build\\client\\test\\tmxb_run\\";
    struct G {
        uint32_t game;
        const char* code;
    } games[] = {{HORIZON_GAME_ESWC, "eswc"},
                 {HORIZON_GAME_SUNRISE, "sunrise"},
                 {HORIZON_GAME_ORIGINAL, "original"},
                 {HORIZON_GAME_TMNF, "tmnf"},
                 {HORIZON_GAME_TMUF, "tmuf"}};
    for (const G& g : games) {
        bool wanted = argc < 2;
        for (int i = 1; i < argc; i++)
            if (strcmp(argv[i], g.code) == 0) wanted = true;
        if (wanted) runGame(g.game, g.code, root);
    }
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    ImGui::DestroyContext();
    return g_fail;
}
