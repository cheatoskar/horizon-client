// Dear ImGui on the game's device: the menu, the Horizon bar, the plugins' overlays, the window
// messages and the per-frame work (game poll, events, queued calls, hotkeys). Runs in our Present
// detour on the game's thread, after the device plugins' pre_present and before the call on.
#include "host/host.h"

#include <d3d9.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include "common/hotkeys.h"
#include "common/util.h"
#include "host/bridge.h"
#include "host/stats.h"
#include "host/panels.h"
#include "host/toasts.h"
#include "host/plugins.h"
#include "host/ui/ui.h"
#include "imgui.h"
#include "imgui_internal.h" // ErrorRecoveryStoreState / TryToRecoverState after a plugin threw
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace horizon {
namespace {

ImGuiContext* g_ctx = nullptr;
bool g_uiOff = false;      // the host's own frame threw three times: no Horizon UI, no input taken
bool g_testFault = false;  // tests\host_test.cpp: the next frame throws in host code
bool g_backends = false;   // ImGui's DX9/Win32 backends set up on the game's device
bool g_lost = false;       // between Reset before/after
HWND g_hwnd = nullptr;
WNDPROC g_origProc = nullptr;
bool g_menu = false;
bool g_barShown = false;
bool g_barEnabled = true;
int g_menuHotkey = 0;
HorizonStyle g_style = {};
std::vector<LogEntry> g_logCopy;
std::vector<ui::LogLine> g_logLines;
unsigned g_logSeen = 0;
bool g_shutdownDone = false;
bool g_swallowed[3] = {};
bool g_panelsShown = false; // a detached panel window was drawn in the last frame
bool g_pluginWinShown = false; // a free plugin window (bar, left click) was drawn in the last frame
// layout mode ("Edit layout")
bool g_layout = false;
int g_layoutHotkey = 0;
// Display: base scale from the screen height, the player's content scale and menu size (host.json ui.*)
float g_baseScale = 1.0f;
long g_screenH = 1080;
int g_contentPct = 100;      // effective
bool g_reloadFonts = false;  // a new content scale waits for the next frame
bool g_reloadIcons = false;  // a new module icon style waits for the next frame
volatile LONG g_wheelSteps = 0; // Ctrl + mouse wheel in the menu, collected by the window procedure
HorizonUiMetrics g_metrics = {};

int menuSizeAuto() { return g_screenH >= 1440 ? 80 : 85; }
int contentAuto() { return g_screenH >= 1440 ? 90 : 100; }
int menuSizeSetting() {
    const int v = hostSettingInt("ui.menu_size", 0);
    return v == 60 || v == 70 || v == 80 || v == 90 ? v : 0;
}
int contentSetting() {
    const int v = hostSettingInt("ui.content_scale", 0);
    return v >= 80 && v <= 125 ? v : 0;
}
int menuSizePct() { return menuSizeSetting() ? menuSizeSetting() : menuSizeAuto(); }
int contentPctWanted() { return contentSetting() ? contentSetting() : contentAuto(); }

// The UI scale in use: the player's (screen height x text size), made smaller when the menu would not fit
// the game window (TMUF at 640x480 on a 1440p screen). Plugins get the same through style() and ui_metrics().
long g_fitW = 0, g_fitH = 0; // the window size the current scale was fitted to
float effectiveScale(int contentPct) {
    const float user = g_baseScale * static_cast<float>(contentPct) / 100.0f;
    RECT rc = {};
    // no size (minimised, Alt+Tab): keep what is in use
    if (!g_hwnd || !GetClientRect(g_hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return g_fitW ? ui::scale() : user;
    g_fitW = rc.right - rc.left;
    g_fitH = rc.bottom - rc.top;
    return ui::fitScale(user, static_cast<float>(g_fitW), static_cast<float>(g_fitH), menuSizePct());
}

// The game is in a race (or the editor's test drive): no input taken on TMNF/TMUF then.
bool racing() {
    const int s = game().gameState();
    return s == HORIZON_STATE_SOLO_RACE || s == HORIZON_STATE_NET_RACE || s == HORIZON_STATE_END_RACE || s == HORIZON_STATE_EDITOR ||
           game().raceRunning();
}

// HorizonHost_SetCaptureActive (TMVS photos/clips): while set the host draws and takes nothing.
// Heartbeat: without a new call it ends by itself after kCaptureTimeoutMs.
constexpr ULONGLONG kCaptureTimeoutMs = 10000;
volatile LONG g_captureOn = 0;
volatile ULONGLONG g_captureLast = 0;

bool captureActive() {
    if (!g_captureOn) return false;
    if (GetTickCount64() - g_captureLast > kCaptureTimeoutMs) {
        if (InterlockedExchange(&g_captureOn, 0)) HWARN("capture: no call for 10 s, Horizon UI is back");
        return false;
    }
    return true;
}

void* hostAlloc(size_t size, void*) { return malloc(size); }
void hostFree(void* p, void*) { free(p); }

HorizonColor col(const ui::Rgba& c) { return HorizonColor{c.r, c.g, c.b, c.a}; }

void fillStyle() {
    const ui::Tokens& t = ui::tokens();
    const float s = ui::scale();
    g_style.size = sizeof(HorizonStyle);
    g_style.accent = col(t.accent);
    g_style.accent2 = col(t.accent2);
    g_style.accent3 = col(t.accent3);
    g_style.accent_ink = col(t.accentInk);
    g_style.ok = col(t.ok);
    g_style.warn = col(t.warn);
    g_style.bad = col(t.bad);
    g_style.bg = col(t.bg);
    g_style.bg2 = col(t.bg2);
    g_style.surface = col(t.surface);
    g_style.text = col(t.text);
    g_style.text2 = col(t.text2);
    g_style.muted = col(t.muted);
    g_style.radius_sm = t.radiusSm * s;
    g_style.radius = t.radius * s;
    g_style.radius_lg = t.radiusLg * s;
    g_style.scale = s;
    g_style.font_body = ui::fonts().body;
    g_style.font_title = ui::fonts().title;
    g_style.font_mono = ui::fonts().mono;
}

bool isKeyMsg(UINT m) {
    return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SYSCHAR || m == WM_DEADCHAR;
}
bool isMouseMsg(UINT m) { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }
int buttonOf(UINT m, bool& up) {
    up = m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP;
    if (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_LBUTTONDBLCLK) return 0;
    if (m == WM_RBUTTONDOWN || m == WM_RBUTTONUP || m == WM_RBUTTONDBLCLK) return 1;
    if (m == WM_MBUTTONDOWN || m == WM_MBUTTONUP || m == WM_MBUTTONDBLCLK) return 2;
    return -1;
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_DESTROY && !g_shutdownDone) {
        g_shutdownDone = true;
        HLOG("window: the game window closes: plugins shut down");
        bridgeStop();
        pluginsShutdownAll();
        statsStop();
        hostConfigSave(true);
    }
    if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) hotkeysKeyMessage(msg, w, l);
    if (g_backends && g_ctx && !g_uiOff && !captureActive()) {
        ImGui::SetCurrentContext(g_ctx);
        bool take = false;
        if (g_menu && msg == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(w) & MK_CONTROL)) {
            // Ctrl + wheel in the menu: text size, not a scroll
            InterlockedExchangeAdd(&g_wheelSteps, GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1);
            return 0;
        }
        if (g_menu || g_layout) {
            if (isKeyMsg(msg)) {
                // the game never turns keys into characters: make the WM_CHAR for a focused text field
                if (msg == WM_KEYDOWN && ImGui::GetIO().WantTextInput) {
                    MSG queued;
                    if (!PeekMessageW(&queued, h, WM_CHAR, WM_CHAR, PM_NOREMOVE)) {
                        MSG key{h, msg, w, l, 0, {}};
                        TranslateMessage(&key);
                    }
                }
                ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
                take = true;
            } else if (isMouseMsg(msg)) {
                ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
                take = true;
            } else {
                ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
            }
        } else {
            ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
            if (isMouseMsg(msg) && msg != WM_MOUSEMOVE && (ui::barHovered() || ImGui::GetIO().WantCaptureMouse)) take = true;
            // a text field in a free plugin window has the keyboard (the menu is closed, the game must not see the keys)
            if (g_pluginWinShown && isKeyMsg(msg) && ImGui::GetIO().WantTextInput) {
                if (msg == WM_KEYDOWN) {
                    MSG queued;
                    if (!PeekMessageW(&queued, h, WM_CHAR, WM_CHAR, PM_NOREMOVE)) {
                        MSG key{h, msg, w, l, 0, {}};
                        TranslateMessage(&key);
                    }
                }
                take = true;
            }
        }
        bool up = false;
        const int b = buttonOf(msg, up);
        if (b >= 0) {
            if (up) {
                take = g_swallowed[b];
                g_swallowed[b] = false;
            } else {
                g_swallowed[b] = take;
            }
        }
        if (take) return 0;
    }
    return CallWindowProcW(g_origProc, h, msg, w, l);
}

void openLauncher() {
    HWND w = FindWindowW(nullptr, L"TM Horizon");
    if (w) {
        ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
        HLOG("menu: TM Horizon brought to the front");
    } else {
        HLOG("menu: TM Horizon is not running");
    }
}

void openLogFolder() {
    const std::wstring folder = folderOf(logFilePath());
    if (!folder.empty()) ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void menuHotkey(void*) {
    if (g_layout) setLayoutMode(false, "the menu key");
    setMenuOpen(!g_menu, "its key");
}
void layoutHotkey(void*) { setLayoutMode(!g_layout, "its key"); }

// The content scale the player wants, fitted to the window, now applied: fonts rebuilt in place, style and
// metrics updated. Cheap when nothing changed (called every frame). Between frames only (no ImGui frame open).
void applyContentScale(bool force) {
    const int want = contentPctWanted();
    const float s = effectiveScale(want);
    if (!force && want == g_contentPct && std::fabs(s - ui::scale()) < 0.001f) return;
    g_contentPct = want;
    ui::reloadFonts(s);
    ui::applyStyle(s);
    fillStyle();
    ImGui_ImplDX9_InvalidateDeviceObjects(); // the font texture is made again by the next NewFrame
    const float user = g_baseScale * static_cast<float>(want) / 100.0f;
    HLOG("menu: text and icons %d%% (UI scale %.2f%s, window %ldx%ld)", want, s, s < user - 0.001f ? ", made smaller to fit" : "",
         g_fitW, g_fitH);
}

ui::Model buildModel() {
    ui::Model m;
    const HostState& s = state();
    m.hostVersion = kHostVersion;
    m.gameName = s.game.name;
    m.exeVersion = s.game.version;
    m.profileName = s.profileName;
    m.menuKey = hotkeyText(g_menuHotkey);
    m.logPath = utf8(logFilePath());
    m.safeMode = s.safeMode;
    m.online = isOnlineNow();
    m.runGate = runGate();
    m.gameState = gameStateText(game().gameState());
    for (auto& up : plugins()) {
        const Plugin* p = up.get();
        ui::PluginView v;
        v.id = p->id;
        v.name = p->name;
        v.version = p->version;
        v.author = p->author;
        v.description = p->description;
        v.status = p->status;
        v.statusText = p->statusText;
        if (p->running && p->enabled && pluginGated(p)) {
            v.status = ui::PluginStatus::Blocked;
            v.statusText = game().onlineKnown() ? "solo only: paused online" : "solo only: paused";
        }
        if (p->restartNote && v.status == ui::PluginStatus::On) v.statusText = "restart for some changes";
        v.enabled = pluginSwitchPosition(p);
        v.hasMenu = p->running && p->drawMenu != nullptr;
        v.liveToggle = p->live;
        v.restartToSwitch = pluginSwitchNeedsRestart(p);
        if (v.restartToSwitch)
            v.restartText = p->running ? "Applies after a restart." : "Needs a restart: this module hooks the game at start.";
        v.soloOnly = p->soloOnly;
        v.panels = panelViews(p->id);
        {
            float x = -1, y = -1, w = 0, h = 0;
            if (sscanf_s(hostSetting("pwin." + p->id, "").c_str(), "%f,%f,%f,%f", &x, &y, &w, &h) == 4) {
                v.winX = x;
                v.winY = y;
                v.winW = w;
                v.winH = h;
            }
        }
        v.minW = p->manifest.uiMinW;
        v.minH = p->manifest.uiMinH;
        v.prefW = p->manifest.uiPrefW;
        v.prefH = p->manifest.uiPrefH;
        for (auto& pv : v.panels) pv.label = panelTabLabel(pv);
        m.plugins.push_back(v);
    }
    for (const auto& r : hotkeyRows()) {
        ui::HotkeyView h;
        h.handle = r.handle;
        h.ownerId = r.owner;
        h.ownerName = r.ownerName;
        h.name = r.name;
        h.key = r.key;
        h.conflict = r.conflict;
        m.hotkeys.push_back(h);
    }
    if (g_menu) {
        const unsigned seen = logSnapshot(g_logSeen, g_logCopy);
        if (seen != g_logSeen) {
            g_logSeen = seen;
            g_logLines.clear();
            for (const auto& e : g_logCopy) g_logLines.push_back(ui::LogLine{e.level, e.text});
        }
        m.log = &g_logLines;
    }
    for (const auto& o : s.otherMods) m.otherMods.push_back(ui::OtherModView{o.name, o.version, o.file, o.kind, o.keys, o.note});
    m.barEnabled = g_barEnabled;
    m.menuHotkey = g_menuHotkey;
    m.menuKeySuggestions = {"Insert", "F2", "#", "+", "Ctrl+Tab"};
    m.onlineFix = (s.game.game == HORIZON_GAME_ESWC || s.game.game == HORIZON_GAME_ORIGINAL) ? (hostSettingInt("OnlineFix", 1) ? 1 : 0) : -1;
    // module groups (modules.json): only the features that apply to this game; a group without any is left out
    for (const ModuleGroup& grp : modulesConfig().groups) {
        ui::GroupView gv;
        gv.id = grp.id;
        gv.name = grp.name;
        gv.description = grp.description;
        for (const GroupMember& mem : grp.members) {
            ui::GroupFeatureView fv;
            fv.label = mem.label;
            if (mem.host) {
                if (mem.id != "login-fix" || m.onlineFix < 0) continue;
                fv.hostFeature = mem.id;
                fv.enabled = m.onlineFix == 1;
                fv.statusText = "Next start";
            } else {
                const ui::PluginView* pv = nullptr;
                for (const ui::PluginView& v : m.plugins)
                    if (v.id == mem.id) pv = &v;
                if (!pv || pv->status == ui::PluginStatus::WrongGame) continue;
                fv.pluginId = mem.id;
                fv.enabled = pv->enabled;
                fv.usable = pv->status != ui::PluginStatus::Failed && !s.safeMode;
                if (pv->status == ui::PluginStatus::Failed || pv->status == ui::PluginStatus::Blocked) fv.statusText = pv->statusText;
                else if (pv->status == ui::PluginStatus::NeedsRestart) fv.statusText = "Restart to apply";
            }
            gv.enabled = gv.enabled || fv.enabled;
            gv.features.push_back(fv);
        }
        if (!gv.features.empty()) m.groups.push_back(gv);
    }
    m.dnd = toastsDnd();
    m.holdToasts = toastsHold();
    m.bridgeEnabled = bridgeState() != HORIZON_BRIDGE_OFF || hostSettingInt("bridge.enabled", 1) != 0;
    m.bridgeText = bridgeStateText();
    m.bridgePaired = bridgePaired();
    m.bridgeState = bridgeState();
    m.menuSizeSetting = menuSizeSetting();
    m.menuSizeAuto = menuSizeAuto();
    m.menuSizePct = menuSizePct();
    m.contentSetting = contentSetting();
    m.contentAuto = contentAuto();
    m.contentPct = g_contentPct;
    m.layoutKey = hotkeyText(g_layoutHotkey);
    m.forever = (s.game.game & HORIZON_GAME_FOREVER) != 0;
    m.layoutAllowed = layoutAllowed(&m.layoutWhy);
    m.widgetOpacity = std::max(20, std::min(100, hostSettingInt("widgets.opacity", 65)));
    m.iconStyle = hostSettingInt("ui.icon_alt", 0) == 1 ? 1 : 0;
    if (g_menu) {
        for (const ToastHistoryEntry& h : toastsHistory()) m.recentToasts.push_back(ui::Model::Recent{h.owner, h.title, h.text, h.kind, h.missed});
        // the shared setup (the folder listing only while the menu is open)
        const SetupStatus st = setupStatus();
        m.setupAvailable = st.available;
        m.setupShared = st.wanted;
        m.setupRestart = st.restartNeeded;
        m.setupFiles = st.exports;
    }
    m.notes = s.notes;
    m.notes.push_back(std::string("Game: ") + game().status());
    if (s.tmvsLoaded) m.notes.push_back("TM Vibrant Shaders keeps its own Direct3D hook and menu.");
    return m;
}

ui::Actions makeActions() {
    ui::Actions a;
    a.setPluginEnabled = [](const std::string& id, bool on) { pluginSetEnabled(id, on); };
    a.drawPluginPage = [](const std::string& id) {
        const Plugin* p = findPlugin(id);
        if (p && p->running && p->drawMenu) pluginDrawMenu(id);
        else if (p && p->running) ImGui::TextDisabled("This mod has no settings.");
        else ImGui::TextDisabled("This mod is not running (%s).", p && !p->statusText.empty() ? p->statusText.c_str() : "off");
    };
    a.beginKeyCapture = [](int handle) { keyCaptureBegin(handle); };
    a.pollKeyCapture = [](int handle, std::string& out) { return keyCapturePoll(handle, out); };
    a.rebindHotkey = [](int handle, const std::string& key) { return hotkeyRebind(handle, key); };
    a.openLogFolder = &openLogFolder;
    a.openLauncher = &openLauncher;
    a.closeMenu = []() { setMenuOpen(false, "the menu"); };
    a.setGroupEnabled = [](const std::string& groupId, bool on) {
        const ModuleGroup* grp = modulesConfig().group(groupId);
        if (!grp) return;
        const uint32_t gameBit = state().game.game;
        for (const GroupMember& mem : grp->members) {
            if (mem.host) {
                if (mem.id == "login-fix" && (gameBit == HORIZON_GAME_ESWC || gameBit == HORIZON_GAME_ORIGINAL)) hostSetSettingInt("OnlineFix", on ? 1 : 0);
                continue;
            }
            const Plugin* p = findPlugin(mem.id);
            if (p && p->status != ui::PluginStatus::WrongGame && p->status != ui::PluginStatus::Failed) pluginSetEnabled(mem.id, on);
        }
        HLOG("menu: %s %s", grp->name.c_str(), on ? "on" : "off");
    };
    a.setOnlineFix = [](bool on) {
        hostSetSettingInt("OnlineFix", on ? 1 : 0);
        HLOG("menu: online login fix %s from the next start", on ? "on" : "off");
    };
    a.drawPanelTab = [](const std::string& pluginId, const std::string& key) { panelDrawTab(pluginId, key); };
    a.setDnd = [](bool on) { toastsSetDnd(on); };
    a.setHoldToasts = [](bool on) { toastsSetHold(on); };
    a.setBridgeEnabled = [](bool on) { bridgeSetEnabled(on); };
    a.forgetBridge = []() { bridgeForget(); };
    a.saveWindow = [](const std::string& id, float x, float y, float w, float h) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%.0f,%.0f,%.0f,%.0f", x, y, w, h);
        hostSetSetting("pwin." + id, buf);
    };
    a.setMenuSize = [](int pct) {
        hostSetSettingInt("ui.menu_size", pct);
        HLOG("menu: size %s", pct ? (std::to_string(pct) + "% of the screen").c_str() : "auto");
    };
    a.setContentScale = [](int pct) {
        hostSetSettingInt("ui.content_scale", pct ? std::max(80, std::min(125, pct)) : 0);
        g_reloadFonts = true;
    };
    a.toggleLayout = []() { setLayoutMode(!g_layout, "the menu"); };
    a.setIconStyle = [](int style) {
        hostSetSettingInt("ui.icon_alt", style == 1 ? 1 : 0);
        g_reloadIcons = true;
    };
    a.setWidgetOpacity = [](int pct) { hostSetSettingInt("widgets.opacity", std::max(20, std::min(100, pct))); };
    a.setBarEnabled = [](bool on) {
        g_barEnabled = on;
        hostSetSettingInt("bar", on ? 1 : 0);
        HLOG("menu: top-edge bar %s", on ? "on" : "off");
    };
    // shared setup: a toast says what happened
    auto toast = [](unsigned kind, const char* title, const std::string& text) {
        HorizonToast t = {};
        t.size = sizeof(t);
        t.kind = kind;
        t.title = title;
        t.text = text.c_str();
        toastsPush("Horizon", t);
    };
    a.setSharedSetup = [toast](bool on) {
        if (setupSetShared(on)) toast(HORIZON_TOAST_INFO, on ? "Shared setup on" : "Shared setup off", "Restart to apply.");
    };
    a.copySetupFromGame = [toast]() {
        if (setupCopyFromGame()) toast(HORIZON_TOAST_OK, "Setup copied", setupStatus().active ? "Restart to apply." : "");
    };
    a.resetSharedSetup = [toast]() {
        if (setupReset()) toast(HORIZON_TOAST_OK, "Shared setup reset", setupStatus().active ? "Restart to apply." : "");
    };
    a.exportSetup = [toast]() {
        std::string file, err;
        if (setupExport(file, err)) toast(HORIZON_TOAST_OK, "Setup exported", file);
        else toast(HORIZON_TOAST_ERROR, "Export failed", err);
    };
    a.importSetup = [toast](const std::string& file) {
        std::string err;
        if (setupImport(file, err)) toast(HORIZON_TOAST_OK, "Setup imported", "Restart to apply.");
        else toast(HORIZON_TOAST_ERROR, "Import failed", err);
    };
    a.openSetupFolder = []() {
        const SetupStatus st = setupStatus();
        if (st.folder.empty()) return;
        const std::wstring folder = st.folder + L"setups\\";
        ensureDir(folder);
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    return a;
}

bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool foreground() {
    HWND f = GetForegroundWindow();
    return f && (f == g_hwnd || GetAncestor(f, GA_ROOTOWNER) == GetAncestor(g_hwnd, GA_ROOTOWNER));
}

// The pointer touches the top edge of the game window's client area.
bool mouseAtTop() {
    POINT p;
    if (!GetCursorPos(&p) || !ScreenToClient(g_hwnd, &p)) return false;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    return p.y <= 2 && p.y >= -2 && p.x >= 0 && p.x < rc.right;
}

void render(IDirect3DDevice9* dev) {
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) return;
    IDirect3DSurface9* back = nullptr;
    IDirect3DSurface9* oldRt = nullptr;
    bool switched = false;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && SUCCEEDED(dev->GetRenderTarget(0, &oldRt))) {
        if (oldRt != back) switched = SUCCEEDED(dev->SetRenderTarget(0, back));
    }
    if (SUCCEEDED(dev->BeginScene())) {
        ImGui_ImplDX9_RenderDrawData(dd);
        dev->EndScene();
    }
    if (switched) dev->SetRenderTarget(0, oldRt);
    if (oldRt) oldRt->Release();
    if (back) back->Release();
}

} // namespace

void* imguiContext() { return g_ctx; }
void imguiAllocators(void** a, void** f, void** u) {
    if (a) *a = reinterpret_cast<void*>(&hostAlloc);
    if (f) *f = reinterpret_cast<void*>(&hostFree);
    if (u) *u = nullptr;
}
const HorizonStyle* hostStyle() { return &g_style; }

const HorizonUiMetrics* hostUiMetrics() {
    const ui::MenuMetrics& mm = ui::menuMetrics();
    g_metrics.size = sizeof(HorizonUiMetrics);
    g_metrics.scale = g_style.scale;
    g_metrics.content_scale = static_cast<float>(g_contentPct) / 100.0f;
    if (g_ctx) {
        const ImVec2 d = ImGui::GetIO().DisplaySize;
        g_metrics.display_w = d.x;
        g_metrics.display_h = d.y;
    }
    g_metrics.menu_w = mm.menuW;
    g_metrics.menu_h = mm.menuH;
    g_metrics.page_w = mm.pageW;
    g_metrics.page_h = mm.pageH;
    return &g_metrics;
}

bool layoutModeOn() { return g_layout; }

bool layoutAllowed(std::string* why) {
    // TMNF/TMUF (Competition Patch rules): never take input during a race, so only in the game's menus
    if ((state().game.game & HORIZON_GAME_FOREVER) != 0 && (racing() || game().gameState() != HORIZON_STATE_MAIN_MENU)) {
        if (why) *why = "TMNF/TMUF: game menus only";
        return false;
    }
    if (captureActive()) {
        if (why) *why = "Not during a capture";
        return false;
    }
    return true;
}

void setLayoutMode(bool on, const char* why) {
    if (on == g_layout) return;
    if (on && g_uiOff) return;
    std::string no;
    if (on && !layoutAllowed(&no)) {
        HorizonToast t = {};
        t.size = sizeof(t);
        t.kind = HORIZON_TOAST_INFO;
        t.title = "Edit layout";
        t.text = no.c_str();
        t.flags = HORIZON_TOAST_IMPORTANT; // the answer to a key press: shown now, also while driving
        toastsPush("Horizon", t);
        HLOG("layout: not now (%s)", no.c_str());
        return;
    }
    if (on && g_menu) setMenuOpen(false, "edit layout");
    g_layout = on;
    HLOG("layout: %s (%s)", on ? "on" : "off", why ? why : "");
    if (!on) {
        if (down(VK_ESCAPE)) dinputHoldUntilReleased(VK_ESCAPE);
        KeyCombo k;
        if (parseKey(hotkeyText(g_layoutHotkey), k) && !k.empty() && down(static_cast<int>(k.vk))) dinputHoldUntilReleased(static_cast<int>(k.vk));
        hostConfigSave(true);
    }
}

uint64_t createTextureRgba(uint32_t w, uint32_t h, const void* rgba) {
    auto* dev = static_cast<IDirect3DDevice9*>(gameDevice());
    if (!dev) return 0;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex) return 0;
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
        tex->Release();
        return 0;
    }
    const auto* src = static_cast<const unsigned char*>(rgba);
    for (uint32_t y = 0; y < h; y++) {
        unsigned char* row = static_cast<unsigned char*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
        for (uint32_t x = 0; x < w; x++) {
            const unsigned char* px = src + (static_cast<size_t>(y) * w + x) * 4;
            row[x * 4 + 0] = px[2]; // B
            row[x * 4 + 1] = px[1]; // G
            row[x * 4 + 2] = px[0]; // R
            row[x * 4 + 3] = px[3]; // A
        }
    }
    tex->UnlockRect(0);
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tex));
}

void releaseTexture(uint64_t id) {
    if (id) reinterpret_cast<IDirect3DTexture9*>(static_cast<uintptr_t>(id))->Release();
}
bool menuOpen() { return g_menu && !captureActive(); }

void setMenuOpen(bool open, const char* why) {
    if (open == g_menu) return;
    if (open && g_uiOff) return; // no frames are drawn any more: an open menu would only hold the input
    g_menu = open;
    HLOG("menu: %s (%s)", open ? "open" : "closed", why ? why : "");
    if (!open) {
        // the key that closed it must not reach the game afterwards
        if (down(VK_ESCAPE)) dinputHoldUntilReleased(VK_ESCAPE);
        KeyCombo k;
        if (parseKey(hotkeyText(g_menuHotkey), k) && !k.empty() && down(static_cast<int>(k.vk))) dinputHoldUntilReleased(static_cast<int>(k.vk));
    }
}

HorizonResult menuOpenPluginPage(const std::string& pluginId, const std::string& panelKey) {
    // a capture hides everything: opening the menu now would show it in the photo
    if (captureActive()) return HORIZON_ERR_WRONG_STATE;
    HorizonResult r = HORIZON_OK;
    std::string tab = panelKey;
    if (!tab.empty()) {
        bool found = false;
        for (const ui::PanelView& v : panelViews(pluginId)) found = found || v.key == tab;
        if (!found) {
            r = HORIZON_ERR_INVALID_ARG; // open on Settings anyway, the caller gets told
            tab.clear();
        }
    }
    if (g_menu && ui::pluginPageShown(pluginId)) {
        setMenuOpen(false, "open_page");
        return r;
    }
    ui::showPluginPage(pluginId);
    ui::selectPanelTab(pluginId, tab);
    setMenuOpen(true, "open_page");
    return r;
}

// Called at host start (before plugins load): the context exists from then on, fonts come with the device.
void overlayCreateContext() {
    IMGUI_CHECKVERSION();
    ImGui::SetAllocatorFunctions(&hostAlloc, &hostFree, nullptr);
    g_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false; // no ImGui debug tooltip in the game after a plugin threw
    g_barEnabled = hostSettingInt("bar", profile().valid ? (profile().bar ? 1 : 0) : 1) != 0;
    const std::string key = profile().valid && !profile().menuKey.empty() ? profile().menuKey : std::string("Insert");
    g_menuHotkey = hotkeyAdd("host", "Horizon", "menu", "Open menu", key.c_str(), &menuHotkey, nullptr);
    g_layoutHotkey = hotkeyAdd("host", "Horizon", "layout", "Edit layout", "Ctrl+Insert", &layoutHotkey, nullptr);
    wchar_t env[8] = {};
    if (state().loader == LoaderKind::Test && GetEnvironmentVariableW(L"HORIZON_TEST_MENU", env, 8) && env[0] == L'1') {
        g_menu = true;
        HLOG("menu: open (test program)");
        char page[64] = {};
        if (GetEnvironmentVariableA("HORIZON_TEST_PAGE", page, sizeof(page)) && page[0]) ui::showPage(page); // e.g. "plugin:performance"
    }
    if (state().loader == LoaderKind::Test && GetEnvironmentVariableW(L"HORIZON_TEST_LAYOUT", env, 8) && env[0] == L'1') {
        g_menu = false;
        g_layout = true; // UI screens of layout mode (tests\host_test.cpp layout)
        HLOG("layout: on (test program)");
    }
    fillStyle();
}

HWND overlayWindow() { return g_hwnd; }

void overlayDeviceCreated(void* device, HWND window, void* params) {
    (void)params;
    auto* dev = static_cast<IDirect3DDevice9*>(device);
    ImGui::SetCurrentContext(g_ctx);
    if (g_backends) {
        ImGui_ImplDX9_Shutdown();
        g_backends = false;
    }
    if (!g_hwnd) {
        g_hwnd = window;
        RECT rc = {};
        GetClientRect(window, &rc);
        // UI scale from the monitor the game is on: the device is often made at 640x480 first and reset
        // to the real size later (Sunrise), and the font atlas is built once.
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        long height = rc.bottom - rc.top;
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi)) height = std::max(height, mi.rcMonitor.bottom - mi.rcMonitor.top);
        g_screenH = height;
        g_baseScale = std::clamp(static_cast<float>(height) / 1080.0f, 1.0f, 2.0f);
        // the player's content scale (General -> Display, Ctrl + wheel) on top: 90 % by default at 1440p and up
        g_contentPct = contentPctWanted();
        const float scale = effectiveScale(g_contentPct); // fitted to the window (small windows: smaller)
        ui::loadFonts(scale);
        ui::applyStyle(scale);
        fillStyle();
        ImGui_ImplWin32_Init(window);
        g_origProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndProc)));
        hotkeysDetectExternal();
        HLOG("overlay: ready on window %p (%ldx%ld, UI scale %.2f of %.2f x %d%%, menu %d%% of the screen); menu key %s, layout key %s",
             static_cast<void*>(window), rc.right - rc.left, rc.bottom - rc.top, scale, g_baseScale, g_contentPct, menuSizePct(),
             hotkeyText(g_menuHotkey).c_str(), hotkeyText(g_layoutHotkey).c_str());
    }
    g_backends = ImGui_ImplDX9_Init(dev);
    g_lost = false;
    static bool s_textures = false;
    if (!s_textures) {
        s_textures = true;
        texturesLoad();
    }
}

void overlayBeforeReset() {
    if (!g_backends) return;
    ImGui::SetCurrentContext(g_ctx);
    ImGui_ImplDX9_InvalidateDeviceObjects();
    g_lost = true;
}

void overlayAfterReset(bool ok) {
    if (!g_backends) return;
    g_lost = !ok;
}

void overlayFrame(void* device) {
    auto* dev = static_cast<IDirect3DDevice9*>(device);
    if (!state().mainThread) {
        state().mainThread = GetCurrentThreadId();
        HLOG("overlay: first frame on thread %lu (the game's thread from now on)", state().mainThread);
    }
    // game work first: state, events, queued plugin calls
    game().frame();
    eventsFrame();
    pluginsRunQueued();
    bridgeDeliver();
    const bool pluginCaptureNow = (hostFlagsAll() & HORIZON_HOST_FLAG_CAPTURE) != 0;
    toastsStep(captureActive() || pluginCaptureNow);
    if (!g_backends || g_lost || !g_ctx) return;
    if (g_uiOff) {
        // the UI is off after repeated errors; game work and plugin events above keep running
        if (game().usesDirectInputBlock()) dinputSetBlocked(false);
        return;
    }
    ImGui::SetCurrentContext(g_ctx);
    // modules switched on in the menu last frame: loaded now, outside the ImGui frame
    pluginsLoadPending();
    // content scale: Ctrl + wheel in the menu, or the slider on General
    if (const LONG steps = InterlockedExchange(&g_wheelSteps, 0)) {
        const int pct = std::max(80, std::min(125, g_contentPct + 5 * static_cast<int>(steps)));
        hostSetSettingInt("ui.content_scale", pct);
        g_reloadFonts = true;
    }
    // a new content scale, or a window size the menu no longer fits (Reset, resize): fonts again
    g_reloadFonts = false;
    applyContentScale(false);
    if (g_reloadIcons) {
        g_reloadIcons = false;
        texturesReloadPluginIcons();
    }
    const bool fg = foreground();
    hotkeysPoll(fg, (g_menu || g_layout) && ImGui::GetIO().WantTextInput, g_menu);
    static bool s_escPrev = false;
    const bool esc = down(VK_ESCAPE);
    if (g_menu && fg && esc && !s_escPrev && !keyCaptureActive()) {
        // ui::drawMenu closes on Esc itself; this keeps the press from the game
        dinputHoldUntilReleased(VK_ESCAPE);
    }
    if (g_layout && fg && esc && !s_escPrev && !keyCaptureActive()) setLayoutMode(false, "Esc");
    s_escPrev = esc;
    // TMNF/TMUF: a race started (or a capture began) while the layout was being edited: hand the input back
    if (g_layout && !layoutAllowed(nullptr)) setLayoutMode(false, "a race started");
    pluginsPrePresent(dev);

    if (captureActive()) {
        if (game().usesDirectInputBlock()) dinputSetBlocked(false);
        g_barShown = false;
        g_panelsShown = false;
        hostConfigSave(false);
        pluginsSaveSettings(false);
        return;
    }
    const bool pluginCapture = (hostFlagsAll() & HORIZON_HOST_FLAG_CAPTURE) != 0; // a plugin takes a photo/video
    const bool top = g_barEnabled && !pluginCapture && fg && mouseAtTop();
    bool anyOverlay = panelsWantFrame(g_menu) || ui::anyPluginWindowOpen();
    for (auto& up : plugins())
        if (up->running && up->enabled && up->drawOverlay) anyOverlay = true;
    const bool toasts = !pluginCapture && toastsVisible();
    const bool need = g_menu || g_layout || g_barShown || top || anyOverlay || toasts;
    if (game().usesDirectInputBlock()) dinputSetBlocked(g_menu || g_layout);
    if (!need) {
        panelsBeginFrame();
        g_panelsShown = false;
        hostConfigSave(false);
        pluginsSaveSettings(false);
        return;
    }
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::GetIO().MouseDrawCursor = g_menu || g_layout || g_barShown || g_pluginWinShown;
    pluginsDrawOverlays();
    if (g_testFault) {
        // tests\host_test.cpp: host code throws inside an open window, as a broken host draw would
        g_testFault = false;
        ImGui::Begin("##hz-test-fault");
        ImGui::PushID("fault");
        throw std::runtime_error("test fault");
    }
    panelsBeginFrame();
    PanelsFrame pf;
    pf.layout = g_layout;
    pf.iconsClickable = !racing(); // a minimised widget's edge icon only takes a click outside a race
    if (!g_menu) g_panelsShown = panelsDrawWindows(pf);
    if (g_layout) panelsDrawLayout();
    const ui::Model model = buildModel();
    static ui::Actions actions = makeActions();
    bool openMenu = false;
    g_barShown = g_barEnabled && !pluginCapture && !g_layout ? ui::drawBar(model, actions, top, g_menu, &openMenu) : false;
    if (openMenu) setMenuOpen(true, "the bar");
    if (g_menu) {
        bool open = true;
        ui::drawMenu(model, actions, &open);
        if (!open) setMenuOpen(false, "the menu");
        pf.menuOpen = true;
        g_panelsShown = panelsDrawWindows(pf); // above the menu, so they can be moved and set up
    }
    g_pluginWinShown = ui::drawPluginWindows(model, actions); // free plugin windows, above the menu and the dim layer
    if (toasts) toastsDraw();
    ImGui::EndFrame();
    ImGui::Render();
    render(dev);
    hostConfigSave(false);
    pluginsSaveSettings(false);
}

namespace {
bool sehRecover(const ImGuiErrorRecoveryState* s) {
    __try {
        ImGui::ErrorRecoveryTryToRecoverState(s);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehEndFrame() {
    __try {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        if (g.WithinFrameScope) {
            ImGui::ErrorRecoveryTryToRecoverState(&g.StackSizesInNewFrame);
            ImGui::EndFrame(); // nothing rendered this frame; the next NewFrame starts clean
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool imguiGuardedCall(bool (*call)(void*), void* ctx) {
    ImGuiContext* c = ImGui::GetCurrentContext();
    if (!c || !c->WithinFrameScope) return call(ctx);
    ImGuiErrorRecoveryState before;
    ImGui::ErrorRecoveryStoreState(&before);
    if (call(ctx)) return true;
    if (!sehRecover(&before)) HERR("overlay: ImGui state not repaired after a plugin error");
    return false;
}

void overlayAfterFault(bool giveUp) {
    HERR("overlay: error while drawing: menu closed, input back to the game%s",
         giveUp ? "; Horizon UI off until the next start" : "");
    // first the input: the game must never stay frozen behind a menu that is no longer drawn
    g_menu = false;
    g_layout = false;
    g_barShown = g_panelsShown = g_pluginWinShown = false;
    for (bool& s : g_swallowed) s = false;
    dinputSetBlocked(false);
    if (giveUp) g_uiOff = true;
    if (g_ctx) {
        ImGui::SetCurrentContext(g_ctx);
        if (!sehEndFrame()) {
            HERR("overlay: ImGui frame not repaired: Horizon UI off until the next start");
            g_uiOff = true;
        }
    }
    HorizonToast t = {};
    t.size = sizeof(t);
    t.kind = HORIZON_TOAST_ERROR;
    t.title = g_uiOff ? "Horizon menu off" : "Menu closed";
    t.text = g_uiOff ? "After an error. Back after a restart." : "After an error. Details in the log.";
    toastsPush("Horizon", t);
}

} // namespace horizon

// Test program only (tests\host_test.cpp). action 1: the next frame throws in host code; 2: open the menu;
// 3: close the menu; 0: state. Returns bits 1 = menu open, 2 = layout mode, 4 = game input held back,
// 8 = Horizon UI off. -1 in a game.
extern "C" __declspec(dllexport) int __cdecl HorizonHost_TestOverlayFault(int action) {
    using namespace horizon;
    if (state().loader != LoaderKind::Test) return -1;
    if (action == 1) g_testFault = true;
    if (action == 2) setMenuOpen(true, "test program");
    if (action == 3) setMenuOpen(false, "test program");
    return (g_menu ? 1 : 0) | (g_layout ? 2 : 0) | (dinputBlocked() ? 4 : 0) | (g_uiOff ? 8 : 0);
}

// ---- for mods that are not plugins (TMVS): stable C exports, see docs/plugin-abi.md -------------
// Bits: 1 = menu open, 2 = bar visible, 4 = a detached panel visible, 8 = layout mode ("Edit layout");
// 0 = nothing of Horizon on screen.
extern "C" __declspec(dllexport) uint32_t __cdecl HorizonHost_GetUiState(void) {
    using namespace horizon;
    if (captureActive() || g_uiOff) return 0;
    return (g_menu ? 1u : 0u) | (g_barShown ? 2u : 0u) | (g_panelsShown ? 4u : 0u) | (g_layout ? 8u : 0u);
}

// active != 0: draw nothing, take no input. Last call wins; call it every frame while capturing
// (it ends by itself 10 s after the last call with active != 0).
extern "C" __declspec(dllexport) void __cdecl HorizonHost_SetCaptureActive(int active) {
    using namespace horizon;
    g_captureLast = GetTickCount64();
    const LONG v = active ? 1 : 0;
    if (InterlockedExchange(&g_captureOn, v) != v) HLOG("capture: %s (HorizonHost_SetCaptureActive)", v ? "on, Horizon UI hidden" : "off");
}

extern "C" __declspec(dllexport) uint32_t __cdecl HorizonHost_Version(void) { return horizon::kHostVersionNum; }
