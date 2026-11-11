// Sample plugin for Horizon Host: the smallest useful example for plugin authors.
//
// It shows every part of the plugin ABI (include/horizon_plugin.h):
//   - HorizonPlugin_Info      what the plugin is (id, games, capabilities, flags)
//   - HorizonPlugin_Init      called once with the host API table; return 0 to load
//   - HorizonPlugin_OnEvent   game events (here: kept as text, newest last, 20 at most)
//   - HorizonPlugin_DrawMenu  the plugin's page in the Horizon menu (Dear ImGui)
//   - HorizonPlugin_DrawOverlay a small always-on HUD (can be switched off by a setting)
//   - HorizonPlugin_SetEnabled  live on/off from the menu
//   - HorizonPlugin_Shutdown  called before the DLL is unloaded
// Everything runs on the game's main thread. Keep handlers short.
//
// Build: see plugins\sample\build_plugin.cmd. Needs the same Dear ImGui version as the host.

// Defines HORIZON_EXPORT as dllexport, so the exports below are visible without a .def file.
#define HORIZON_PLUGIN_BUILD
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "horizon_imgui.h"   // horizon_imgui_attach(), after imgui.h
#include "horizon_plugin.h"

namespace {

const HorizonHostApi* g_host = nullptr; // valid from Init until Shutdown
bool g_imgui = false;                   // true when we may draw
bool g_enabled = true;                  // set by the host through SetEnabled
bool g_showOverlay = true;              // our setting "ShowOverlay"
int32_t g_pingHotkey = 0;               // handle of our hotkey

constexpr int kMaxLines = 20;
char g_lines[kMaxLines][96];            // ring buffer of event texts
int g_lineCount = 0;                    // how many are stored (<= kMaxLines)
int g_lineNext = 0;                     // where the next one goes

void addLine(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_lines[g_lineNext], sizeof(g_lines[0]), fmt, ap);
    va_end(ap);
    g_lineNext = (g_lineNext + 1) % kMaxLines;
    if (g_lineCount < kMaxLines) g_lineCount++;
}

// Hotkey callback: runs on the game thread once per key press, only while we are enabled and allowed.
void HORIZON_CALL onPing(void*) {
    addLine("ping (hotkey)");
    g_host->log(g_host->plugin, HORIZON_LOG_INFO, "ping");
}

const char* eventName(uint32_t type) {
    switch (type) {
    case HORIZON_EVENT_MAP_LOADED: return "map loaded";
    case HORIZON_EVENT_RACE_START: return "race start";
    case HORIZON_EVENT_RESPAWN: return "respawn";
    case HORIZON_EVENT_CHECKPOINT: return "checkpoint";
    case HORIZON_EVENT_FINISH: return "finish";
    case HORIZON_EVENT_RACE_END: return "race end";
    case HORIZON_EVENT_MENU_ENTERED: return "menu entered";
    case HORIZON_EVENT_REPLAY_SAVED: return "replay saved";
    case HORIZON_EVENT_ONLINE_CHANGED: return "online changed";
    case HORIZON_EVENT_GAME_STATE: return "game state";
    default: return "event";
    }
}

}  // namespace

extern "C" {

// The info block lives as long as the DLL. The host reads it before Init and checks abi and games.
static const HorizonPluginInfo kInfo = {
    HORIZON_ABI,                                         // abi: always the header's value
    "sample",                                            // id: lowercase [a-z0-9-], same as the folder
    "Sample",                                            // name shown in the menu
    "1.0.0",                                             // version
    HORIZON_GAME_ALL,                                    // games this plugin can run in
    HORIZON_CAP_MENU | HORIZON_CAP_OVERLAY | HORIZON_CAP_GAME_EVENTS, // which optional exports it has
    HORIZON_FLAG_ONLINE_OK | HORIZON_FLAG_LIVE_TOGGLE,   // allowed online, can be toggled live
    "cheatoskar",
    "Example plugin: lists the last game events and draws a small overlay.",
    HORIZON_ABI_MINOR,                                   // minor the plugin was built against
};

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) { return &kInfo; }

// Return 0 to load. Any other number makes the host show the plugin as "Failed" with that number.
HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    if (!host || host->abi != HORIZON_ABI) return 1;
    g_host = host;

    // Attach our ImGui copy to the host's context. If it fails we still work, just without a page.
    g_imgui = horizon_imgui_attach(host);
    if (!g_imgui) host->log(host->plugin, HORIZON_LOG_WARN, "ImGui attach failed, no menu page");

    // Settings are stored by the host per plugin.
    g_showOverlay = host->get_int(host->plugin, "ShowOverlay", 1) != 0;

    // A hotkey: id "ping", shown name "Ping", default key "" (none). The user can bind one in the menu.
    g_pingHotkey = host->register_hotkey(host->plugin, "ping", "Ping", "", onPing, nullptr);

    // Ask for the events we want. Without this, OnEvent is never called.
    host->subscribe(host->plugin, HORIZON_EVENTS_ALL);

    const HorizonGameInfo* gi = host->game_info();
    host->log(host->plugin, HORIZON_LOG_INFO, "loaded in %s", gi ? gi->game_name : "?");
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    // Stop threads, free memory. Do not use g_host after this.
    g_host = nullptr;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_SetEnabled(int32_t enabled) { g_enabled = enabled != 0; }

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_OnEvent(const HorizonEvent* e) {
    if (!e) return;
    // Check e->size before reading fields a newer host may have added; the fields used here are the first ones.
    switch (e->type) {
    case HORIZON_EVENT_CHECKPOINT: addLine("checkpoint %d at %d ms", e->checkpoint_index, e->time_ms); break;
    case HORIZON_EVENT_FINISH: addLine("finish %d ms%s", e->time_ms, e->is_pb ? " (best)" : ""); break;
    case HORIZON_EVENT_MAP_LOADED:
        addLine("map loaded: %s", (e->map && e->map->name) ? e->map->name : "?");
        break;
    default: addLine("%s", eventName(e->type)); break;
    }
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawMenu(void) {
    if (!g_imgui) return;
    if (ImGui::Checkbox("Show overlay", &g_showOverlay)) g_host->set_int(g_host->plugin, "ShowOverlay", g_showOverlay ? 1 : 0);
    const char* key = g_pingHotkey ? g_host->hotkey_text(g_host->plugin, g_pingHotkey) : "";
    ImGui::Text("Ping key: %s", (key && *key) ? key : "none (set one in the hotkey list)");
    ImGui::Separator();
    if (g_lineCount == 0) ImGui::TextUnformatted("No events yet.");
    // Oldest first.
    const int first = (g_lineNext - g_lineCount + kMaxLines) % kMaxLines;
    for (int i = 0; i < g_lineCount; i++) ImGui::TextUnformatted(g_lines[(first + i) % kMaxLines]);
}

// Called every frame while enabled and not gated. Draw with the foreground list: no window needed.
HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawOverlay(void) {
    if (!g_imgui || !g_enabled || !g_showOverlay) return;
    const int last = (g_lineNext - 1 + kMaxLines) % kMaxLines;
    char text[128];
    snprintf(text, sizeof(text), "sample: %s", g_lineCount ? g_lines[last] : "waiting");
    ImGui::GetForegroundDrawList()->AddText(ImVec2(12.0f, 12.0f), IM_COL32(255, 255, 255, 220), text);
}

}  // extern "C"
