// wr-hunter.dll in the client test host, for ESWC, Sunrise, TMNF and TMUF: map load (UID found through the
// id table, nothing from the host), a reset, two finished runs, checkpoint times from the fake game's
// list (Forever) or from CHECKPOINT events (ESWC/Sunrise), the TMX record through the fake bridge, both
// layouts drawn, settings and history surviving Shutdown/Init.
//   wr_hunter_fakehost.exe <wr-hunter.dll> [eswc|sunrise|tmnf|tmuf ...]
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "client_host.h"
#include "imgui.h"

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) g_fail++;
}

typedef int32_t(__cdecl* HookFn)(const char*, char*, int32_t);
struct Plugin {
    HorizonPlugin_InitFn init;
    HorizonPlugin_ShutdownFn shutdown;
    HorizonPlugin_OnEventFn onEvent;
    HorizonPlugin_DrawOverlayFn overlay;
    HorizonPlugin_DrawMenuFn menu;
    HookFn hook;
};

std::string state(const Plugin& p) {
    char out[512] = {};
    p.hook("state", out, sizeof(out));
    return out;
}
int field(const std::string& s, const char* key) {
    const std::string k = std::string(key) + "=";
    const size_t at = s.find(" " + k) != std::string::npos ? s.find(" " + k) + 1 : (s.compare(0, k.size(), k) == 0 ? 0 : std::string::npos);
    return at == std::string::npos ? -999 : atoi(s.c_str() + at + k.size());
}

void frame(const Plugin& p, bool drawPanels) {
    ImGui::NewFrame();
    p.overlay();
    if (drawPanels) {
        ImGui::Begin("menu");
        p.menu();
        for (const chost::Panel& pn : chost::panels) {
            ImGui::PushID(pn.key.c_str());
            pn.draw(pn.user);
            ImGui::PopID();
        }
        ImGui::End();
    }
    ImGui::EndFrame();
    chost::endFrame();
}

void send(const Plugin& p, HorizonEvent e) { p.onEvent(&e); }

const char* kUid = "UidOfTheTestMap000000000aB";

void runGame(HMODULE mod, const Plugin& p, uint32_t game, const char* name) {
    printf("===== wr-hunter, game %s\n", name);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string root = exe;
    root.resize(root.find_last_of('\\') + 1);
    root += std::string("wrh-") + name + "\\";
    // fresh data folder
    const std::string maps = root + "wr-hunter\\maps\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((maps + "*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do DeleteFileA((maps + fd.cFileName).c_str());
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    DeleteFileA((root + "wr-hunter\\records\\" + kUid + ".Replay.Gbx").c_str());
    chost::setup(game, root, "wr-hunter");
    const bool forever = (game & HORIZON_GAME_FOREVER) != 0;
    chost::buildWorld(forever, kUid, L"$o$fffTest Map");
    const HorizonHostApi api = chost::api();
    check(p.init(&api) == 0, "Init returns 0");
    check(chost::panels.size() == 2, "two panels registered (splits HUD, history)");
    bool hud = false;
    for (auto& pn : chost::panels) hud = hud || (pn.key == "splits" && (pn.flags & HORIZON_PANEL_HUD));
    check(hud, "splits is a HUD panel");

    HorizonMapInfo mi = chost::mapInfo("Test Map", "", 24000); // the host does not know the UID
    HorizonEvent e = chost::event(HORIZON_EVENT_MAP_LOADED);
    e.map = &mi;
    send(p, e);
    frame(p, false);
    frame(p, false); // record lookup + replay download through the fake bridge
    std::string s = state(p);
    check(s.find(std::string("map=") + kUid) != std::string::npos && field(s, "uid") == 1, "map kept by the UID read from the id table: " + s);
    check(field(s, "targetSplits") == 5 && field(s, "target") == 24420, "TMX record as target (5 splits)");

    auto drive = [&](const std::vector<int>& cps, bool finish) {
        chost::gameState = HORIZON_STATE_SOLO_RACE;
        chost::setRun(0, 0, {});
        send(p, chost::event(HORIZON_EVENT_RACE_START));
        chost::setRun(1, 10, {});
        frame(p, false);
        std::vector<int> passed;
        for (size_t i = 0; i < cps.size(); i++) {
            const bool last = i + 1 == cps.size();
            if (last && !finish) break;
            passed.push_back(cps[i]);
            if (last) {
                chost::setRun(2, cps[i], passed);
                frame(p, true);
                HorizonEvent f = chost::event(HORIZON_EVENT_FINISH);
                f.time_ms = cps[i];
                f.is_local_player = 1;
                send(p, f);
            } else {
                chost::setRun(1, cps[i] + 40, passed);
                if (!forever) {
                    HorizonEvent c = chost::event(HORIZON_EVENT_CHECKPOINT);
                    c.time_ms = cps[i];
                    c.checkpoint_index = static_cast<int32_t>(i);
                    send(p, c);
                }
                frame(p, true);
            }
        }
    };
    drive({5300, 10100, 15400, 20500, 25200}, false); // reset before the finish
    s = state(p);
    check(field(s, "live") == 4 && field(s, "run") == 1, "four splits while running: " + s);
    drive({5300, 10100, 15400, 20500, 25200}, true);
    s = state(p);
    check(field(s, "resets") == 1 && field(s, "attempts") == 2, "the restart counted as a reset");
    check(field(s, "finishes") == 1 && field(s, "pb") == 25200 && field(s, "live") == 5, "first finish is the PB, five splits");
    drive({5000, 10300, 15100, 20000, 24900}, true);
    s = state(p);
    check(field(s, "pb") == 24900 && field(s, "finishes") == 2, "faster run is the new PB");
    check(field(s, "sob") == 5000 + 4800 + 4800 + 4900 + 4700, "sum of best from gold sectors: " + s);
    check(chost::toasts.size() == 1 && chost::toasts[0].title == "New personal best", "PB toast");
    p.hook("layout 1", nullptr, 0);
    frame(p, true);
    p.hook("layout 0", nullptr, 0);
    frame(p, true);

    // online: splits stay (online-ok)
    chost::online = true;
    drive({5100, 10000, 15000, 20000, 25000}, true);
    s = state(p);
    check(field(s, "finishes") == 3, "splits work online");
    chost::online = false;

    // ghost target on Forever
    if (forever) {
        chost::setGhost(23900, {4800, 9600, 14500, 19300, 23900});
        p.hook("target 2", nullptr, 0);
        for (int i = 0; i < 70; i++) frame(p, false);
        s = state(p);
        check(field(s, "target") == 23900, "ghost in the race as target: " + s);
    }
    p.shutdown();
    check(GetFileAttributesA((maps + kUid + ".json").c_str()) != INVALID_FILE_ATTRIBUTES, "map file written");
    check(GetFileAttributesA((root + "wr-hunter\\records\\" + kUid + ".Replay.Gbx").c_str()) != INVALID_FILE_ATTRIBUTES, "record replay cached");

    // again: everything comes back from the file, the record from the cache (no bridge call)
    chost::bridgeCalls.clear();
    chost::settings["Target"] = "1";
    check(p.init(&api) == 0, "Init again");
    send(p, e);
    frame(p, true);
    s = state(p);
    check(field(s, "pb") == 24900 && field(s, "finishes") == 3 && field(s, "resets") == 1, "stats reloaded: " + s);
    check(chost::bridgeCalls.empty() && field(s, "targetSplits") == 5, "record from the cache");
    // no bridge (TM Horizon closed) and a map without the cached record
    p.shutdown();
    (void)mod;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: wr_hunter_fakehost <wr-hunter.dll> [games]\n");
        return 2;
    }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600, 900);
    io.IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    HMODULE mod = LoadLibraryA(argv[1]);
    if (!mod) {
        printf("FAIL  LoadLibrary %s (%lu)\n", argv[1], GetLastError());
        return 3;
    }
    Plugin p;
    p.init = reinterpret_cast<HorizonPlugin_InitFn>(GetProcAddress(mod, HORIZON_EXPORT_INIT));
    p.shutdown = reinterpret_cast<HorizonPlugin_ShutdownFn>(GetProcAddress(mod, HORIZON_EXPORT_SHUTDOWN));
    p.onEvent = reinterpret_cast<HorizonPlugin_OnEventFn>(GetProcAddress(mod, HORIZON_EXPORT_ON_EVENT));
    p.overlay = reinterpret_cast<HorizonPlugin_DrawOverlayFn>(GetProcAddress(mod, HORIZON_EXPORT_DRAW_OVERLAY));
    p.menu = reinterpret_cast<HorizonPlugin_DrawMenuFn>(GetProcAddress(mod, HORIZON_EXPORT_DRAW_MENU));
    p.hook = reinterpret_cast<HookFn>(GetProcAddress(mod, "HorizonClient_TestHook"));
    if (!p.init || !p.shutdown || !p.onEvent || !p.overlay || !p.menu || !p.hook) {
        printf("FAIL  exports missing\n");
        return 4;
    }
    std::vector<std::string> games;
    for (int i = 2; i < argc; i++) games.push_back(argv[i]);
    if (games.empty()) games = {"eswc", "sunrise", "tmnf", "tmuf"};
    for (const std::string& g : games) {
        const uint32_t bit = g == "eswc" ? HORIZON_GAME_ESWC : g == "sunrise" ? HORIZON_GAME_SUNRISE : g == "tmuf" ? HORIZON_GAME_TMUF : HORIZON_GAME_TMNF;
        runGame(mod, p, bit, g.c_str());
    }
    printf("wr_hunter_fakehost: %s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
    ImGui::DestroyContext();
    return g_fail;
}
