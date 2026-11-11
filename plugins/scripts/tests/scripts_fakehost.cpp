// scripts.dll in a stub host, once per game (ESWC, Sunrise, TMNF, TMUF): the two example scripts are
// copied into <game>\horizon\scripts\, the plugin loads them, the host fires a race's events and draws
// frames (overlay + the Scripts and Options panels). Both scripts must run unchanged everywhere, draw
// their windows and log nothing bad.
//
//   scripts_fakehost.exe <scripts.dll> <examples folder>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "fake_graph.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "stub_host.h"
#include "test_host.h"

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) g_fail++;
}
bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

void copyTree(const std::string& from, const std::string& to) {
    CreateDirectoryA(to.c_str(), nullptr);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((from + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        const std::string a = from + "\\" + fd.cFileName, b = to + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copyTree(a, b);
        else CopyFileA(a.c_str(), b.c_str(), FALSE);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void removeTree(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            const std::string p = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(p);
            else DeleteFileA(p.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}

bool windowDrawn(const char* name) {
    ImGuiWindow* w = ImGui::FindWindowByName(name);
    return w && w->Active; // Active: drawn in the frame that just ended
}

struct Exports {
    HorizonPlugin_InfoFn info;
    HorizonPlugin_InitFn init;
    HorizonPlugin_ShutdownFn shutdown;
    HorizonPlugin_DrawOverlayFn overlay;
    HorizonPlugin_OnEventFn onEvent;
};

void frame(const Exports& x, bool panels) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600, 900);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    x.overlay();
    if (panels) {
        ImGui::SetNextWindowSize(ImVec2(1000, 700));
        ImGui::Begin("Horizon menu (test)");
        for (const testhost::Panel& p : testhost::panels) {
            ImGui::PushID(p.key.c_str());
            p.draw(p.user);
            ImGui::PopID();
        }
        ImGui::End();
    }
    ImGui::EndFrame();
    ImGui::Render();
    testhost::runQueued();
}

void runGame(const char* dll, const std::string& examples, const std::string& root, uint32_t game, const char* label) {
    printf("===== %s\n", label);
    const std::string folder = root + label + "\\";
    removeTree(root + label);
    CreateDirectoryA(root.c_str(), nullptr);
    CreateDirectoryA((root + label).c_str(), nullptr);
    CreateDirectoryA((folder + "horizon").c_str(), nullptr);
    copyTree(examples, folder + "horizon\\scripts");
    testhost::setup(game, folder);
    stubhost::settings.clear();
    testhost::clearLogs();
    stubhost::graph = fake::build((game & HORIZON_GAME_FOREVER) != 0);

    HMODULE mod = LoadLibraryA(dll);
    check(mod != nullptr, "LoadLibrary");
    if (!mod) return;
    Exports x;
    x.info = reinterpret_cast<HorizonPlugin_InfoFn>(GetProcAddress(mod, HORIZON_EXPORT_INFO));
    x.init = reinterpret_cast<HorizonPlugin_InitFn>(GetProcAddress(mod, HORIZON_EXPORT_INIT));
    x.shutdown = reinterpret_cast<HorizonPlugin_ShutdownFn>(GetProcAddress(mod, HORIZON_EXPORT_SHUTDOWN));
    x.overlay = reinterpret_cast<HorizonPlugin_DrawOverlayFn>(GetProcAddress(mod, HORIZON_EXPORT_DRAW_OVERLAY));
    x.onEvent = reinterpret_cast<HorizonPlugin_OnEventFn>(GetProcAddress(mod, HORIZON_EXPORT_ON_EVENT));
    check(x.info && x.init && x.shutdown && x.overlay && x.onEvent, "exports (Info, Init, Shutdown, DrawOverlay, OnEvent)");
    check(GetProcAddress(mod, HORIZON_EXPORT_DRAW_MENU) == nullptr, "no DrawMenu: the page is the Scripts and Options panels");
    if (!x.info || !x.init || !x.shutdown || !x.overlay || !x.onEvent) return;
    const HorizonPluginInfo* info = x.info();
    check(info && !strcmp(info->id, "scripts") && (info->games & game) && (info->flags & HORIZON_FLAG_ONLINE_OK), "info: id scripts, this game, online-ok");

    const HorizonHostApi host = testhost::api();
    check(x.init(&host) == 0, "Init returns 0");
    check(testhost::panels.size() == 2 && testhost::panels[0].title == "Scripts" && testhost::panels[1].title == "Options", "panels: Scripts, Options");
    const std::string title = testhost::gameName(game);
    check(contains(testhost::logs(), "[clock] Clock ready on " + title), "clock: Main() ran");

    // a race: map, start, checkpoint, finish, end, menu - with frames in between
    frame(x, true);
    HorizonMapInfo mi = {};
    mi.size = sizeof(mi);
    mi.name = "A01-Race";
    mi.author = game == HORIZON_GAME_SUNRISE ? "" : "Nadeo"; // games leave it empty for maps from their own menus
    mi.uid = "";
    mi.path = "";
    mi.environment = game & HORIZON_GAME_FOREVER ? "Stadium" : "Speed";
    mi.author_time = 23140;
    mi.gold_time = 25000;
    mi.silver_time = 30000;
    mi.bronze_time = 38000;
    mi.author_score = -1;
    HorizonEvent e = {};
    e.size = sizeof(e);
    e.type = HORIZON_EVENT_MAP_LOADED;
    e.map = &mi;
    x.onEvent(&e);
    frame(x, true);
    check(contains(testhost::logs(), std::string("[map-info] map: A01-Race") + (game == HORIZON_GAME_SUNRISE ? "\n" : " by Nadeo")), "map-info: OnMapLoaded");
    e.map = nullptr;
    const uint32_t seq[] = {HORIZON_EVENT_GAME_STATE, HORIZON_EVENT_RACE_START, HORIZON_EVENT_CHECKPOINT, HORIZON_EVENT_RESPAWN, HORIZON_EVENT_FINISH,
                            HORIZON_EVENT_RACE_END};
    for (uint32_t t : seq) {
        e.type = t;
        e.time_ms = t == HORIZON_EVENT_FINISH ? 24010 : 5120;
        e.game_state = HORIZON_STATE_SOLO_RACE;
        x.onEvent(&e);
        frame(x, true);
    }
    check(windowDrawn("Clock##hs.clock"), "clock window drawn");
    check(windowDrawn("Map Info##hs.map-info"), "map-info window drawn");
    // online: both are online-ok and keep running
    testhost::online = true;
    e.type = HORIZON_EVENT_ONLINE_CHANGED;
    e.is_online = 1;
    x.onEvent(&e);
    frame(x, false);
    check(windowDrawn("Clock##hs.clock") && windowDrawn("Map Info##hs.map-info"), "online: both still drawn (online-ok)");
    testhost::online = false;
    x.onEvent(&e);
    e.type = HORIZON_EVENT_MENU_ENTERED;
    x.onEvent(&e);
    frame(x, true);
    frame(x, true);
    check(!windowDrawn("Map Info##hs.map-info"), "menu: map-info hides");
    check(windowDrawn("Clock##hs.clock"), "menu: clock stays");
    // photo/video capture: overlays hidden (setting on by default)
    testhost::hostFlags = HORIZON_HOST_FLAG_CAPTURE;
    frame(x, false);
    frame(x, false);
    check(!windowDrawn("Clock##hs.clock"), "capture: clock hidden");
    testhost::hostFlags = 0;

    const std::string& L = testhost::logs();
    check(!contains(L, "[clock] main.as") && !contains(L, "[map-info] main.as") && !contains(L, "error") && !contains(L, "never called") &&
              !contains(L, "is ignored"),
          "no errors or warnings logged");
    x.shutdown();
    check(FreeLibrary(mod) != 0, "Shutdown + FreeLibrary");
    if (g_fail) printf("%s\n", L.c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: scripts_fakehost <scripts.dll> <examples folder>\n");
        return 2;
    }
    char full[MAX_PATH];
    GetFullPathNameA(argv[1], MAX_PATH, full, nullptr);
    const std::string dll = full;
    GetFullPathNameA(argv[2], MAX_PATH, full, nullptr);
    const std::string examples = full;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string root = exe;
    root.resize(root.find_last_of('\\') + 1);
    root += "fake";

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char* px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    const struct {
        uint32_t game;
        const char* label;
    } games[] = {{HORIZON_GAME_ESWC, "eswc"}, {HORIZON_GAME_SUNRISE, "sunrise"}, {HORIZON_GAME_TMNF, "tmnf"}, {HORIZON_GAME_TMUF, "tmuf"}};
    for (const auto& g : games) runGame(dll.c_str(), examples, root + "\\", g.game, g.label);
    ImGui::DestroyContext();
    printf(g_fail ? "fake host: %d FAILED\n" : "fake host: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
