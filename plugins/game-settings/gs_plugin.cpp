// Game settings (game-settings) - a Horizon Host module for TrackMania Nations ESWC 1.80, TrackMania Sunrise
// (Win10 repack) and TrackMania Original (Online Patch v2 exe).
//
// Volume (the game's Windows audio session), Music (the game's own volume, heard at once; no Sounds slider, see
// gs::soundsSliderSupported) and the
// window mode (switched at once with the game's own StartFullscreen / StopFullscreen, between two frames; fullscreen
// in the monitor's own resolution) and the window size (presets that fit the monitor) from the menu. How and why:
// gs_game.h.
//
// The chosen values are this module's settings (horizon\settings\game-settings.json: Music, Sounds, WindowMode,
// WindowW/WindowH; -1 / 0 = the game's own) and are written into the game's settings at every start, in Init: the host starts inside
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

constexpr char kVersion[] = "1.2.0";
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
int g_windowWanted = -1; // the mode asked for in the menu, switched between two frames
gs::WinSize g_sizePref = {0, 0};   // chosen window size (client area), 0 = the game's own
gs::WinSize g_sizeWanted = {0, 0}; // asked for in the menu, applied between two frames
int g_startFrames = 0;             // overlay frames since start: the chosen size is applied once the game runs
bool g_startSizeDone = false;

// The game's main window: the largest visible window of the game thread without an owner (call on that thread).
BOOL CALLBACK pickLargest(HWND h, LPARAM lp) {
    HWND* best = reinterpret_cast<HWND*>(lp);
    RECT r, b;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || !GetWindowRect(h, &r)) return TRUE;
    if (!*best || !GetWindowRect(*best, &b) ||
        static_cast<long long>(r.right - r.left) * (r.bottom - r.top) > static_cast<long long>(b.right - b.left) * (b.bottom - b.top))
        *best = h;
    return TRUE;
}
HWND gameWindow() {
    HWND w = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), &pickLargest, reinterpret_cast<LPARAM>(&w));
    return w;
}

// The monitor the window is on: its own mode (pixels, refresh) and its work area.
bool monitorOf(HWND w, int* pw, int* ph, int* hz, RECT* work) {
    HMONITOR m = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!m || !GetMonitorInfoW(m, &mi)) return false;
    if (work) *work = mi.rcWork;
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return false;
    *pw = static_cast<int>(dm.dmPelsWidth);
    *ph = static_cast<int>(dm.dmPelsHeight);
    *hz = dm.dmDisplayFrequency > 1 ? static_cast<int>(dm.dmDisplayFrequency) : 0;
    return *pw > 0 && *ph > 0;
}

// The window frame around a client area of w x h (the window's own style).
bool outerSize(HWND win, int w, int h, int* ow, int* oh) {
    RECT r = {0, 0, w, h};
    if (!AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongW(win, GWL_STYLE)), GetMenu(win) != nullptr,
                            static_cast<DWORD>(GetWindowLongW(win, GWL_EXSTYLE))))
        return false;
    *ow = r.right - r.left;
    *oh = r.bottom - r.top;
    return true;
}

// The window's client area to w x h, centred on its monitor's work area. The game resets its device to the new
// size itself (gs_game.h). Windowed only.
bool resizeWindow(int w, int h) {
    bool fs = true;
    HWND win = gameWindow();
    RECT work, cur;
    int pw = 0, ph = 0, hz = 0, ow = 0, oh = 0;
    if (!win || !g_game->fullscreenNow(&fs) || fs || !monitorOf(win, &pw, &ph, &hz, &work) || !outerSize(win, w, h, &ow, &oh)) return false;
    if (GetClientRect(win, &cur) && cur.right == w && cur.bottom == h) return true;
    const int x = work.left + ((work.right - work.left) - ow) / 2;
    const int y = work.top + ((work.bottom - work.top) - oh) / 2;
    const BOOL ok = SetWindowPos(win, nullptr, x > work.left ? x : work.left, y > work.top ? y : work.top, ow, oh,
                                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    g_host->log(g_host->plugin, ok ? HORIZON_LOG_INFO : HORIZON_LOG_WARN, "window size %dx%d %s", w, h, ok ? "set" : "NOT set");
    return ok != FALSE;
}

// game thread, between two frames (gs::runBetweenFrames)
void switchWindow(void*) {
    if (!g_game || !g_host || g_windowWanted < 0) return;
    const int mode = g_windowWanted;
    g_windowWanted = -1;
    int pw = 0, ph = 0, hz = 0;
    HWND win = gameWindow();
    if (mode == 1 && !(win && monitorOf(win, &pw, &ph, &hz, nullptr))) pw = ph = hz = 0; // the game's own mode
    if (!g_game->switchWindowMode(mode == 1, pw, ph, hz)) return; // logged
    g_prefs.window = mode;
    g_host->set_int(g_host->plugin, "WindowMode", mode);
}

void sizeWindow(void*) {
    if (!g_game || !g_host || g_sizeWanted.w <= 0) return;
    const gs::WinSize want = g_sizeWanted;
    g_sizeWanted = {0, 0};
    if (!resizeWindow(want.w, want.h)) return;
    g_sizePref = want;
    g_host->set_int(g_host->plugin, "WindowW", want.w);
    g_host->set_int(g_host->plugin, "WindowH", want.h);
}

void startSize(void*) {
    if (g_game && g_host && g_sizePref.w > 0) resizeWindow(g_sizePref.w, g_sizePref.h);
}

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

// A 0..100 % slider: shows the game's value, applies a drag at most every 100 ms and once more when it is let
// go (gs::Throttle), so the game does not redo its sounds on every frame. true when an edit ended: *done = value.
struct Drag {
    bool active = false;
    int value = 0;
    gs::Throttle throttle;
};
Drag g_dragVolume, g_dragMusic, g_dragSounds;

bool slider(const char* id, Drag& d, int current, void (*apply)(int), int* done) {
    int pct = d.active ? d.value : current;
    ImGui::SliderInt(id, &pct, 0, 100, "%d%%");
    const bool active = ImGui::IsItemActive();
    const bool released = ImGui::IsItemDeactivated();
    if (active && !d.active) d.throttle.begin(current);
    if (active || released) {
        d.value = pct;
        if (d.throttle.due(pct, released, GetTickCount())) apply(pct);
    }
    d.active = active;
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    *done = pct;
    return true;
}

void applyMusic(int pct) { g_game->setMusic(gs::fromPercent(pct)); }
void applySounds(int pct) { g_game->setSounds(gs::fromPercent(pct)); }
void applyVolume(int pct) { g_master->set(gs::fromPercent(pct)); }

void volumeRow(const char* label, const char* id, bool music) {
    float v = -1.0f;
    const bool ok = g_game && (music ? g_game->readMusic(&v) : g_game->readSounds(&v));
    rowLabel(label);
    if (!ok) {
        ImGui::TextDisabled("Unavailable");
        return;
    }
    int pct = 0;
    if (slider(id, music ? g_dragMusic : g_dragSounds, gs::toPercent(v), music ? &applyMusic : &applySounds, &pct)) {
        int& pref = music ? g_prefs.music : g_prefs.sounds; // kept, and put into the game at every start
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
    HORIZON_CAP_MENU | HORIZON_CAP_OVERLAY,
    HORIZON_FLAG_ONLINE_OK,
    "cheatoskar",
    "Volume, music, window mode and window size from the menu.",
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
    // no Sounds slider on these games (gs::soundsSliderSupported): a value saved by 1.1.0 stays unused
    g_prefs.sounds = gs::soundsSliderSupported(table) ? clampPct(host->get_int(host->plugin, "Sounds", -1)) : -1;
    const int w = host->get_int(host->plugin, "WindowMode", -1);
    g_prefs.window = w == 0 || w == 1 ? w : -1;
    const int ww = host->get_int(host->plugin, "WindowW", 0), wh = host->get_int(host->plugin, "WindowH", 0);
    if (ww >= 640 && wh >= 480 && ww <= 16384 && wh <= 16384) g_sizePref = {ww, wh};
    g_game->applyStart(g_prefs);
    g_game->toggleOk(); // logs the check once
    g_master = new gs::MasterVolume();
    g_master->start(&logCb, nullptr);
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    gs::stopBetweenFrames();
    delete g_master;
    g_master = nullptr;
    delete g_game;
    g_game = nullptr;
    if (g_host) g_host->log(g_host->plugin, HORIZON_LOG_INFO, "shutdown");
    g_host = nullptr;
}

// Nothing is drawn: once the game runs (a second of frames), the chosen window size is put on the window once.
HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawOverlay(void) {
    if (g_startSizeDone || !g_game) return;
    if (g_sizePref.w <= 0 || g_prefs.window == 1) {
        g_startSizeDone = true;
        return;
    }
    if (++g_startFrames < 60) return;
    g_startSizeDone = gs::runBetweenFrames(&startSize, nullptr) || g_startFrames > 600;
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
        int pct = 0;
        if (slider("##volume", g_dragVolume, gs::toPercent(master), &applyVolume, &pct))
            g_host->log(g_host->plugin, HORIZON_LOG_INFO, "volume %d%%", pct);
    }
    volumeRow("Music", "##music", true);
    if (gs::soundsSliderSupported(g_game->table())) volumeRow("Sounds", "##sounds", false);

    // Window mode: the game's own switch, between two frames
    ImGui::Separator();
    bool fs = false;
    const bool fsOk = g_game->fullscreenNow(&fs);
    rowLabel("Window mode");
    if (!fsOk) {
        ImGui::TextDisabled("Unavailable");
    } else {
        static const char* kModes[] = {"Windowed", "Fullscreen"};
        int mode = fs ? 1 : 0;
        if (ImGui::Combo("##window", &mode, kModes, 2) && (mode == 1) != fs && g_windowWanted < 0) {
            g_windowWanted = mode;
            if (!gs::runBetweenFrames(&switchWindow, nullptr)) g_windowWanted = -1;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fullscreen uses your monitor's resolution.");

        // Window size: presets that fit the monitor (windowed only)
        HWND win = gameWindow();
        RECT work = {}, client = {};
        int pw = 0, ph = 0, hz = 0, ow = 0, oh = 0;
        const bool sizeOk = win && monitorOf(win, &pw, &ph, &hz, &work) && GetClientRect(win, &client);
        rowLabel("Window size");
        if (!sizeOk) {
            ImGui::TextDisabled("Unavailable");
        } else {
            // the frame around the client area: what is left of the work area for the client
            int fw = 0, fh = 0;
            if (!outerSize(win, 100, 100, &ow, &oh)) ow = oh = 100;
            fw = ow - 100;
            fh = oh - 100;
            gs::WinSize sizes[16];
            const int n = gs::windowSizes((work.right - work.left) - fw, (work.bottom - work.top) - fh, sizes, 16);
            char cur[32];
            _snprintf_s(cur, _TRUNCATE, "%ldx%ld", client.right, client.bottom);
            if (fs) ImGui::BeginDisabled();
            if (ImGui::BeginCombo("##size", cur)) {
                for (int i = 0; i < n; i++) {
                    char label[32];
                    _snprintf_s(label, _TRUNCATE, "%dx%d%s", sizes[i].w, sizes[i].h, sizes[i].w * 3 == sizes[i].h * 4 ? "  4:3" : "  16:9");
                    const bool sel = client.right == sizes[i].w && client.bottom == sizes[i].h;
                    if (ImGui::Selectable(label, sel) && !sel && g_sizeWanted.w == 0) {
                        g_sizeWanted = sizes[i];
                        if (!gs::runBetweenFrames(&sizeWindow, nullptr)) g_sizeWanted = {0, 0};
                    }
                }
                ImGui::EndCombo();
            }
            if (fs) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(fs ? "Windowed only." : "Sizes that fit your monitor. Kept for the next start.");
        }
    }
    ImGui::PopID();
}

} // extern "C"
