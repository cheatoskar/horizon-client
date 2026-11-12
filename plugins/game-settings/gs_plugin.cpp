// Game settings (game-settings) - a Horizon Host module for TrackMania Nations ESWC 1.80, TrackMania Sunrise
// (Win10 repack) and TrackMania Original (Online Patch v2 exe).
//
// Volume (the game's Windows audio session), Music and Sounds (the game's own volumes, heard at once) and the
// window mode (the game's own setting, applies at the next start) from the menu. How and why: gs_game.h.
//
// The chosen values are this module's settings (horizon\settings\game-settings.json: Music, Sounds, WindowMode;
// -1 = the game's own) and are written into the game's settings at every start, in Init: the host starts inside
// the game's DirectInput8Create, before the game makes its Direct3D device and audio from those settings.
// Legacy Essentials has the same code (vendored) and leaves it to this module when the client runs.
#define HORIZON_PLUGIN_BUILD
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "horizon_imgui.h"
#include "horizon_plugin.h"
#include "gs_game.h"
#include "gs_master.h"

namespace {

constexpr char kVersion[] = "1.0.0";
constexpr char kId[] = "game-settings";
constexpr char kName[] = "Game settings";
constexpr char kEswcSha[] = "983bd12724739dcb0f876aeb3a4259154b068940f5167f57ede750ca452d8068";
constexpr char kSunriseSha[] = "e59dfc7eaa5314e85b2b0df6f40ef7ba01a23bd5a9525f84407f5dff301df4fe";
constexpr char kOriginalSha[] = "75e129d769f4595ddde54a8ef484162915bc2bf998c1a2139cc963294fe8175e";

const HorizonHostApi* g_host = nullptr;
bool g_imgui = false;
gs::Game* g_game = nullptr;
gs::MasterVolume* g_master = nullptr;
gs::Prefs g_prefs;
int g_windowAtStart = -1; // the mode the game started in (0/1), -1 unknown

void logCb(void*, int level, const char* text) {
    if (!g_host) return;
    g_host->log(g_host->plugin, level == 0 ? HORIZON_LOG_INFO : level == 1 ? HORIZON_LOG_WARN : HORIZON_LOG_ERROR, "%s", text);
}

int clampPct(int v) { return v < 0 ? -1 : v > 100 ? 100 : v; }

// label on the left, control filling the rest of the row
void rowLabel(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void volumeRow(const char* label, const char* id, bool music) {
    float v = -1.0f;
    const bool ok = g_game && (music ? g_game->readMusic(&v) : g_game->readSounds(&v));
    rowLabel(label);
    if (!ok) {
        ImGui::TextDisabled("Unavailable");
        return;
    }
    int pct = gs::toPercent(v);
    if (ImGui::SliderInt(id, &pct, 0, 100, "%d%%")) {
        if (music) g_game->setMusic(gs::fromPercent(pct));
        else g_game->setSounds(gs::fromPercent(pct));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        int& pref = music ? g_prefs.music : g_prefs.sounds;
        pref = pct;
        g_host->set_int(g_host->plugin, music ? "Music" : "Sounds", pct);
        g_host->log(g_host->plugin, HORIZON_LOG_INFO, "%s %d%%", music ? "music" : "sounds", pct);
    }
}

} // namespace

extern "C" {

static const HorizonPluginInfo kInfo = {
    HORIZON_ABI,
    kId,
    kName,
    kVersion,
    HORIZON_GAME_ESWC | HORIZON_GAME_SUNRISE | HORIZON_GAME_ORIGINAL,
    HORIZON_CAP_MENU,
    HORIZON_FLAG_ONLINE_OK,
    "cheatoskar",
    "Volume, music, sounds and window mode from the menu.",
    HORIZON_ABI_MINOR,
};

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) { return &kInfo; }

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    if (!host || host->abi != HORIZON_ABI) return 1;
    g_host = host;
    const HorizonGameInfo* gi = host->game_info ? host->game_info() : nullptr;
    const gs::GameTable* table = nullptr;
    if (gi && gi->exe_sha256 && gi->game == HORIZON_GAME_ESWC && _stricmp(gi->exe_sha256, kEswcSha) == 0) table = &gs::eswcTable();
    if (gi && gi->exe_sha256 && gi->game == HORIZON_GAME_SUNRISE && _stricmp(gi->exe_sha256, kSunriseSha) == 0) table = &gs::sunriseTable();
    if (gi && gi->exe_sha256 && gi->game == HORIZON_GAME_ORIGINAL && _stricmp(gi->exe_sha256, kOriginalSha) == 0) table = &gs::originalTable();
    if (!table) {
        host->log(host->plugin, HORIZON_LOG_WARN, "refusing to load: not TmNationsESWC 1.80, TmSunrise (Win10 repack) or TMORIGINAL.EXE (Online Patch v2) (sha256 %s)",
                  gi && gi->exe_sha256 ? gi->exe_sha256 : "?");
        return 2;
    }
    g_imgui = horizon_imgui_attach(host);
    host->log(host->plugin, HORIZON_LOG_INFO, "---- game-settings %s start, %s ----", kVersion, table->name);
    g_game = new gs::Game(table, gi->exe_base ? gi->exe_base : 0x400000, &logCb, nullptr);
    g_prefs.music = clampPct(host->get_int(host->plugin, "Music", -1));
    g_prefs.sounds = clampPct(host->get_int(host->plugin, "Sounds", -1));
    const int w = host->get_int(host->plugin, "WindowMode", -1);
    g_prefs.window = w == 0 || w == 1 ? w : -1;
    g_game->applyStart(g_prefs);
    bool fs = false;
    if (g_game->readFullscreen(&fs)) g_windowAtStart = fs ? 1 : 0;
    g_master = new gs::MasterVolume();
    g_master->start(&logCb, nullptr);
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    delete g_master;
    g_master = nullptr;
    delete g_game;
    g_game = nullptr;
    if (g_host) g_host->log(g_host->plugin, HORIZON_LOG_INFO, "shutdown");
    g_host = nullptr;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawMenu(void) {
    if (!g_imgui || !g_host || !g_game) return;
    ImGui::PushID("game-settings");

    // Volume: the game's Windows audio session
    const float master = g_master ? g_master->get() : -1.0f;
    rowLabel("Volume");
    if (master < 0.0f) {
        ImGui::TextDisabled("Unavailable");
    } else {
        int pct = gs::toPercent(master);
        if (ImGui::SliderInt("##volume", &pct, 0, 100, "%d%%")) g_master->set(gs::fromPercent(pct));
        if (ImGui::IsItemDeactivatedAfterEdit()) g_host->log(g_host->plugin, HORIZON_LOG_INFO, "volume %d%%", pct);
    }
    volumeRow("Music", "##music", true);
    volumeRow("Sounds", "##sounds", false);

    // Window mode: the game's setting, read when it starts
    ImGui::Separator();
    bool fs = false;
    const bool fsOk = g_game->readFullscreen(&fs);
    rowLabel("Window mode");
    if (!fsOk) {
        ImGui::TextDisabled("Unavailable");
    } else {
        static const char* kModes[] = {"Windowed", "Fullscreen"};
        int mode = fs ? 1 : 0;
        if (ImGui::Combo("##window", &mode, kModes, 2) && (mode == 1) != fs) {
            if (g_game->setFullscreen(mode == 1)) {
                g_prefs.window = mode;
                g_host->set_int(g_host->plugin, "WindowMode", mode);
                g_host->log(g_host->plugin, HORIZON_LOG_INFO, "window mode %s (next start)", kModes[mode]);
                if (mode != g_windowAtStart && g_host->request_restart_note) g_host->request_restart_note(g_host->plugin, "Window mode");
            }
        }
        if (g_windowAtStart >= 0 && mode != g_windowAtStart) ImGui::TextDisabled("Restart game");
    }
    ImGui::PopID();
}

} // extern "C"
