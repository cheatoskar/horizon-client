// TMX Browser against the fake host (plugins\client-common\tests\client_host): loads the real DLL, drives it
// through its test hook, draws the panels in an ImGui window and checks results, thumbnails, play/save gates,
// files, favourites across Shutdown/Init and texture release. PASS/FAIL per check, exit code = failures.
// Usage: tmx_browser_fakehost.exe [game ...]   (eswc sunrise tmnf tmuf; default all four)
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "client_host.h"
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

static int countCalls(const char* prefix) {
    int n = 0;
    for (const chost::BridgeCall& c : chost::bridgeCalls)
        if (c.path.compare(0, strlen(prefix), prefix) == 0) n++;
    return n;
}

static void runGame(uint32_t game, const char* code, const std::string& root) {
    g_game = code;
    const bool forever = (game & HORIZON_GAME_FOREVER) != 0;
    const std::string dataRoot = root + code + "\\";
    system(("rd /s /q \"" + dataRoot + "\" 2>nul").c_str());
    tmx::createDirs(dataRoot);
    chost::setup(game, dataRoot, "tmx-browser");
    g_api = chost::api();
    const std::string dataFolder = dataRoot + "tmx-browser\\";

    const HorizonPluginInfo* info = pInfo();
    check(info && std::string(info->id) == "tmx-browser" && (info->games & game) && (info->flags & HORIZON_FLAG_ONLINE_OK) && info->abi == HORIZON_ABI, "Info: id, games, online flag");
    check(pInit(&g_api) == 0, "Init");
    check(chost::panels.size() == 2 && chost::panels[0].key == "browse" && chost::panels[1].key == "favourites" && (chost::panels[0].flags & HORIZON_PANEL_MENU_PAGE), "two menu-page panels registered");

    // ---- search
    std::string st = hook("search a");
    check(!chost::bridgeCalls.empty() && chost::bridgeCalls.back().path == std::string("/v1/tmx/search?game=") + (game == HORIZON_GAME_ESWC ? "tmn" : game == HORIZON_GAME_SUNRISE ? "tms" : game == HORIZON_GAME_TMNF ? "tmnf" : "tmuf") + "&name=a&page=1",
          "search request path: " + (chost::bridgeCalls.empty() ? std::string("none") : chost::bridgeCalls.back().path));
    check(field(st, "loading") == 1, "loading while the answer is out");
    frame("browse");
    st = hook("state");
    check(field(st, "results") == 12 && field(st, "more") == 1 && field(st, "loading") == 0, "12 results parsed, more = 1");

    // ---- thumbnails: lazy, at most 3 in flight, all 12 arrive
    size_t before = chost::bridgeCalls.size();
    int maxPerFrame = 0;
    for (int i = 0; i < 12; i++) {
        before = chost::bridgeCalls.size();
        frame("browse");
        int thumbs = 0;
        for (size_t k = before; k < chost::bridgeCalls.size(); k++)
            if (chost::bridgeCalls[k].path.compare(0, 14, "/v1/tmx/thumb?") == 0) thumbs++;
        if (thumbs > maxPerFrame) maxPerFrame = thumbs;
    }
    st = hook("state");
    check(field(st, "thumbs") == 12 && field(st, "created") == 12, "12 thumbnails decoded and textures created");
    check(maxPerFrame >= 1 && maxPerFrame <= 3, "thumbnail requests queued, at most 3 per round (max " + std::to_string(maxPerFrame) + ")");
    check(countCalls("/v1/tmx/thumb?") == 12, "one thumbnail request per card");
    const int thumbCallsBefore = countCalls("/v1/tmx/thumb?");
    frame("browse", 3);
    check(countCalls("/v1/tmx/thumb?") == thumbCallsBefore, "no repeated thumbnail requests once loaded");

    // ---- paging
    hook("more");
    frame("browse");
    st = hook("state");
    check(field(st, "results") == 24 && field(st, "more") == 0 && field(st, "page") == 2, "More appends page 2 (24 results)");
    frame("browse", 2);

    // ---- detail
    st = hook("select 2");
    check(strField(st, "playblock").empty() && st.find("detail=browse:1001") != std::string::npos, "select 2 opens the detail of map 1001");
    frame("browse", 2);
    chost::playedMaps.clear();

    // ---- play
    st = hook("play");
    frame("browse");
    const std::string folder = forever ? dataRoot + "user\\Tracks\\Challenges\\Downloaded\\_Horizon\\" : dataRoot + "game\\GameData\\Tracks\\Challenges\\Downloaded\\_Horizon\\";
    const std::string expect = folder + "Speedway Echo (1001).Challenge.Gbx";
    check(chost::playedMaps.size() == 1 && chost::playedMaps[0] == expect, "play_map_file called with " + (chost::playedMaps.empty() ? std::string("nothing") : chost::playedMaps[0]));
    std::string bytes;
    tmx::readFile(expect, bytes);
    check(bytes.compare(0, 3, "GBX") == 0 && bytes.size() >= 8 && bytes.compare(bytes.size() - 4, 4, "1001") == 0, "map file written with the downloaded bytes");
    check(!chost::toasts.empty() && chost::toasts.back().kind == HORIZON_TOAST_OK, "success toast");
    st = hook("state");
    check(strField(st, "msg") == "Map started", "status line says Map started");

    // ---- play refused
    const size_t played = chost::playedMaps.size();
    const int mapCalls = countCalls("/v1/tmx/map?");
    chost::online = true;
    st = hook("play");
    frame("browse", 2);
    check(chost::playedMaps.size() == played && countCalls("/v1/tmx/map?") == mapCalls, "online: play refused, no download");
    check(strField(hook("state"), "playblock") == "Playing is off while online", "online: reason shown");
    // Save is allowed online
    st = hook("save");
    frame("browse");
    check(countCalls("/v1/tmx/map?") == mapCalls + 1 && chost::playedMaps.size() == played, "online: Save still downloads, does not play");
    check(fileExists(expect), "saved file exists");
    chost::online = false;

    chost::gameState = HORIZON_STATE_SOLO_RACE;
    const int mapCalls2 = countCalls("/v1/tmx/map?");
    st = hook("play");
    frame("browse");
    check(chost::playedMaps.size() == played && countCalls("/v1/tmx/map?") == mapCalls2, "in a race: play refused");
    check(strField(hook("state"), "playblock") == "Go back to the main menu to play", "in a race: main menu hint");
    hook("save");
    frame("browse");
    check(countCalls("/v1/tmx/map?") == mapCalls2 + 1, "in a race: Save works");
    chost::gameState = HORIZON_STATE_MAIN_MENU;

    chost::bridgeStateValue = HORIZON_BRIDGE_SEARCHING;
    const int mapCalls3 = countCalls("/v1/tmx/map?");
    hook("play");
    frame("browse", 2);
    check(chost::playedMaps.size() == played && countCalls("/v1/tmx/map?") == mapCalls3, "bridge not connected: play and save refused");
    check(strField(hook("state"), "playblock") == "Start TM Horizon to download maps", "bridge hint");
    hook("search x");
    check(!strField(hook("state"), "err").empty(), "search without the bridge shows a line");
    chost::bridgeStateValue = HORIZON_BRIDGE_CONNECTED;

    // an error answer
    chost::bridgeHandler = [](const std::string&, const std::string&, std::string* type, std::string* body) {
        *type = "application/json";
        *body = "{\"ok\":false}";
        return 429;
    };
    hook("search zz");
    frame("browse");
    check(strField(hook("state"), "err") == "Too many requests, wait a moment", "429 shows the rate limit line");
    chost::bridgeHandler = nullptr;

    // ---- random opens the detail
    hook("back");
    hook("random");
    frame("browse");
    st = hook("state");
    check(st.find("detail=browse:1007") != std::string::npos, "Random opens the detail of the answer");
    hook("back");

    // ---- favourites
    st = hook("search a");
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
    hook("fav"); // un-favourite the open one
    check(field(hook("state"), "favs") == 2, "toggle off");
    hook("fav");
    check(field(hook("state"), "favs") == 3, "toggle on again");
    hook("back");

    // ---- Shutdown releases textures, favourites survive
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
    frame("favourites", 5);
    check(field(hook("state"), "thumbs") == 3, "favourite cards load their thumbnails");

    // ---- late answers after Shutdown are ignored
    hook("search a");
    pShutdown();
    chost::endFrame(); // delivers the search answer to a dead plugin
    st = hook("state");
    check(field(st, "results") == 0 && chost::toasts.empty(), "late bridge answer after Shutdown is ignored");
    check(pInit(&g_api) == 0, "Init once more");

    // ---- switching off
    hook("search a");
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
    check(chost::bridgeCalls.back().path.find("name=hello&author=bob&tags=0,3&difficulty=1&lengthmin=60000&lengthmax=120000&awards=7&page=1") != std::string::npos, "form values reach the request: " + chost::bridgeCalls.back().path);
    frame("browse", 3);
    pShutdown();
    check(pInit(&g_api) == 0, "Init (settings load)");
    hook("search");
    check(chost::bridgeCalls.back().path.find("name=hello&author=bob&tags=0,3") != std::string::npos, "form values restored from the settings");
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
    if (!pInfo || !pInit || !pShutdown || !pSetEnabled || !pHook) {
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
    } games[] = {{HORIZON_GAME_ESWC, "eswc"}, {HORIZON_GAME_SUNRISE, "sunrise"}, {HORIZON_GAME_TMNF, "tmnf"}, {HORIZON_GAME_TMUF, "tmuf"}};
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
