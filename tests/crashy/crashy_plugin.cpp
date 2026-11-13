// Test plugin for tests\host_test.cpp ("crash"): a module that throws while drawing. Built twice:
//   CRASH_menu     id "crashmenu":    DrawMenu opens a child window, pushes style and ID, then throws a C++ exception
//   CRASH_overlay  id "crashoverlay": DrawOverlay (from its 5th frame) opens a window, pushes, then writes to address 0
// The host must switch off only that module, close what it left open on the ImGui stacks, show a toast
// and keep the menu and the other modules working.
#define HORIZON_PLUGIN_BUILD
#include <stdexcept>

#include "imgui.h"
#include "horizon_imgui.h"
#include "horizon_plugin.h"

#if defined(CRASH_menu)
#define CRASH_ID "crashmenu"
#define CRASH_NAME "Crash in menu"
#elif defined(CRASH_overlay)
#define CRASH_ID "crashoverlay"
#define CRASH_NAME "Crash in overlay"
#else
#error "build with /DCRASH_menu or /DCRASH_overlay"
#endif

namespace {
const HorizonHostApi* g_host = nullptr;
bool g_imgui = false;
int g_frames = 0;
} // namespace

// a C++ exception from plugin code below an extern "C" export, as a real plugin would throw it
[[noreturn]] __declspec(noinline) void crashFail(const char* what) { throw std::runtime_error(what); }

extern "C" {

static const HorizonPluginInfo kInfo = {
    HORIZON_ABI,
    CRASH_ID,
    CRASH_NAME,
    "1.0.0",
    HORIZON_GAME_ALL,
    HORIZON_CAP_MENU | HORIZON_CAP_OVERLAY,
    HORIZON_FLAG_ONLINE_OK,
    "test",
    "Test only: throws while drawing.",
    HORIZON_ABI_MINOR,
};

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) { return &kInfo; }

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    if (!host || host->abi != HORIZON_ABI) return 1;
    g_host = host;
    g_imgui = horizon_imgui_attach(host);
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) { g_host = nullptr; }

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawMenu(void) {
    if (!g_imgui) return;
    ImGui::Text("before the error");
#if defined(CRASH_menu)
    ImGui::BeginChild("crash-child", ImVec2(200.0f, 100.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    ImGui::PushID("crash");
    crashFail("crash test: DrawMenu");
#endif
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawOverlay(void) {
    if (!g_imgui) return;
#if defined(CRASH_overlay)
    if (++g_frames < 5) return;
    ImGui::Begin("crash overlay window");
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
    ImGui::PushID("crash");
    *static_cast<volatile int*>(nullptr) = 1; // access violation, as a bad pointer in a real plugin
#endif
}

} // extern "C"
