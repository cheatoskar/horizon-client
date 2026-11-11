// Headless test of the menu's input (no game, no Direct3D): with the menu open and its dim layer active,
// the pointer must hit the menu panel (not the dim layer), a click must land on a menu control, and the
// same must hold after the display size changed (Reset 640x480 -> 2560x1440).
#include <windows.h>

#ifdef small
#undef small
#endif
#include "host/ui/ui.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace horizon::ui;

namespace {

int g_failed = 0;
void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

Model g_model;
Actions g_actions;
bool g_open = true;
int g_barToggles = 0;

void frames(int n, ImVec2 mouse, int clickAt = -1) {
    ImGuiIO& io = ImGui::GetIO();
    for (int f = 0; f < n; ++f) {
        io.DeltaTime = 1.0f / 30.0f;
        io.AddMousePosEvent(mouse.x, mouse.y);
        if (clickAt >= 0 && f == clickAt) io.AddMouseButtonEvent(0, true);
        if (clickAt >= 0 && f == clickAt + 2) io.AddMouseButtonEvent(0, false);
        ImGui::NewFrame();
        if (g_open) drawMenu(g_model, g_actions, &g_open);
        ImGui::EndFrame();
    }
}

std::string hoveredRoot() {
    ImGuiContext& c = *ImGui::GetCurrentContext();
    return c.HoveredWindow && c.HoveredWindow->RootWindow ? c.HoveredWindow->RootWindow->Name : "";
}

} // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    setIconFontFile("assets/lucide-subset.ttf");
    loadFonts(1.0f);
    applyStyle(1.0f);
    unsigned char* px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    io.DisplaySize = ImVec2(640, 480); // the first device is made at 640x480 and Reset later
    g_model.hostVersion = "0.1.0";
    g_model.gameName = "TrackMania Sunrise";
    g_model.menuHotkey = 1;
    g_model.barEnabled = true;
    g_model.menuKeySuggestions = {"Insert", "F2"};
    g_actions.setBarEnabled = [](bool) { ++g_barToggles; };
    g_actions.closeMenu = [] {};
    showPage("general");

    std::printf("menu input with the dim layer\n");
    frames(40, ImVec2(320, 240));
    check(g_open, "menu stays open");
    check(hoveredRoot() == "##horizon_panel", "pointer over the menu hits the panel, not the dim layer (640x480)");

    io.DisplaySize = ImVec2(2560, 1440); // Reset to the real size
    frames(40, ImVec2(1280, 720));
    check(hoveredRoot() == "##horizon_panel", "after the display change (2560x1440) the pointer still hits the panel");

    io.DisplaySize = ImVec2(1600, 900);
    frames(10, ImVec2(5, 5)); // outside the panel: dim only, the panel is not hovered
    check(hoveredRoot() != "##horizon_panel", "pointer outside the panel does not hit the panel");
    // click on the "Top bar" switch (right card, first row at 1600x900)
    g_barToggles = 0;
    frames(20, ImVec2(1314, 209), 10);
    check(g_open, "menu still open after the click");
    check(g_barToggles == 1, "a click on a menu switch lands (setBarEnabled called once)");

    ImGui::DestroyContext();
    std::printf(g_failed ? "FAILED\n" : "all ok\n");
    return g_failed;
}
