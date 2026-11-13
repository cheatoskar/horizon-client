// fake_host: a stand-in for Horizon Host that loads one plugin DLL and exercises it.
//
//   fake_host.exe <plugin dll> [--game eswc|sunrise|original|tmnf|tmuf] [--expect-init <n>]
//
// It fills a HorizonHostApi with stubs (log calls are printed, settings live in a std::map, the data
// folder is build\test\data\<id>\), creates a headless Dear ImGui context, calls Info and Init, sends a
// short event sequence, draws the menu and overlay inside a frame, fires the plugin's hotkeys, toggles
// SetEnabled, calls Shutdown and unloads the DLL. engine_pointer returns NULL, so a plugin that carries
// game addresses must not touch game code. Exit code 0 = everything behaved as expected.
#include <winsock2.h>
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
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
std::mutex g_queueMu; // run_on_game_thread may be called from a plugin thread
std::string g_dataFolder;
std::wstring g_dataFolderW;
std::string g_pluginId = "plugin";
HorizonGameInfo g_gameInfo;
HorizonStyle g_style;
std::string g_exeDir;
uint32_t g_subscribed = 0;
int g_failures = 0;
std::vector<std::string> g_logLines;   // everything the plugin logged (scenario checks)
std::vector<std::string> g_toasts;     // titles passed to notify
std::string g_standaloneDummy;         // --standalone-dummy <dll>: stands in for eswc_autosave.dll

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
    g_logLines.push_back(buf);
}
void HORIZON_CALL f_notify(HorizonPluginHandle, const HorizonToast* t) {
    printf("  toast[%u] %s\n", t ? t->kind : 0u, t && t->title ? t->title : "");
    if (t && t->title) g_toasts.push_back(t->title);
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
int32_t g_gameState = HORIZON_STATE_MAIN_MENU; // the performance scenarios change it
int32_t HORIZON_CALL f_get_game_state(void) { return g_gameState; }
int32_t HORIZON_CALL f_is_online(void) { return 0; }
int32_t HORIZON_CALL f_actions_allowed(HorizonPluginHandle) { return 1; }
void HORIZON_CALL f_run_on_game_thread(HorizonPluginHandle, HorizonCallback cb, void* user) {
    std::lock_guard<std::mutex> lk(g_queueMu);
    g_queue.push_back({cb, user});
}
void* HORIZON_CALL f_device(void) { return nullptr; }
HorizonDeviceCallbacks g_devCb = {}; // the performance scenarios drive pre/post_present with it
void HORIZON_CALL f_set_device_callbacks(HorizonPluginHandle, const HorizonDeviceCallbacks* cb) {
    if (cb) g_devCb = *cb;
    else g_devCb = HorizonDeviceCallbacks{};
}
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
// ABI 1.4: fixed metrics of a 1080p screen with the menu at 80 %
const HorizonUiMetrics* HORIZON_CALL f_ui_metrics(void) {
    static const HorizonUiMetrics m = {sizeof(HorizonUiMetrics), 1.0f, 1.0f, 1920.0f, 1080.0f, 1536.0f, 864.0f, 1100.0f, 700.0f};
    return &m;
}
int g_openPageCalls = 0;
HorizonResult HORIZON_CALL f_open_page(HorizonPluginHandle, const char* panel_key) {
    g_openPageCalls++;
    printf("  open_page(\"%s\")\n", panel_key ? panel_key : "");
    return HORIZON_OK;
}

void flushQueue() {
    std::vector<std::pair<HorizonCallback, void*>> q;
    {
        std::lock_guard<std::mutex> lk(g_queueMu);
        q.swap(g_queue);
    }
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

// ---------------------------------------------------------------- TMX Upload: acts as the launcher over real HTTP ----
namespace tmxtest {

struct HttpResp {
    int status = 0; // 0 = no connection
    std::string body;
    double secs = 0;
};

HttpResp httpCall(int port, const char* method, const std::string& path, const std::string& key = "", const std::string& body = "",
                  const char* hostHeader = nullptr, const char* ua = "ureq/2.9") {
    HttpResp r;
    const auto t0 = std::chrono::steady_clock::now();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return r;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(port));
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return r;
    }
    DWORD to = 20000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + (hostHeader ? std::string(hostHeader) : "127.0.0.1:" + std::to_string(port)) +
                      "\r\nUser-Agent: " + ua + "\r\n";
    if (!key.empty()) req += "X-TMX-Key: " + key + "\r\n";
    if (!body.empty()) req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n" + body;
    send(s, req.data(), static_cast<int>(req.size()), 0);
    std::string all;
    char buf[8192];
    for (;;) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        all.append(buf, static_cast<size_t>(n));
    }
    closesocket(s);
    if (all.rfind("HTTP/1.1 ", 0) == 0) r.status = atoi(all.c_str() + 9);
    const size_t he = all.find("\r\n\r\n");
    if (he != std::string::npos) r.body = all.substr(he + 4);
    r.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

std::string between(const std::string& s, const std::string& a, const std::string& b) {
    const size_t i = s.find(a);
    if (i == std::string::npos) return "";
    const size_t j = s.find(b, i + a.size());
    return j == std::string::npos ? "" : s.substr(i + a.size(), j - i - a.size());
}

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

void writeFileBytes(const std::string& path, const std::string& bytes) {
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "wb");
    if (f) {
        fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
    }
}

// A stand-in for the mod's bridge: answers every request with the ping body.
struct FakeBridge {
    SOCKET l = INVALID_SOCKET;
    int port = 0;
    std::atomic<bool> run{true};
    std::thread t;
    bool start() {
        for (int p = 27311; p < 27316; p++) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(static_cast<u_short>(p));
            if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(s, 4) == 0) {
                l = s;
                port = p;
                break;
            }
            closesocket(s);
        }
        if (l == INVALID_SOCKET) return false;
        t = std::thread([this]() {
            while (run) {
                fd_set rs;
                FD_ZERO(&rs);
                FD_SET(l, &rs);
                timeval tv = {0, 100000};
                if (select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
                SOCKET c = accept(l, nullptr, nullptr);
                if (c == INVALID_SOCKET) continue;
                char b[2048];
                recv(c, b, sizeof(b), 0);
                const std::string body = "{\"ok\":true,\"mod\":\"1.1.1\"}";
                const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                                         "\r\nConnection: close\r\n\r\n" + body;
                send(c, resp.data(), static_cast<int>(resp.size()), 0);
                closesocket(c);
            }
        });
        return true;
    }
    void stop() {
        run = false;
        if (t.joinable()) t.join();
        if (l != INVALID_SOCKET) closesocket(l);
        l = INVALID_SOCKET;
    }
};

} // namespace tmxtest

typedef int32_t(__cdecl* TmxTestHookFn)(int32_t op, int32_t arg, char* out, int32_t outSize);

static void tmxUploadTests(HMODULE mod, HorizonPlugin_InitFn init, HorizonPlugin_ShutdownFn shutdown, HorizonPlugin_SetEnabledFn setEnabled,
                           HorizonPlugin_OnEventFn onEvent, const HorizonHostApi* host, const char* gameName) {
    using namespace tmxtest;
    printf("tmx-upload: acting as the launcher over HTTP on 127.0.0.1\n");
    WSADATA wsa = {};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    auto hook = proc<TmxTestHookFn>(mod, "HorizonPlugin_TestHook");
    check(hook && setEnabled && onEvent, "test hook, SetEnabled and OnEvent exported");
    if (!hook || !setEnabled || !onEvent) return;
    auto status = [&]() {
        char b[96];
        hook(2, 0, b, sizeof(b));
        return std::string(b);
    };
    auto state = [&]() {
        char b[96];
        hook(4, 0, b, sizeof(b));
        return std::string(b);
    };
    auto waitMode = [&](const char* m, int ms) {
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() < ms) {
            if (status().rfind(m, 0) == 0) return true;
            Sleep(50);
        }
        return false;
    };
    hook(3, 300, nullptr, 0);
    if (!waitMode("serving", 6000)) {
        if (status().rfind("passive", 0) == 0) {
            printf("  SKIPPED: a real bridge answers on 27311-27315 (%s), nothing to test against\n", status().c_str());
            return;
        }
        check(false, "bridge serving after Init");
        return;
    }
    const int port = atoi(between(status() + ":", ":", ":").c_str());
    printf("  bridge on port %d\n", port);
    check(port >= 27311 && port <= 27315, "port in 27311-27315");
    const std::string expectSite = !strcmp(gameName, "eswc") ? "tmn" : !strcmp(gameName, "sunrise") ? "tms" : !strcmp(gameName, "original") ? "tmo" : "tmnf";

    HttpResp r = httpCall(port, "GET", "/v1/ping");
    check(r.status == 200 && has(r.body, "\"ok\":true") && has(r.body, "\"mod\""), "ping");
    r = httpCall(port, "GET", "/v1/ping", "", "", "evil.example:27311");
    check(r.status == 403, "rebound Host header refused");
    r = httpCall(port, "GET", "/v1/hello");
    check(r.status == 401, "hello without key: 401");
    r = httpCall(port, "GET", "/v1/next?wait=0", "nope");
    check(r.status == 401, "next with a wrong key: 401");

    // pairing: pending, No, cool-down, Allow, key
    r = httpCall(port, "POST", "/v1/pair");
    check(r.status == 200 && has(r.body, "\"pending\""), "pair: pending");
    check(state().rfind("asked", 0) == 0, "pair prompt is up");
    r = httpCall(port, "POST", "/v1/pair");
    check(has(r.body, "\"pending\""), "pair: still pending");
    hook(1, 0, nullptr, 0);
    check(state().rfind("idle", 0) == 0, "No closes the prompt");
    r = httpCall(port, "POST", "/v1/pair");
    check(has(r.body, "\"denied\"") && state().rfind("idle", 0) == 0, "pair after No: denied, no new prompt");
    hook(5, 0, nullptr, 0);
    r = httpCall(port, "POST", "/v1/pair");
    check(has(r.body, "\"pending\"") && state().rfind("asked", 0) == 0, "pair asks again after the cool-down");
    hook(1, 1, nullptr, 0);
    r = httpCall(port, "POST", "/v1/pair");
    const std::string key = between(r.body, "\"key\":\"", "\"");
    check(r.status == 200 && has(r.body, "\"ok\":true") && key.size() == 32, "pair: key after Allow");
    flushQueue();
    check(g_settings["Tokens"].find(key) != std::string::npos, "token saved in the plugin settings");

    r = httpCall(port, "GET", "/v1/hello", key);
    check(r.status == 200 && has(r.body, ("\"game\":\"" + expectSite + "\"").c_str()) && has(r.body, "\"mod\""), "hello with key");
    r = httpCall(port, "GET", "/v1/next?wait=0", key);
    check(r.status == 200 && has(r.body, "\"upload\":null"), "next: nothing yet");

    // events: a replay before any finish is ignored, then a local finish and its replay is queued
    const std::string dir = g_dataFolder;
    const std::string f1 = dir + "tmxtest1.Replay.Gbx";
    static const char kHead[] = "GBX\x06\0 header tmxtestuid \xff\x01\x02 body ";
    std::string bytes1(kHead, sizeof(kHead) - 1);
    bytes1 += std::string(5000, '\x7f');
    writeFileBytes(f1, bytes1);
    writeFileBytes(dir + "notes.txt", "x");
    HorizonMapInfo map = {};
    map.size = sizeof(map);
    map.uid = "tmxtestuid";
    map.name = "A01-Race";
    map.path = "C:\\fake\\A01.Challenge.Gbx";
    auto ev = [&](uint32_t type, int time, const char* path) {
        HorizonEvent e = makeEvent(type);
        e.map = &map;
        e.time_ms = time;
        e.is_local_player = 1;
        e.path = path;
        e.source = "test";
        onEvent(&e);
    };
    ev(HORIZON_EVENT_MAP_LOADED, -1, nullptr);
    ev(HORIZON_EVENT_REPLAY_SAVED, -1, f1.c_str());
    r = httpCall(port, "GET", "/v1/next?wait=0", key);
    check(has(r.body, "\"upload\":null"), "replay without a local finish is not queued");
    ev(HORIZON_EVENT_FINISH, 26500, nullptr);
    ev(HORIZON_EVENT_REPLAY_SAVED, -1, (dir + "missing.Replay.Gbx").c_str());
    ev(HORIZON_EVENT_REPLAY_SAVED, -1, (dir + "notes.txt").c_str());
    r = httpCall(port, "GET", "/v1/next?wait=0", key);
    check(has(r.body, "\"upload\":null"), "missing file and non-replay file are not queued");
    ev(HORIZON_EVENT_REPLAY_SAVED, -1, f1.c_str());
    r = httpCall(port, "GET", "/v1/next?wait=0", key, "", nullptr, "Mozilla/5.0");
    const std::string id1 = between(r.body, "\"id\":\"", "\"");
    check(!id1.empty() && has(r.body, "\"verified\":true") && has(r.body, "\"fileName\":\"tmxtest1.Replay.Gbx\"") && has(r.body, "\"mapName\":\"A01-Race\"") &&
              has(r.body, "\"timeMs\":26500") && has(r.body, ("\"site\":\"" + expectSite + "\"").c_str()) && has(r.body, "\"trackId\":0"),
          "next: the queued replay with id, site, map, time, verified");
    r = httpCall(port, "GET", "/v1/file?id=" + id1, key);
    check(r.status == 200 && r.body == bytes1, "file: the exact bytes");
    r = httpCall(port, "GET", "/v1/file?id=nope", key);
    check(r.status == 404, "file: unknown id 404");
    r = httpCall(port, "POST", "/v1/result", key, "{\"id\":\"" + id1 + "\",\"ok\":true,\"detail\":\"Replay uploaded to TMX by TM Horizon.\"}");
    check(r.status == 200 && state().find(":1:0") != std::string::npos, "result accepted and listed");
    r = httpCall(port, "POST", "/v1/result", key, "{\"id\":\"" + id1 + "\",\"ok\":true}");
    check(r.status == 404, "result twice: 404");
    r = httpCall(port, "GET", "/v1/next?wait=0", key);
    check(has(r.body, "\"upload\":null"), "next: queue empty after the result");

    // Upload last: the newest replay again (the stub host's save_replay says NOT_SUPPORTED, and the queue covers the finish anyway)
    hook(6, 0, nullptr, 0);
    r = httpCall(port, "GET", "/v1/next?wait=0", key);
    const std::string id2 = between(r.body, "\"id\":\"", "\"");
    check(!id2.empty() && id2 != id1, "Upload last offers the replay again with a new id");
    r = httpCall(port, "POST", "/v1/result", key, "{\"id\":\"" + id2 + "\",\"ok\":false,\"detail\":\"Not faster than your \\\"record\\\"\"}");
    check(r.status == 200 && state().find(":2:0") != std::string::npos, "result not ok accepted");

    // a second finish: the TMX copy replaces the full file of the same finish
    const std::string f2 = dir + "tmxtest2.Replay.Gbx", f3 = dir + "tmxtest2 TMX.Replay.Gbx";
    writeFileBytes(f2, "GBX tmxtestuid full");
    writeFileBytes(f3, "GBX tmxtestuid local only");
    ev(HORIZON_EVENT_FINISH, 25000, nullptr);
    ev(HORIZON_EVENT_REPLAY_SAVED, -1, f2.c_str());
    ev(HORIZON_EVENT_REPLAY_SAVED, -1, f3.c_str());
    r = httpCall(port, "GET", "/v1/next?wait=0", key);
    const std::string id3 = between(r.body, "\"id\":\"", "\"");
    check(has(r.body, "tmxtest2 TMX.Replay.Gbx") && has(r.body, "\"timeMs\":25000"), "TMX copy is offered");
    httpCall(port, "POST", "/v1/result", key, "{\"id\":\"" + id3 + "\",\"ok\":true}");
    r = httpCall(port, "GET", "/v1/next?wait=0", key);
    check(has(r.body, "\"upload\":null"), "the full file of that finish was dropped");

    // a map whose uid is not in the file: offered, not verified
    map.uid = "someotheruid";
    ev(HORIZON_EVENT_MAP_LOADED, -1, nullptr);
    ev(HORIZON_EVENT_FINISH, 30000, nullptr);
    ev(HORIZON_EVENT_REPLAY_SAVED, -1, f1.c_str());
    r = httpCall(port, "GET", "/v1/next?wait=0", key);
    check(has(r.body, "\"verified\":false"), "uid not in the file: verified false");
    httpCall(port, "POST", "/v1/result", key, "{\"id\":\"" + between(r.body, "\"id\":\"", "\"") + "\",\"ok\":false,\"detail\":\"x\"}");

    // long poll with nothing new
    r = httpCall(port, "GET", "/v1/next?wait=1", key);
    check(r.status == 200 && r.secs >= 0.8 && r.secs < 3.0, "next?wait=1 waits about a second");

    // restart: the token survives, Shutdown is quick and frees the port
    flushQueue();
    const auto t0 = std::chrono::steady_clock::now();
    shutdown();
    const double down = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("  Shutdown took %.2f s\n", down);
    check(down < 2.0, "Shutdown returns quickly");
    check(httpCall(port, "GET", "/v1/ping").status == 0, "port closed after Shutdown");
    check(init(host) == 0, "Init again");
    check(waitMode("serving", 6000), "serving again");
    const int port2 = atoi(between(status() + ":", ":", ":").c_str());
    r = httpCall(port2, "GET", "/v1/hello", key);
    check(r.status == 200, "paired app is not asked again after a restart");

    // SetEnabled(0) stops the server, an open long poll does not hold it up
    HttpResp slow;
    std::thread poller([&]() { slow = httpCall(port2, "GET", "/v1/next?wait=30", key); });
    Sleep(300);
    const auto t1 = std::chrono::steady_clock::now();
    setEnabled(0);
    const double off = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    poller.join();
    printf("  SetEnabled(0) took %.2f s\n", off);
    check(off < 2.0 && slow.status == 200, "SetEnabled(0) is quick even with a long poll open");
    check(httpCall(port2, "GET", "/v1/ping").status == 0, "no server while switched off");

    // coexistence: another bridge on a port -> passive, no second server; takeover when it goes
    FakeBridge fb;
    check(fb.start(), "fake bridge started");
    setEnabled(1);
    check(waitMode("passive", 6000), "passive while another bridge answers the ping");
    check(status() == "passive:0:" + std::to_string(fb.port), "passive status names that port");
    int listening = 0;
    for (int p = 27311; p < 27316; p++)
        if (p != fb.port && httpCall(p, "GET", "/v1/ping").status != 0) listening++;
    check(listening == 0, "no second bridge was started");
    fb.stop();
    check(waitMode("serving", 8000), "takes over after the other bridge is gone");
    printf("tmx-upload tests done\n");
    WSACleanup();
}

bool logHas(const char* part) {
    for (const auto& l : g_logLines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

// Autosave & Login-Fix: default on, off = no reaction, standalone ESWC Autosave present = passive.
// Each scenario loads the plugin DLL fresh (own globals) against the shared fake host.
struct ScenarioRun {
    HMODULE mod = nullptr;
    int32_t init = -1;
    HorizonPlugin_OnEventFn onEvent = nullptr;
    HorizonPlugin_SetEnabledFn setEnabled = nullptr;
    HorizonPlugin_ShutdownFn shutdown = nullptr;
};

ScenarioRun scenarioStart(const char* dll, HorizonHostApi* host, bool clearSettings) {
    ScenarioRun r;
    if (clearSettings) g_settings.clear();
    g_logLines.clear();
    g_toasts.clear();
    g_hotkeys.clear();
    g_subscribed = 0;
    r.mod = LoadLibraryA(dll);
    if (!r.mod) return r;
    auto init = proc<HorizonPlugin_InitFn>(r.mod, HORIZON_EXPORT_INIT);
    r.onEvent = proc<HorizonPlugin_OnEventFn>(r.mod, HORIZON_EXPORT_ON_EVENT);
    r.setEnabled = proc<HorizonPlugin_SetEnabledFn>(r.mod, HORIZON_EXPORT_SET_ENABLED);
    r.shutdown = proc<HorizonPlugin_ShutdownFn>(r.mod, HORIZON_EXPORT_SHUTDOWN);
    r.init = init ? init(host) : -1;
    flushQueue();
    return r;
}

void scenarioStop(ScenarioRun& r) {
    if (r.shutdown) r.shutdown();
    flushQueue();
    if (r.mod) FreeLibrary(r.mod);
}

void fireFinishAndEnd(HorizonPlugin_OnEventFn onEvent) {
    HorizonEvent e = makeEvent(HORIZON_EVENT_FINISH);
    e.time_ms = 26500;
    e.is_local_player = 1;
    e.is_pb = 1;
    onEvent(&e);
    e = makeEvent(HORIZON_EVENT_RACE_END);
    e.end_screen = 1;
    e.engine_replay = nullptr;
    onEvent(&e);
    flushQueue();
}

// Horizon Performance: a benchmark series from bench-request.json must start when a replay plays (the host reports
// HORIZON_STATE_REPLAY) and, on a game where it cannot see the replay (Sunrise: "loading"), by the benchmark key.
// While it waits, its line is drawn on screen (DrawOverlay, independent of the frame-time window).
void perfFrames(int count, int sleepMs) {
    for (int i = 0; i < count; i++) {
        if (g_devCb.pre_present) g_devCb.pre_present(g_devCb.user, nullptr);
        if (g_devCb.post_present) g_devCb.post_present(g_devCb.user, nullptr, 0);
        Sleep(static_cast<DWORD>(sleepMs));
        flushQueue();
    }
}

int perfBanner(HMODULE mod) {
    auto draw = proc<HorizonPlugin_DrawOverlayFn>(mod, HORIZON_EXPORT_DRAW_OVERLAY);
    if (!draw) return -1;
    int vtx = 0;
    for (int frame = 0; frame < 2; frame++) {
        ImGui::NewFrame();
        draw();
        vtx = ImGui::GetForegroundDrawList()->VtxBuffer.Size;
        ImGui::Render();
    }
    return vtx;
}

bool waitLog(const char* part, int maxFrames) {
    for (int i = 0; i < maxFrames && !logHas(part); i++) perfFrames(1, 5);
    return logHas(part);
}

bool seriesFileWritten() {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_dataFolder + "bench\\*_series.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

void perfScenarios(const char* dll, HorizonHostApi* host, bool replayDetected) {
    const char* req = "{\"version\": 1, \"label\": \"series\", \"series\": {\"mode\": \"replay\", \"seconds\": 1, \"settle\": 0.1, "
                      "\"runs\": 1, \"warmup\": 0, \"sets\": [{\"label\": \"baseline\"}]}}";
    // old results out of the way
    const std::string bench = g_dataFolder + "bench\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((bench + "*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA((bench + fd.cFileName).c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (FILE* f = fopen((g_dataFolder + "bench-request.json").c_str(), "wb")) {
        fputs(req, f);
        fclose(f);
    }
    printf("performance scenario: series in replay mode, %s\n", replayDetected ? "the host reports the replay" : "the replay is not visible (started by the key)");
    g_gameState = HORIZON_STATE_MAIN_MENU;
    g_devCb = HorizonDeviceCallbacks{};
    ScenarioRun r = scenarioStart(dll, host, true);
    check(r.init == 0 && g_devCb.pre_present && g_devCb.post_present, "Init ok, device callbacks set");
    check(logHas("benchmark series from the launcher"), "the request was read");
    if (r.init != 0 || !g_devCb.pre_present) {
        scenarioStop(r);
        return;
    }
    // waiting in the menu: no start, the line is on screen
    perfFrames(40, 5);
    check(!logHas("benchmark: starting"), "main menu: the series waits");
    check(perfBanner(r.mod) > 0, "waiting: the benchmark line is drawn (DrawOverlay)");
    check(!g_hotkeys.empty() && g_hotkeys[0].key == "Ctrl+Shift+B", "benchmark key Ctrl+Shift+B (Ctrl+F10 belongs to Legacy Randomizer)");
    // the replay plays
    g_gameState = replayDetected ? HORIZON_STATE_REPLAY : HORIZON_STATE_LOADING;
    perfFrames(40, 5);
    if (replayDetected) {
        check(logHas("benchmark: starting (the scene is on screen"), "replay state: the series starts by itself");
    } else {
        check(!logHas("benchmark: starting"), "loading (replay not visible): no start by itself");
        for (auto& k : g_hotkeys)
            if (k.action == "bench") k.cb(k.user);
        perfFrames(2, 5);
        check(logHas("benchmark: starting (by its key"), "the benchmark key starts it");
    }
    check(waitLog("benchmark series started", 1200), "warm-up over: series started");
    check(perfBanner(r.mod) > 0, "running: the progress line is drawn");
    check(waitLog("benchmark series done", 1200), "series done");
    check(seriesFileWritten(), "series file written in bench\\");
    scenarioStop(r);
    g_gameState = HORIZON_STATE_MAIN_MENU;
}

void autosaveScenarios(const char* dll, HorizonHostApi* host) {
    printf("scenario 1: default settings, nothing else installed\n");
    {
        ScenarioRun r = scenarioStart(dll, host, true);
        check(r.init == 0, "Init ok");
        check(g_subscribed != 0 && g_hotkeys.size() == 1, "default on: events subscribed, save hotkey registered");
        check(g_toasts.empty() && !logHas("paused"), "no standalone note");
        if (r.onEvent) {
            fireFinishAndEnd(r.onEvent);
            check(logHas("finish: local player") && logHas("race end after the finish"), "default on: a finish leads to a save attempt");
        }
        scenarioStop(r);
    }

    printf("scenario 2: switched off live (SetEnabled 0)\n");
    {
        ScenarioRun r = scenarioStart(dll, host, true);
        check(r.init == 0 && r.setEnabled && r.onEvent, "Init ok, exports present");
        if (r.init == 0 && r.setEnabled && r.onEvent) {
            r.setEnabled(0);
            g_logLines.clear();
            fireFinishAndEnd(r.onEvent);
            check(!logHas("finish: local player") && !logHas("race end after the finish") && !logHas("SAVED"),
                  "off: finish and race end do nothing");
            r.setEnabled(1);
            fireFinishAndEnd(r.onEvent);
            check(logHas("race end after the finish"), "on again: works again");
        }
        scenarioStop(r);
    }

    printf("scenario 3: standalone eswc_autosave.dll loaded in the process\n");
    if (g_standaloneDummy.empty()) {
        check(false, "--standalone-dummy <dll> missing");
    } else {
        const std::string dir = g_exeDir + "standalone\\";
        makeDirs(dir);
        const std::string copy = dir + "eswc_autosave.dll";
        check(CopyFileA(g_standaloneDummy.c_str(), copy.c_str(), FALSE) != 0, "dummy eswc_autosave.dll copied");
        HMODULE dummy = LoadLibraryA(copy.c_str());
        check(dummy != nullptr, "dummy loaded as eswc_autosave.dll");
        ScenarioRun r = scenarioStart(dll, host, true);
        check(r.init == 0, "Init ok (module stays loaded, passive)");
        check(g_subscribed == 0 && g_hotkeys.empty(), "passive: no events subscribed, no hotkey");
        check(logHas("module paused"), "log says: module paused");
        check(g_toasts.size() == 1 && g_toasts[0].find("Modul pausiert") != std::string::npos &&
                  g_toasts[0].find("Eigenst") != std::string::npos,
              "toast: Eigenstaendiges ESWC Autosave gefunden - Modul pausiert");
        if (r.onEvent) {
            g_logLines.clear();
            fireFinishAndEnd(r.onEvent);
            check(!logHas("finish: local player") && !logHas("SAVED"), "passive: events do nothing");
        }
        // the page shows the note
        for (int frame = 0; frame < 3; frame++) {
            ImGui::NewFrame();
            ImGui::Begin("autosave page");
            auto draw = proc<HorizonPlugin_DrawMenuFn>(r.mod, HORIZON_EXPORT_DRAW_MENU);
            if (draw) draw();
            ImGui::End();
            ImGui::Render();
        }
        check(GetFileAttributesA(copy.c_str()) != INVALID_FILE_ATTRIBUTES, "standalone file untouched");
        scenarioStop(r);
        if (dummy) FreeLibrary(dummy);
        DeleteFileA(copy.c_str());
    }

    printf("scenario 4: standalone files in the game folder (not loaded)\n");
    {
        const std::string dir = g_exeDir + "gamefolder\\";
        makeDirs(dir);
        const std::string dll1 = dir + "eswc_autosave.dll", chain = dir + "dinput8.chain.dll";
        HANDLE h = CreateFileA(dll1.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        g_gameInfo.game_folder = dir.c_str();
        ScenarioRun r = scenarioStart(dll, host, true);
        check(r.init == 0 && g_subscribed != 0, "only eswc_autosave.dll left over (not chained): module active");
        scenarioStop(r);
        h = CreateFileA(chain.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        r = scenarioStart(dll, host, true);
        check(r.init == 0 && g_subscribed == 0 && logHas("module paused"), "eswc_autosave.dll + dinput8.chain.dll: module paused");
        check(GetFileAttributesA(dll1.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesA(chain.c_str()) != INVALID_FILE_ATTRIBUTES,
              "standalone files untouched");
        scenarioStop(r);
        DeleteFileA(dll1.c_str());
        DeleteFileA(chain.c_str());
        RemoveDirectoryA(dir.c_str());
        g_gameInfo.game_folder = "C:\\fake\\";
    }
}

// Controller fix (controller-hotplug): test mode (HORIZON_TEST_HOTPLUG=1) gives the module a game in memory and
// takes the attached controllers from HORIZON_TEST_HOTPLUG_DEVICES ("1:Name;2:Name"), read by its worker thread.
bool toastHas(const char* part) {
    for (const auto& t : g_toasts)
        if (t.find(part) != std::string::npos) return true;
    return false;
}
// runs queued game-thread calls for up to ms, until pred() holds
template <typename Pred> bool pumpUntil(Pred pred, int ms) {
    for (int t = 0; t < ms; t += 20) {
        flushQueue();
        if (pred()) return true;
        Sleep(20);
    }
    flushQueue();
    return pred();
}

void drawPage(HMODULE mod) {
    for (int frame = 0; frame < 2; frame++) {
        ImGui::NewFrame();
        ImGui::Begin("hotplug page");
        auto draw = proc<HorizonPlugin_DrawMenuFn>(mod, HORIZON_EXPORT_DRAW_MENU);
        if (draw) draw();
        ImGui::End();
        ImGui::Render();
    }
}

void hotplugScenarios(const char* dll, HorizonHostApi* host) {
    SetEnvironmentVariableA("HORIZON_TEST_HOTPLUG", "1");
    SetEnvironmentVariableA("HORIZON_TEST_HOTPLUG_DEVICES", "");
    printf("scenario 1: default on, a pad is plugged in while the game runs\n");
    {
        ScenarioRun r = scenarioStart(dll, host, true);
        check(r.init == 0 && logHas("ready: watching"), "Init ok, watching");
        check(g_toasts.empty() && !logHas("paused"), "no toast at start, not paused");
        SetEnvironmentVariableA("HORIZON_TEST_HOTPLUG_DEVICES", "1:Fake Pad A");
        check(pumpUntil([] { return toastHas("Controller added: Fake Pad A"); }, 3000), "toast: Controller added: Fake Pad A");
        check(g_toasts.size() == 1 && logHas("registered: Fake Pad A"), "one toast, registered once");

        printf("scenario 2: switched off, a second pad comes\n");
        if (r.setEnabled) r.setEnabled(0);
        g_toasts.clear();
        SetEnvironmentVariableA("HORIZON_TEST_HOTPLUG_DEVICES", "1:Fake Pad A;2:Fake Pad B");
        pumpUntil([] { return false; }, 800);
        check(g_toasts.empty() && !logHas("registered: Fake Pad B"), "off: the new pad is not registered");
        if (r.setEnabled) r.setEnabled(1);
        check(pumpUntil([] { return toastHas("Fake Pad B"); }, 3000), "on again: the pad plugged in meanwhile is registered");

        printf("scenario 3: unplug and replug the same pad\n");
        g_toasts.clear();
        SetEnvironmentVariableA("HORIZON_TEST_HOTPLUG_DEVICES", "2:Fake Pad B");
        check(pumpUntil([] { return logHas("unplugged: Fake Pad A"); }, 3000), "unplugged: logged");
        check(g_toasts.empty(), "no toast for unplugging");
        SetEnvironmentVariableA("HORIZON_TEST_HOTPLUG_DEVICES", "1:Fake Pad A;2:Fake Pad B");
        check(pumpUntil([] { return toastHas("Controller back: Fake Pad A"); }, 3000), "plugged in again: toast");
        check(logHas("plugged in again: Fake Pad A") && g_toasts.size() == 1, "same slot (no second registration)");
        int registered = 0;
        for (const auto& l : g_logLines)
            if (l.find("registered: Fake Pad A") != std::string::npos) registered++;
        check(registered == 1, "Fake Pad A registered exactly once over the whole run");
        drawPage(r.mod);
        scenarioStop(r);
    }

    printf("scenario 4: standalone controller_hotplug.dll loaded in the process\n");
    if (g_standaloneDummy.empty()) {
        check(false, "--standalone-dummy <dll> missing");
    } else {
        const std::string dir = g_exeDir + "standalone\\";
        makeDirs(dir);
        const std::string copy = dir + "controller_hotplug.dll";
        check(CopyFileA(g_standaloneDummy.c_str(), copy.c_str(), FALSE) != 0, "dummy controller_hotplug.dll copied");
        HMODULE dummy = LoadLibraryA(copy.c_str());
        check(dummy != nullptr, "dummy loaded as controller_hotplug.dll");
        SetEnvironmentVariableA("HORIZON_TEST_HOTPLUG_DEVICES", "1:Fake Pad A");
        ScenarioRun r = scenarioStart(dll, host, true);
        check(r.init == 0 && logHas("module paused") && !logHas("ready: watching"), "Init ok, passive (no thread)");
        check(pumpUntil([] { return toastHas("paused"); }, 500) && g_toasts.size() == 1 &&
                  g_toasts[0] == "Controller fix paused: standalone active",
              "toast: Controller fix paused: standalone active");
        pumpUntil([] { return false; }, 400);
        check(!toastHas("Controller added"), "passive: no pad registered");
        drawPage(r.mod);
        scenarioStop(r);
        if (dummy) FreeLibrary(dummy);
        DeleteFileA(copy.c_str());
    }

    printf("scenario 5: the standalone claimed controller hot-plug in this process (other DLL name)\n");
    {
        char name[64];
        sprintf_s(name, "Local\\HorizonControllerHotplug.%lu", GetCurrentProcessId());
        HANDLE claim = CreateMutexA(nullptr, FALSE, name);
        ScenarioRun r = scenarioStart(dll, host, true);
        check(r.init == 0 && logHas("module paused"), "claim held by someone else: module paused");
        scenarioStop(r);
        if (claim) CloseHandle(claim);
    }
    SetEnvironmentVariableA("HORIZON_TEST_HOTPLUG", nullptr);
    SetEnvironmentVariableA("HORIZON_TEST_HOTPLUG_DEVICES", nullptr);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); // keep the output when a plugin crashes the process
    // before the plugin loads: its C runtime copies the environment at load time
    SetEnvironmentVariableA("HORIZON_TEST_SAVE_REPLAY", "1"); // the sample calls save_replay once in Init
    const char* dll = nullptr;
    const char* gameName = "eswc";
    int expectInit = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) gameName = argv[++i];
        else if (!strcmp(argv[i], "--expect-init") && i + 1 < argc) expectInit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--standalone-dummy") && i + 1 < argc) g_standaloneDummy = argv[++i];
        else dll = argv[i];
    }
    if (!dll) {
        printf("usage: fake_host <plugin dll> [--game eswc|sunrise|original|tmnf|tmuf] [--expect-init <n>]\n");
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
    } else if (!strcmp(gameName, "original")) {
        // the real TM Original sha: plugins that pin the exe accept it; their code checks then fail against this
        // test program, so they stay off without touching anything
        g_gameInfo.game = HORIZON_GAME_ORIGINAL;
        g_gameInfo.game_name = "TrackMania Original";
        g_gameInfo.exe_version = "Online Patch v2 (community build)";
        g_gameInfo.exe_sha256 = "75e129d769f4595ddde54a8ef484162915bc2bf998c1a2139cc963294fe8175e";
    } else if (!strcmp(gameName, "tmnf")) {
        g_gameInfo.game = HORIZON_GAME_TMNF;
        g_gameInfo.game_name = "TrackMania Nations Forever";
        g_gameInfo.exe_version = "fake";
        g_gameInfo.exe_sha256 = "1111111111111111111111111111111111111111111111111111111111111111";
        g_gameInfo.loader = 2;
    } else if (!strcmp(gameName, "tmuf")) {
        g_gameInfo.game = HORIZON_GAME_TMUF;
        g_gameInfo.game_name = "TrackMania United Forever";
        g_gameInfo.exe_version = "fake";
        g_gameInfo.exe_sha256 = "2222222222222222222222222222222222222222222222222222222222222222";
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
    host.open_page = f_open_page;
    host.notify = f_notify;
    host.ui_metrics = f_ui_metrics;

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

    if (!strcmp(pi->id, "tmx-upload")) tmxUploadTests(mod, init, shutdown, setEnabled, onEvent, &host, gameName);

    printf("Shutdown\n");
    shutdown();
    flushQueue();
    const std::string pluginName = pi->name; // pi points into the DLL
    check(FreeLibrary(mod) != 0, "FreeLibrary");
    if (g_pluginId == "performance" && (g_gameInfo.game == HORIZON_GAME_ESWC || g_gameInfo.game == HORIZON_GAME_SUNRISE))
        perfScenarios(dll, &host, g_gameInfo.game == HORIZON_GAME_ESWC);
    if (g_pluginId == "eswc-autosave" && g_gameInfo.game == HORIZON_GAME_ESWC) {
        check(pluginName == "Autosave & Login-Fix", "display name: Autosave & Login-Fix");
        autosaveScenarios(dll, &host);
    }
    if (g_pluginId == "controller-hotplug" && g_gameInfo.game == HORIZON_GAME_ESWC) {
        check(pluginName == "Controller fix", "display name: Controller fix");
        hotplugScenarios(dll, &host);
    }
    ImGui::DestroyContext();
    printf("fake host: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
