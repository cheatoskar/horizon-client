#include "stub_host.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"

namespace stubhost {

std::map<std::string, std::string> settings;
std::string logs;
HorizonGameInfo gameInfo;
HorizonStyle style;
fake::Graph graph;

namespace {

std::string g_dataFolder;
std::wstring g_dataFolderW;

void HORIZON_CALL h_log(HorizonPluginHandle, int32_t, const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("  log: %s\n", b);
    logs += b;
    logs += "\n";
}
int32_t HORIZON_CALL h_get_string(HorizonPluginHandle, const char* k, const char* fb, char* out, uint32_t size) {
    if (!size) return 0;
    auto it = settings.find(k);
    strncpy_s(out, size, it == settings.end() ? fb : it->second.c_str(), _TRUNCATE);
    return static_cast<int32_t>(strlen(out));
}
void HORIZON_CALL h_set_string(HorizonPluginHandle, const char* k, const char* v) { settings[k] = v; }
int32_t HORIZON_CALL h_get_int(HorizonPluginHandle, const char* k, int32_t fb) {
    auto it = settings.find(k);
    return it == settings.end() ? fb : atoi(it->second.c_str());
}
void HORIZON_CALL h_set_int(HorizonPluginHandle, const char* k, int32_t v) { settings[k] = std::to_string(v); }
float HORIZON_CALL h_get_float(HorizonPluginHandle, const char*, float fb) { return fb; }
void HORIZON_CALL h_set_float(HorizonPluginHandle, const char*, float) {}
const char* HORIZON_CALL h_data_folder(HorizonPluginHandle) { return g_dataFolder.c_str(); }
const wchar_t* HORIZON_CALL h_data_folder_w(HorizonPluginHandle) { return g_dataFolderW.c_str(); }
void* HORIZON_CALL h_imgui_context(void) { return ImGui::GetCurrentContext(); }
int32_t HORIZON_CALL h_imgui_version(void) { return IMGUI_VERSION_NUM; }
void HORIZON_CALL h_imgui_allocators(void** a, void** f, void** u) {
    ImGuiMemAllocFunc af;
    ImGuiMemFreeFunc ff;
    ImGui::GetAllocatorFunctions(&af, &ff, u);
    *a = reinterpret_cast<void*>(af);
    *f = reinterpret_cast<void*>(ff);
}
const HorizonStyle* HORIZON_CALL h_style(void) { return &style; }
const HorizonGameInfo* HORIZON_CALL h_game_info(void) { return &gameInfo; }
void* HORIZON_CALL h_engine_pointer(uint32_t which) {
    switch (which) {
    case HORIZON_ENGINE_APP: return reinterpret_cast<void*>(graph.app);
    case HORIZON_ENGINE_RACE: return reinterpret_cast<void*>(graph.race);
    case HORIZON_ENGINE_CHALLENGE: return reinterpret_cast<void*>(graph.challenge);
    default: return nullptr;
    }
}
int32_t HORIZON_CALL h_register_hotkey(HorizonPluginHandle, const char*, const char*, const char*, HorizonCallback, void*) { return 0; }
const char* HORIZON_CALL h_hotkey_text(HorizonPluginHandle, int32_t) { return ""; }
void HORIZON_CALL h_subscribe(HorizonPluginHandle, uint32_t) {}
void HORIZON_CALL h_post_replay_saved(HorizonPluginHandle, const char*) {}
HorizonResult HORIZON_CALL h_play_map_file(HorizonPluginHandle, const char*, uint32_t) { return HORIZON_ERR_NOT_SUPPORTED; }
HorizonResult HORIZON_CALL h_return_to_menu(HorizonPluginHandle) { return HORIZON_ERR_NOT_SUPPORTED; }
int32_t HORIZON_CALL h_get_game_state(void) { return HORIZON_STATE_SOLO_RACE; }
int32_t HORIZON_CALL h_is_online(void) { return 0; }
int32_t HORIZON_CALL h_actions_allowed(HorizonPluginHandle) { return 1; }
void HORIZON_CALL h_run_on_game_thread(HorizonPluginHandle, HorizonCallback, void*) {}
void* HORIZON_CALL h_device(void) { return nullptr; }
void HORIZON_CALL h_set_device_callbacks(HorizonPluginHandle, const HorizonDeviceCallbacks*) {}
int32_t HORIZON_CALL h_menu_open(void) { return 1; }
void HORIZON_CALL h_request_restart_note(HorizonPluginHandle, const char*) {}
int32_t HORIZON_CALL h_reflect_offset(const char*, const char*) { return -1; }
void* HORIZON_CALL h_reflect_method(const char*, const char*) { return nullptr; }

HorizonColor rgba(float r, float g, float b, float a) { return HorizonColor{r, g, b, a}; }

} // namespace

void setup() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_dataFolder = exe;
    g_dataFolder.resize(g_dataFolder.find_last_of('\\') + 1);
    g_dataFolder += "data\\";
    CreateDirectoryA(g_dataFolder.c_str(), nullptr);
    g_dataFolderW.assign(g_dataFolder.begin(), g_dataFolder.end());

    memset(&gameInfo, 0, sizeof(gameInfo));
    gameInfo.size = sizeof(gameInfo);
    gameInfo.game = HORIZON_GAME_TMNF;
    gameInfo.game_name = "TrackMania Nations Forever";
    gameInfo.exe_version = "fake";
    gameInfo.exe_sha256 = "1111111111111111111111111111111111111111111111111111111111111111";
    gameInfo.exe_timestamp = 0; // unknown build: the plugin's reflect copy scans this exe and finds the fake list
    gameInfo.exe_base = 0x400000;
    gameInfo.language = "en";
    gameInfo.game_folder = "C:\\fake\\";
    gameInfo.user_folder = "";
    gameInfo.host_version = "0.0.0-test";
    gameInfo.loader = 2;

    // TM Horizon tokens (src/host/ui/ui.h)
    memset(&style, 0, sizeof(style));
    style.size = sizeof(style);
    style.accent = rgba(1.0f, 0.42f, 0.10f, 1.0f);
    style.accent2 = rgba(1.0f, 0.54f, 0.27f, 1.0f);
    style.accent3 = rgba(1.0f, 0.69f, 0.37f, 1.0f);
    style.accent_ink = rgba(0.10f, 0.04f, 0.01f, 1.0f);
    style.ok = rgba(0.24f, 0.86f, 0.52f, 1.0f);
    style.warn = rgba(0.95f, 0.76f, 0.31f, 1.0f);
    style.bad = rgba(1.0f, 0.36f, 0.36f, 1.0f);
    style.bg = rgba(0.039f, 0.047f, 0.067f, 0.97f);
    style.bg2 = rgba(0.063f, 0.075f, 0.102f, 1.0f);
    style.surface = rgba(0.106f, 0.118f, 0.141f, 1.0f);
    style.text = rgba(0.933f, 0.941f, 0.953f, 1.0f);
    style.text2 = rgba(0.765f, 0.788f, 0.820f, 1.0f);
    style.muted = rgba(0.557f, 0.592f, 0.639f, 1.0f);
    style.radius_sm = 10.0f;
    style.radius = 14.0f;
    style.radius_lg = 20.0f;
    style.scale = 1.0f;
}

HorizonHostApi api() {
    HorizonHostApi host = {};
    host.abi = HORIZON_ABI;
    host.size = sizeof(host);
    host.abi_minor = 0; // the explorer needs nothing past ABI 1.0
    host.plugin = reinterpret_cast<HorizonPluginHandle>(static_cast<uintptr_t>(0x1234));
    host.log = h_log;
    host.get_string = h_get_string;
    host.set_string = h_set_string;
    host.get_int = h_get_int;
    host.set_int = h_set_int;
    host.get_float = h_get_float;
    host.set_float = h_set_float;
    host.data_folder = h_data_folder;
    host.data_folder_w = h_data_folder_w;
    host.imgui_context = h_imgui_context;
    host.imgui_version = h_imgui_version;
    host.imgui_allocators = h_imgui_allocators;
    host.style = h_style;
    host.game_info = h_game_info;
    host.engine_pointer = h_engine_pointer;
    host.register_hotkey = h_register_hotkey;
    host.hotkey_text = h_hotkey_text;
    host.subscribe = h_subscribe;
    host.post_replay_saved = h_post_replay_saved;
    host.play_map_file = h_play_map_file;
    host.return_to_menu = h_return_to_menu;
    host.get_game_state = h_get_game_state;
    host.is_online = h_is_online;
    host.actions_allowed = h_actions_allowed;
    host.run_on_game_thread = h_run_on_game_thread;
    host.device = h_device;
    host.set_device_callbacks = h_set_device_callbacks;
    host.menu_open = h_menu_open;
    host.request_restart_note = h_request_restart_note;
    host.reflect_offset = h_reflect_offset;
    host.reflect_method = h_reflect_method;
    return host;
}

} // namespace stubhost
