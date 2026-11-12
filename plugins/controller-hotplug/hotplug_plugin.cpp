// Controller fix (controller-hotplug) - a Horizon Host module for TrackMania Nations ESWC 1.80
// TrackMania Sunrise (Win10 repack) and TrackMania Original (Online Patch v2 exe).
//
// The games enumerate DirectInput game controllers once, at start. This module watches for controllers that
// are plugged in later and registers them through the game's own enumeration callback, so they show up in the
// game's input settings and their saved bindings work like for a pad that was there at start. Replugging the
// same pad keeps its slot and bindings. Input values are never read for the game, changed or injected.
// Details: hp_game.h, docs/plugins/controller-hotplug.md.
//
// No hooks: the worker thread (hp_watch) asks for steps through run_on_game_thread.
// If the standalone build (controller_hotplug.dll) runs in this process, the module stays passive.
#define HORIZON_PLUGIN_BUILD
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "horizon_imgui.h"
#include "horizon_plugin.h"
#include "hp_service.h"

namespace {

constexpr char kVersion[] = "1.1.0";
constexpr char kId[] = "controller-hotplug";
constexpr char kName[] = "Controller fix";
constexpr char kEswcSha[] = "983bd12724739dcb0f876aeb3a4259154b068940f5167f57ede750ca452d8068";
constexpr char kSunriseSha[] = "e59dfc7eaa5314e85b2b0df6f40ef7ba01a23bd5a9525f84407f5dff301df4fe";
constexpr char kOriginalSha[] = "75e129d769f4595ddde54a8ef484162915bc2bf998c1a2139cc963294fe8175e";
constexpr char kStandaloneNote[] = "Controller fix paused: standalone active";

const HorizonHostApi* g_host = nullptr;
bool g_imgui = false;
bool g_passive = false;
bool g_fake = false;
HANDLE g_claim = nullptr;
hp::GameBackend* g_backend = nullptr;
hp::FakeGame* g_fakeGame = nullptr;
hp::Service* g_service = nullptr;
volatile LONG g_queued = 0;
volatile LONG g_alive = 0;

void HORIZON_CALL onTick(void*) {
    InterlockedExchange(&g_queued, 0);
    if (g_service) g_service->tick();
}

void kick(void*) {
    if (!g_alive || !g_host) return;
    if (InterlockedExchange(&g_queued, 1) == 0) g_host->run_on_game_thread(g_host->plugin, onTick, nullptr);
}

void logCb(void*, int level, const char* text) {
    if (!g_host) return;
    g_host->log(g_host->plugin, level == 0 ? HORIZON_LOG_INFO : level == 1 ? HORIZON_LOG_WARN : HORIZON_LOG_ERROR, "%s", text);
}

void toast(const char* title, uint32_t kind, uint32_t ms) {
    if (!g_host || g_host->abi_minor < 3 || !g_host->notify) {
        if (g_host) g_host->log(g_host->plugin, HORIZON_LOG_INFO, "toast: %s", title);
        return;
    }
    HorizonToast t = {};
    t.size = sizeof(t);
    t.kind = kind;
    t.title = title;
    t.text = "";
    t.duration_ms = ms;
    g_host->notify(g_host->plugin, &t);
}

void toastCb(void*, const char* title) { toast(title, HORIZON_TOAST_OK, 0); }

void HORIZON_CALL standaloneToast(void*) { toast(kStandaloneNote, HORIZON_TOAST_WARN, 8000); }

bool standaloneRunning() { return GetModuleHandleW(L"controller_hotplug.dll") != nullptr || hp::ownerClaimedElsewhere(); }

bool envSet(const wchar_t* name) {
    wchar_t buf[8];
    const DWORD n = GetEnvironmentVariableW(name, buf, 8);
    return n > 0 && n < 8 && buf[0] == L'1';
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
    HORIZON_FLAG_ONLINE_OK | HORIZON_FLAG_LIVE_TOGGLE,
    "cheatoskar",
    "Adds controllers plugged in while the game runs.",
    HORIZON_ABI_MINOR,
};

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) { return &kInfo; }

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    if (!host || host->abi != HORIZON_ABI) return 1;
    g_host = host;
    const HorizonGameInfo* gi = host->game_info ? host->game_info() : nullptr;
    const hp::GameTable* table = nullptr;
    if (gi && gi->exe_sha256 && gi->game == HORIZON_GAME_ESWC && _stricmp(gi->exe_sha256, kEswcSha) == 0) table = &hp::eswcTable();
    if (gi && gi->exe_sha256 && gi->game == HORIZON_GAME_SUNRISE && _stricmp(gi->exe_sha256, kSunriseSha) == 0) table = &hp::sunriseTable();
    if (gi && gi->exe_sha256 && gi->game == HORIZON_GAME_ORIGINAL && _stricmp(gi->exe_sha256, kOriginalSha) == 0) table = &hp::originalTable();
    if (!table) {
        host->log(host->plugin, HORIZON_LOG_WARN, "refusing to load: not TmNationsESWC 1.80, TmSunrise (Win10 repack) or TMORIGINAL.EXE (Online Patch v2) (sha256 %s)",
                  gi && gi->exe_sha256 ? gi->exe_sha256 : "?");
        return 2;
    }
    g_imgui = horizon_imgui_attach(host);
    host->log(host->plugin, HORIZON_LOG_INFO, "---- controller-hotplug %s start, %s ----", kVersion, table->name);
    if (standaloneRunning()) {
        g_passive = true;
        host->log(host->plugin, HORIZON_LOG_WARN, "standalone controller_hotplug.dll is active: module paused, no thread, no game calls");
        host->run_on_game_thread(host->plugin, standaloneToast, nullptr);
        return 0;
    }
    hp::claimOwner(&g_claim);
    g_fake = envSet(L"HORIZON_TEST_HOTPLUG"); // tests: a game in memory and devices from the environment
    hp::Game* game = nullptr;
    if (g_fake) {
        g_fakeGame = new hp::FakeGame();
        game = g_fakeGame;
        host->log(host->plugin, HORIZON_LOG_WARN, "test mode: fake game and fake device list");
    } else {
        g_backend = new hp::GameBackend(table, gi->exe_base ? gi->exe_base : 0x400000, &logCb, nullptr);
        game = g_backend;
    }
    g_service = new hp::Service();
    hp::ServiceCallbacks cb = {nullptr, &logCb, &toastCb, &kick};
    InterlockedExchange(&g_alive, 1);
    if (!g_service->start(game, cb, g_fake)) return 3;
    host->log(host->plugin, HORIZON_LOG_INFO, "ready: watching for controllers plugged in while the game runs");
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    InterlockedExchange(&g_alive, 0);
    if (g_service) {
        g_service->stop();
        delete g_service;
        g_service = nullptr;
    }
    delete g_backend;
    g_backend = nullptr;
    delete g_fakeGame;
    g_fakeGame = nullptr;
    hp::releaseOwner(g_claim);
    g_claim = nullptr;
    if (g_host) g_host->log(g_host->plugin, HORIZON_LOG_INFO, "shutdown");
    g_host = nullptr;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_SetEnabled(int32_t enabled) {
    if (g_service && !g_passive) g_service->setEnabled(enabled != 0);
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawMenu(void) {
    if (!g_imgui || !g_host) return;
    if (g_passive) {
        ImGui::TextUnformatted("Paused");
        ImGui::TextDisabled("Standalone active");
        return;
    }
    if (!g_service) return;
    if (g_backend && g_backend->failed()) {
        ImGui::TextUnformatted("Off");
        ImGui::TextDisabled("Unknown game code");
        return;
    }
    ImGui::TextDisabled("%s", g_service->notifications() ? "Watching" : "Polling");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set new controllers up in the game's input settings.");
    ImGui::Separator();
    std::vector<hp::Tracker::View> v;
    g_service->view(v);
    if (v.empty()) ImGui::TextDisabled("No controller");
    for (const auto& c : v) {
        const std::string name = hp::toUtf8(c.name);
        const char* state = !c.present ? "Unplugged" : c.inGame ? "In game" : "Waiting";
        ImGui::Bullet();
        ImGui::TextWrapped("%s  (%s)", name.c_str(), state); // long device names wrap in narrow windows
    }
    const hp::Hint h = g_service->hint();
    if (h == hp::Hint::XboxOff) ImGui::TextDisabled("Xbox controller off");
    if (h == hp::Hint::XInputNotListed) ImGui::TextDisabled("XInput only");
}

} // extern "C"
