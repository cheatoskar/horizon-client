// input-display.dll in the client test host for ESWC, Sunrise, TMNF and TMUF: the car's steer/gas/brake read
// from the fake world (by the default offsets, like when reflection has no member), speed/gear/RPM where the
// game has them, garbage refused, no race, every style drawn. The plugin never writes: the test checks the
// fake car's memory is unchanged afterwards.
//   input_display_fakehost.exe <input-display.dll> [games]
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
HorizonPlugin_InitFn g_init;
HorizonPlugin_ShutdownFn g_shutdown;
HorizonPlugin_DrawOverlayFn g_overlay;
HorizonPlugin_DrawMenuFn g_menu;
HookFn g_hook;

std::string state() {
    char out[256] = {};
    g_hook("state", out, sizeof(out));
    return out;
}
void frame() {
    ImGui::NewFrame();
    g_overlay();
    ImGui::Begin("w");
    g_menu();
    for (const chost::Panel& p : chost::panels) p.draw(p.user);
    ImGui::End();
    ImGui::EndFrame();
    chost::endFrame();
}

void runGame(uint32_t game, const char* name) {
    printf("===== input-display, game %s\n", name);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string root = exe;
    root.resize(root.find_last_of('\\') + 1);
    root += std::string("inp-") + name + "\\";
    chost::setup(game, root, "input-display");
    const bool forever = (game & HORIZON_GAME_FOREVER) != 0;
    chost::buildWorld(forever, "UidOfTheTestMap000000000aB", L"Test");
    const HorizonHostApi api = chost::api();
    check(g_init(&api) == 0, "Init returns 0");
    check(chost::panels.size() == 1 && (chost::panels[0].flags & HORIZON_PANEL_HUD), "one HUD panel");
    chost::setRun(1, 3000, {});
    chost::setCar(-0.75f, 1.0f, 0.0f, 187, 4, 7300.0f);
    std::vector<unsigned char> before(0x800);
    memcpy(before.data(), reinterpret_cast<const void*>(chost::world.vehicle), before.size());
    frame();
    std::string s = state();
    check(s.find("car=1") == 0 && s.find("steer=-0.75") != std::string::npos && s.find("gas=1.00") != std::string::npos, "inputs read: " + s);
    if (forever) check(s.find("speed=187") != std::string::npos && s.find("gear=4") != std::string::npos && s.find("rpm=7300") != std::string::npos,
                       "speed, gear, RPM on Forever");
    else check(s.find("speed=-1") != std::string::npos && s.find("gear=-1") != std::string::npos, "no speed/gear fields on ESWC/Sunrise (shown as not available)");
    for (int style = 0; style < 4; style++) {
        char cmd[16];
        snprintf(cmd, sizeof(cmd), "style %d", style);
        g_hook(cmd, nullptr, 0);
        frame();
    }
    check(memcmp(before.data(), reinterpret_cast<const void*>(chost::world.vehicle), before.size()) == 0, "the car's memory is untouched");
    chost::setCar(0.1f, 5.0f, 0.0f, 100, 2, 3000.0f); // gas 5.0: an offset that is wrong in this exe
    frame();
    check(state().find("car=0") == 0, "implausible values refused");
    chost::clearRace();
    frame();
    check(state().find("race=0") != std::string::npos, "no race: nothing read");
    chost::online = true; // online-ok: still shown
    chost::buildWorld(forever, "UidOfTheTestMap000000000aB", L"Test");
    chost::setCar(0.3f, 0.0f, 1.0f, 50, 1, 2000.0f);
    frame();
    check(state().find("car=1") == 0 && state().find("brake=1.00") != std::string::npos, "shown online too");
    g_shutdown();
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600, 900);
    io.IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    HMODULE mod = LoadLibraryA(argv[1]);
    if (!mod) return 3;
    g_init = reinterpret_cast<HorizonPlugin_InitFn>(GetProcAddress(mod, HORIZON_EXPORT_INIT));
    g_shutdown = reinterpret_cast<HorizonPlugin_ShutdownFn>(GetProcAddress(mod, HORIZON_EXPORT_SHUTDOWN));
    g_overlay = reinterpret_cast<HorizonPlugin_DrawOverlayFn>(GetProcAddress(mod, HORIZON_EXPORT_DRAW_OVERLAY));
    g_menu = reinterpret_cast<HorizonPlugin_DrawMenuFn>(GetProcAddress(mod, HORIZON_EXPORT_DRAW_MENU));
    g_hook = reinterpret_cast<HookFn>(GetProcAddress(mod, "HorizonClient_TestHook"));
    if (!g_init || !g_shutdown || !g_overlay || !g_menu || !g_hook) return 4;
    std::vector<std::string> games;
    for (int i = 2; i < argc; i++) games.push_back(argv[i]);
    if (games.empty()) games = {"eswc", "sunrise", "tmnf", "tmuf"};
    for (const std::string& g : games)
        runGame(g == "eswc" ? HORIZON_GAME_ESWC : g == "sunrise" ? HORIZON_GAME_SUNRISE : g == "tmuf" ? HORIZON_GAME_TMUF : HORIZON_GAME_TMNF, g.c_str());
    printf("input_display_fakehost: %s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
    ImGui::DestroyContext();
    return g_fail;
}
