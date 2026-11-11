// fake_host: a stand-in for Horizon Host that loads one plugin DLL and exercises it.
//
//   fake_host.exe <plugin dll> [--game eswc|sunrise|tmnf] [--expect-init <n>]
//
// It fills a HorizonHostApi with stubs (log calls are printed, settings live in a std::map, the data
// folder is build\test\data\<id>\), creates a headless Dear ImGui context, calls Info and Init, sends a
// short event sequence, draws the menu and overlay inside a frame, fires the plugin's hotkeys, toggles
// SetEnabled, calls Shutdown and unloads the DLL. engine_pointer returns NULL, so a plugin that carries
// game addresses must not touch game code. Exit code 0 = everything behaved as expected.
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "horizon_plugin.h"

namespace {

const char* kEswcSha = "983bd12724739dcb0f876aeb3a4259154b068940f5167f57ede750ca452d8068";

struct Hotkey {
    std::string action, key;
    HorizonCallback cb;
    void* user;
};

std::map<std::string, std::string> g_settings;
std::vector<Hotkey> g_hotkeys;
std::vector<std::pair<HorizonCallback, void*>> g_queue;
std::string g_dataFolder;
std::wstring g_dataFolderW;
std::string g_pluginId = "plugin";
HorizonGameInfo g_gameInfo;
HorizonStyle g_style;
std::string g_exeDir;
uint32_t g_subscribed = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failures++;
}

// ---------------------------------------------------------------- host API stubs ------------
void HORIZON_CALL f_log(HorizonPluginHandle, int32_t level, const char* fmt, ...) {
    static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("  log[%s] %s: %s\n", (level >= 0 && level <= 3) ? names[level] : "?", g_pluginId.c_str(), buf);
}
int32_t HORIZON_CALL f_get_string(HorizonPluginHandle, const char* key, const char* fallback, char* out, uint32_t size) {
    auto it = g_settings.find(key);
    const std::string& v = it == g_settings.end() ? std::string(fallback) : it->second;
    if (!size) return 0;
    strncpy_s(out, size, v.c_str(), _TRUNCATE);
    return static_cast<int32_t>(strlen(out));
}
void HORIZON_CALL f_set_string(HorizonPluginHandle, const char* key, const char* value) { g_settings[key] = value; }
int32_t HORIZON_CALL f_get_int(HorizonPluginHandle, const char* key, int32_t fallback) {
    auto it = g_settings.find(key);
    return it == g_settings.end() ? fallback : atoi(it->second.c_str());
}
void HORIZON_CALL f_set_int(HorizonPluginHandle, const char* key, int32_t value) { g_settings[key] = std::to_string(value); }
float HORIZON_CALL f_get_float(HorizonPluginHandle, const char* key, float fallback) {
    auto it = g_settings.find(key);
    return it == g_settings.end() ? fallback : static_cast<float>(atof(it->second.c_str()));
}
void HORIZON_CALL f_set_float(HorizonPluginHandle, const char* key, float value) { g_settings[key] = std::to_string(value); }
const char* HORIZON_CALL f_data_folder(HorizonPluginHandle) { return g_dataFolder.c_str(); }
const wchar_t* HORIZON_CALL f_data_folder_w(HorizonPluginHandle) { return g_dataFolderW.c_str(); }

void* HORIZON_CALL f_imgui_context(void) { return ImGui::GetCurrentContext(); }
int32_t HORIZON_CALL f_imgui_version(void) { return IMGUI_VERSION_NUM; }
void HORIZON_CALL f_imgui_allocators(void** a, void** f, void** u) {
    ImGuiMemAllocFunc af;
    ImGuiMemFreeFunc ff;
    ImGui::GetAllocatorFunctions(&af, &ff, u);
    *a = reinterpret_cast<void*>(af);
    *f = reinterpret_cast<void*>(ff);
}
const HorizonStyle* HORIZON_CALL f_style(void) { return &g_style; }
const HorizonGameInfo* HORIZON_CALL f_game_info(void) { return &g_gameInfo; }
void* HORIZON_CALL f_engine_pointer(uint32_t) { return nullptr; }

int32_t HORIZON_CALL f_register_hotkey(HorizonPluginHandle, const char* action, const char* name, const char* key,
                                       HorizonCallback cb, void* user) {
    printf("  hotkey registered: %s \"%s\" default \"%s\"\n", action, name, key);
    g_hotkeys.push_back({action, key, cb, user});
    return static_cast<int32_t>(g_hotkeys.size());
}
const char* HORIZON_CALL f_hotkey_text(HorizonPluginHandle, int32_t handle) {
    return (handle >= 1 && handle <= static_cast<int32_t>(g_hotkeys.size())) ? g_hotkeys[handle - 1].key.c_str() : "";
}
void HORIZON_CALL f_subscribe(HorizonPluginHandle, uint32_t mask) { g_subscribed = mask; }
void HORIZON_CALL f_post_replay_saved(HorizonPluginHandle, const char* path) { printf("  replay saved posted: %s\n", path); }
HorizonResult HORIZON_CALL f_play_map_file(HorizonPluginHandle, const char*, uint32_t) { return HORIZON_ERR_NOT_SUPPORTED; }
HorizonResult HORIZON_CALL f_return_to_menu(HorizonPluginHandle) { return HORIZON_ERR_NOT_SUPPORTED; }
int32_t HORIZON_CALL f_get_game_state(void) { return HORIZON_STATE_MAIN_MENU; }
int32_t HORIZON_CALL f_is_online(void) { return 0; }
int32_t HORIZON_CALL f_actions_allowed(HorizonPluginHandle) { return 1; }
void HORIZON_CALL f_run_on_game_thread(HorizonPluginHandle, HorizonCallback cb, void* user) { g_queue.push_back({cb, user}); }
void* HORIZON_CALL f_device(void) { return nullptr; }
void HORIZON_CALL f_set_device_callbacks(HorizonPluginHandle, const HorizonDeviceCallbacks*) {}
int32_t HORIZON_CALL f_menu_open(void) { return 1; }
void HORIZON_CALL f_request_restart_note(HorizonPluginHandle, const char* why) { printf("  restart note: %s\n", why); }
int32_t HORIZON_CALL f_reflect_offset(const char*, const char*) { return -1; }
void* HORIZON_CALL f_reflect_method(const char*, const char*) { return nullptr; }
// ABI minor 1
uint64_t HORIZON_CALL f_create_texture_rgba(HorizonPluginHandle, uint32_t, uint32_t, const void*) { return 0; } // no device
void HORIZON_CALL f_release_texture(HorizonPluginHandle, uint64_t) {}
int g_saveCalls = 0;
HorizonResult HORIZON_CALL f_save_replay(HorizonPluginHandle, const char* name) {
    g_saveCalls++;
    printf("  save_replay(\"%s\") -> NOT_SUPPORTED (no game)\n", name ? name : "");
    return HORIZON_ERR_NOT_SUPPORTED;
}
// ABI minor 2: panels are kept and drawn in one window each, like the host's tab body
struct FakePanel {
    std::string key, title;
    uint32_t flags;
    HorizonPanelDrawFn draw;
    void* user;
};
std::vector<FakePanel> g_panels;
uint32_t g_hostFlags = 0;
HorizonResult HORIZON_CALL f_register_panel(HorizonPluginHandle, const HorizonPanelDef* d) {
    if (!d || d->size < sizeof(uint32_t) * 2 || !d->key || !*d->key || !d->title || !d->draw) return HORIZON_ERR_INVALID_ARG;
    for (auto& p : g_panels)
        if (p.key == d->key) {
            p = {d->key, d->title, d->flags, d->draw, d->user};
            return HORIZON_OK;
        }
    g_panels.push_back({d->key, d->title, d->flags, d->draw, d->user});
    printf("  panel \"%s\" (%s)%s\n", d->key, d->title, (d->flags & HORIZON_PANEL_HUD) ? " HUD" : "");
    return HORIZON_OK;
}
void HORIZON_CALL f_unregister_panel(HorizonPluginHandle, const char* key) {
    for (size_t i = 0; i < g_panels.size(); i++)
        if (key && g_panels[i].key == key) {
            g_panels.erase(g_panels.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
}
int32_t HORIZON_CALL f_panel_visible(HorizonPluginHandle, const char*) { return 1; }
void HORIZON_CALL f_set_host_flag(HorizonPluginHandle, uint32_t flag, int32_t on) { g_hostFlags = on ? (g_hostFlags | flag) : (g_hostFlags & ~flag); }
uint32_t HORIZON_CALL f_host_flags(void) { return g_hostFlags; }

void flushQueue() {
    std::vector<std::pair<HorizonCallback, void*>> q;
    q.swap(g_queue);
    for (auto& c : q) c.first(c.second);
}

void makeDirs(const std::string& path) {
    for (size_t i = 0; i < path.size(); i++)
        if (path[i] == '\\' && i > 2) CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
    CreateDirectoryA(path.c_str(), nullptr);
}

template <typename T> T proc(HMODULE m, const char* name) { return reinterpret_cast<T>(GetProcAddress(m, name)); }

HorizonEvent makeEvent(uint32_t type) {
    HorizonEvent e = {};
    e.size = sizeof(e);
    e.type = type;
    e.play_mode = HORIZON_MODE_SOLO;
    e.time_ms = -1;
    e.respawns = -1;
    return e;
}

}  // namespace

int main(int argc, char** argv) {
    // before the plugin loads: its C runtime copies the environment at load time
    SetEnvironmentVariableA("HORIZON_TEST_SAVE_REPLAY", "1"); // the sample calls save_replay once in Init
    const char* dll = nullptr;
    const char* gameName = "eswc";
    int expectInit = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) gameName = argv[++i];
        else if (!strcmp(argv[i], "--expect-init") && i + 1 < argc) expectInit = atoi(argv[++i]);
        else dll = argv[i];
    }
    if (!dll) {
        printf("usage: fake_host <plugin dll> [--game eswc|sunrise|tmnf] [--expect-init <n>]\n");
        return 2;
    }

    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_exeDir = exe;
    g_exeDir.resize(g_exeDir.find_last_of('\\') + 1);

    memset(&g_gameInfo, 0, sizeof(g_gameInfo));
    g_gameInfo.size = sizeof(g_gameInfo);
    g_gameInfo.exe_base = 0x400000;
    g_gameInfo.language = "en";
    g_gameInfo.game_folder = "C:\\fake\\";
    g_gameInfo.user_folder = "";
    g_gameInfo.host_version = "0.0.0-fake";
    g_gameInfo.loader = 1;
    if (!strcmp(gameName, "eswc")) {
        g_gameInfo.game = HORIZON_GAME_ESWC;
        g_gameInfo.game_name = "TrackMania Nations ESWC";
        g_gameInfo.exe_version = "0.1.7.9 (unofficial 1.80)";
        g_gameInfo.exe_sha256 = kEswcSha;
    } else if (!strcmp(gameName, "sunrise")) {
        g_gameInfo.game = HORIZON_GAME_SUNRISE;
        g_gameInfo.game_name = "TrackMania Sunrise";
        g_gameInfo.exe_version = "fake";
        g_gameInfo.exe_sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    } else if (!strcmp(gameName, "tmnf")) {
        g_gameInfo.game = HORIZON_GAME_TMNF;
        g_gameInfo.game_name = "TrackMania Nations Forever";
        g_gameInfo.exe_version = "fake";
        g_gameInfo.exe_sha256 = "1111111111111111111111111111111111111111111111111111111111111111";
        g_gameInfo.loader = 2;
    } else {
        printf("unknown game %s\n", gameName);
        return 2;
    }
    memset(&g_style, 0, sizeof(g_style));
    g_style.size = sizeof(g_style);
    g_style.scale = 1.0f;

    // Headless Dear ImGui: a display size, a built font atlas, frames without a renderer.
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &tw, &th);
    printf("fake host: game %s, plugin %s\n", gameName, dll);

    HMODULE mod = LoadLibraryA(dll);
    if (!mod) {
        printf("LoadLibrary failed (%lu)\n", GetLastError());
        return 1;
    }
    auto info = proc<HorizonPlugin_InfoFn>(mod, HORIZON_EXPORT_INFO);
    auto init = proc<HorizonPlugin_InitFn>(mod, HORIZON_EXPORT_INIT);
    auto shutdown = proc<HorizonPlugin_ShutdownFn>(mod, HORIZON_EXPORT_SHUTDOWN);
    auto drawMenu = proc<HorizonPlugin_DrawMenuFn>(mod, HORIZON_EXPORT_DRAW_MENU);
    auto drawOverlay = proc<HorizonPlugin_DrawOverlayFn>(mod, HORIZON_EXPORT_DRAW_OVERLAY);
    auto onEvent = proc<HorizonPlugin_OnEventFn>(mod, HORIZON_EXPORT_ON_EVENT);
    auto setEnabled = proc<HorizonPlugin_SetEnabledFn>(mod, HORIZON_EXPORT_SET_ENABLED);
    check(info && init && shutdown, "required exports Info/Init/Shutdown present");
    if (!info || !init || !shutdown) return 1;

    const HorizonPluginInfo* pi = info();
    check(pi && pi->abi == HORIZON_ABI, "Info.abi matches");
    if (!pi) return 1;
    g_pluginId = pi->id;
    printf("  info: id=%s name=\"%s\" version=%s games=0x%02x caps=0x%02x flags=0x%02x author=%s\n", pi->id, pi->name,
           pi->version, pi->games, pi->caps, pi->flags, pi->author);
    printf("  games bit for this run supported by plugin: %s (a real host would not load it otherwise)\n",
           (pi->games & g_gameInfo.game) ? "yes" : "no");
    check(((pi->caps & HORIZON_CAP_MENU) != 0) == (drawMenu != nullptr), "CAP_MENU matches the DrawMenu export");
    check(((pi->caps & HORIZON_CAP_OVERLAY) != 0) == (drawOverlay != nullptr), "CAP_OVERLAY matches the DrawOverlay export");
    check(((pi->caps & HORIZON_CAP_GAME_EVENTS) != 0) == (onEvent != nullptr), "CAP_GAME_EVENTS matches the OnEvent export");

    g_dataFolder = g_exeDir + "data\\" + g_pluginId + "\\";
    makeDirs(g_dataFolder);
    g_dataFolderW.assign(g_dataFolder.begin(), g_dataFolder.end());

    HorizonHostApi host = {};
    host.abi = HORIZON_ABI;
    host.size = sizeof(host);
    host.abi_minor = HORIZON_ABI_MINOR;
    host.plugin = reinterpret_cast<HorizonPluginHandle>(static_cast<uintptr_t>(0x1234));
    host.log = f_log;
    host.get_string = f_get_string;
    host.set_string = f_set_string;
    host.get_int = f_get_int;
    host.set_int = f_set_int;
    host.get_float = f_get_float;
    host.set_float = f_set_float;
    host.data_folder = f_data_folder;
    host.data_folder_w = f_data_folder_w;
    host.imgui_context = f_imgui_context;
    host.imgui_version = f_imgui_version;
    host.imgui_allocators = f_imgui_allocators;
    host.style = f_style;
    host.game_info = f_game_info;
    host.engine_pointer = f_engine_pointer;
    host.register_hotkey = f_register_hotkey;
    host.hotkey_text = f_hotkey_text;
    host.subscribe = f_subscribe;
    host.post_replay_saved = f_post_replay_saved;
    host.play_map_file = f_play_map_file;
    host.return_to_menu = f_return_to_menu;
    host.get_game_state = f_get_game_state;
    host.is_online = f_is_online;
    host.actions_allowed = f_actions_allowed;
    host.run_on_game_thread = f_run_on_game_thread;
    host.device = f_device;
    host.set_device_callbacks = f_set_device_callbacks;
    host.menu_open = f_menu_open;
    host.request_restart_note = f_request_restart_note;
    host.reflect_offset = f_reflect_offset;
    host.reflect_method = f_reflect_method;
    host.create_texture_rgba = f_create_texture_rgba;
    host.release_texture = f_release_texture;
    host.save_replay = f_save_replay;
    host.register_panel = f_register_panel;
    host.unregister_panel = f_unregister_panel;
    host.panel_visible = f_panel_visible;
    host.set_host_flag = f_set_host_flag;
    host.host_flags = f_host_flags;

    const int32_t r = init(&host);
    printf("  Init returned %d (expected %d)\n", r, expectInit);
    check(r == expectInit, "Init result as expected");
    if (r != 0) {
        // The host would show the plugin as Failed and never call anything else.
        FreeLibrary(mod);
        printf("fake host: %s\n", g_failures ? "FAILED" : "passed");
        return g_failures ? 1 : 0;
    }

    // Event sequence. engine_replay stays NULL: a plugin with game addresses must skip, not crash.
    if (onEvent) {
        printf("events (subscribed mask 0x%08x)\n", g_subscribed);
        HorizonMapInfo map = {};
        map.size = sizeof(map);
        map.path = "C:\\fake\\Tracks\\A01.Challenge.Gbx";
        map.uid = "fakeuid";
        map.name = "A01-Race";
        map.author = "Nadeo";
        map.environment = "Stadium";
        map.author_time = 27000;
        map.gold_time = 29000;
        map.silver_time = 32000;
        map.bronze_time = 38000;
        map.play_mode = HORIZON_MODE_SOLO;
        map.author_score = -1;
        HorizonEvent e = makeEvent(HORIZON_EVENT_MAP_LOADED);
        e.map = &map;
        onEvent(&e);
        e = makeEvent(HORIZON_EVENT_RACE_START);
        onEvent(&e);
        e = makeEvent(HORIZON_EVENT_CHECKPOINT);
        e.time_ms = 9500;
        e.checkpoint_index = 0;
        onEvent(&e);
        e = makeEvent(HORIZON_EVENT_FINISH);
        e.time_ms = 26500;
        e.is_local_player = 1;
        e.is_pb = 1;
        e.respawns = 0;
        onEvent(&e);
        e = makeEvent(HORIZON_EVENT_RACE_END);
        e.end_screen = 1;
        e.engine_replay = nullptr;
        onEvent(&e);
        e = makeEvent(HORIZON_EVENT_RACE_END); // without a pending finish: must do nothing
        e.end_screen = 0;
        onEvent(&e);
        flushQueue();
    }

    // One headless frame: menu inside a window, overlay on top.
    printf("frame\n");
    ImDrawData* dd = nullptr;
    for (int frame = 0; frame < 3; frame++) { // a new window is hidden on its first frame
        ImGui::NewFrame();
        ImGui::Begin("fake plugin page");
        if (drawMenu) drawMenu();
        ImGui::End();
        if (drawOverlay) drawOverlay();
        for (auto& p : g_panels) {
            ImGui::Begin((p.title + "###" + p.key).c_str());
            p.draw(p.user);
            ImGui::End();
        }
        ImGui::Render();
        dd = ImGui::GetDrawData();
    }
    check(dd != nullptr, "frame rendered headless");
    if (strcmp(pi->id, "sample") == 0) check(g_saveCalls == 1 && g_panels.size() == 2, "sample: save_replay called, two panels drawn");
    printf("  panels drawn: %d, save_replay calls: %d\n", static_cast<int>(g_panels.size()), g_saveCalls);
    printf("  draw lists: %d, vertices: %d\n", dd ? dd->CmdListsCount : 0, dd ? dd->TotalVtxCount : 0);

    printf("hotkeys: %d\n", static_cast<int>(g_hotkeys.size()));
    for (auto& h : g_hotkeys) {
        printf("  pressing \"%s\"\n", h.action.c_str());
        h.cb(h.user);
    }
    flushQueue();

    if (setEnabled) {
        printf("SetEnabled(0), SetEnabled(1)\n");
        setEnabled(0);
        if (onEvent) {
            HorizonEvent e = makeEvent(HORIZON_EVENT_FINISH);
            e.is_local_player = 1;
            e.is_pb = 1;
            e.time_ms = 1000;
            onEvent(&e);
        }
        setEnabled(1);
    }

    printf("Shutdown\n");
    shutdown();
    flushQueue();
    check(FreeLibrary(mod) != 0, "FreeLibrary");
    ImGui::DestroyContext();
    printf("fake host: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
