#include "client_host.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>

#include "imgui.h"

namespace chost {

World world;
std::map<std::string, std::string> settings;
std::vector<Panel> panels;
std::vector<Toast> toasts;
std::vector<BridgeCall> bridgeCalls;
std::vector<std::string> playedMaps;
int menuReturns = 0;
std::string logs;
bool online = false;
int gameState = HORIZON_STATE_MAIN_MENU;
int bridgeStateValue = HORIZON_BRIDGE_CONNECTED;
bool menuIsOpen = true;
HorizonStyle style;
std::function<int(const std::string&, const std::string&, std::string*, std::string*)> bridgeHandler;

namespace {

HorizonGameInfo g_info;
std::string g_dataFolder;
std::wstring g_dataFolderW;
std::string g_gameFolder, g_userFolder;
std::string g_pluginId;
std::vector<std::pair<HorizonCallback, void*>> g_queue;
struct Pending {
    HorizonBridgeCallback cb;
    void* user;
    int status;
    std::string type, body;
};
std::deque<Pending> g_bridge;
std::vector<std::pair<int, std::string>> g_hotkeys;
uint32_t g_hostFlags = 0;

// ---------------------------------------------------------------- memory arena for the fake world
uint8_t* g_arena = nullptr;
size_t g_used = 0;
const size_t kArena = 4u << 20;

uintptr_t alloc(size_t n) {
    if (!g_arena) g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_used = (g_used + 15) & ~static_cast<size_t>(15);
    const uintptr_t p = reinterpret_cast<uintptr_t>(g_arena + g_used);
    memset(g_arena + g_used, 0, n);
    g_used += n;
    return p;
}
void put32(uintptr_t a, uint32_t v) { memcpy(reinterpret_cast<void*>(a), &v, 4); }
void putF(uintptr_t a, float v) { memcpy(reinterpret_cast<void*>(a), &v, 4); }
uintptr_t obj(size_t size) {
    const uintptr_t o = alloc(size);
    static uintptr_t vt = 0;
    if (!vt) vt = alloc(64);
    put32(o, static_cast<uint32_t>(vt)); // a readable vtable: "an engine object" when reflection is not there
    return o;
}

// ---------------------------------------------------------------- fake engine reflection
// The car's classes as Forever registers them (TMNF/TMUF, static RE): CSceneVehicleCar 0x0a02b000 -> CSceneVehicle
// 0x0a060000 -> CSceneMobil 0x0a011000, the first two with an EMPTY name. CMwClassInfo: +04 id, +08 parent,
// +14 name, +18 next, +20 params, +24 param count. The car's vtable slot 2 is `mov eax, classinfo; ret`.
uintptr_t g_carVtable = 0, g_carClasses = 0;
uintptr_t fakeClass(uint32_t id, const char* name, uintptr_t parent, uintptr_t next) {
    const uintptr_t c = alloc(0x28);
    const size_t n = strlen(name) + 1;
    const uintptr_t s = alloc(n);
    memcpy(reinterpret_cast<void*>(s), name, n);
    put32(c + 0x04, id);
    put32(c + 0x08, static_cast<uint32_t>(parent));
    put32(c + 0x14, static_cast<uint32_t>(s));
    put32(c + 0x18, static_cast<uint32_t>(next));
    return c;
}
uintptr_t carVtable() {
    if (g_carVtable) return g_carVtable;
    const uintptr_t mobil = fakeClass(0x0a011000u, "CSceneMobil", 0, 0);
    const uintptr_t vehicle = fakeClass(0x0a060000u, "", mobil, mobil);
    g_carClasses = fakeClass(0x0a02b000u, "", vehicle, vehicle);
    const uintptr_t ret = alloc(1);
    *reinterpret_cast<uint8_t*>(ret) = 0xc3;
    const uintptr_t getInfo = alloc(6); // mov eax, imm32; ret
    *reinterpret_cast<uint8_t*>(getInfo) = 0xb8;
    put32(getInfo + 1, static_cast<uint32_t>(g_carClasses));
    *reinterpret_cast<uint8_t*>(getInfo + 5) = 0xc3;
    g_carVtable = alloc(16 * 4);
    for (uintptr_t i = 0; i < 16; i++) put32(g_carVtable + i * 4, static_cast<uint32_t>(i == 2 ? getInfo : ret));
    return g_carVtable;
}

// CMwId::GetString's first bytes and the table address, in this exe's image: the plugins find the id table
// with the same scan as in the games.
unsigned char g_idCode[0x48] = {1};
uintptr_t g_idGlobal = 0, g_idTable = 0, g_idPool = 0, g_idData = 0;
uint32_t g_idCount = 0, g_poolUsed = 0;

void idSetup(bool forever) {
    static const unsigned char head[] = {0x8B, 0x01, 0x8B, 0xC8, 0x81, 0xE1, 0x00, 0x00, 0x00, 0xC0, 0x81, 0xF9, 0x00, 0x00, 0x00, 0x40,
                                         0x74, 0x0B, 0x81, 0xF9, 0x00, 0x00, 0x00, 0x80, 0x74, 0x03, 0x33, 0xC0, 0xC3, 0x25, 0xFF, 0xFF, 0xFF, 0x3F};
    memset(g_idCode, 0x90, sizeof(g_idCode));
    memcpy(g_idCode, head, sizeof(head));
    g_idGlobal = alloc(4);
    g_idTable = alloc(0x40);
    g_idPool = alloc(4096);
    g_idData = alloc(256 * 4);
    g_idCount = 0;
    g_poolUsed = 1;
    put32(g_idGlobal, static_cast<uint32_t>(g_idTable));
    put32(g_idTable + 4, static_cast<uint32_t>(g_idPool));
    size_t i = sizeof(head);
    if (forever) {
        const unsigned char tail[] = {0x0F, 0xB7, 0xC8, 0x56, 0x8B, 0x35};
        memcpy(g_idCode + i, tail, sizeof(tail));
        i += sizeof(tail);
        memcpy(g_idCode + i, &g_idGlobal, 4);
        put32(g_idTable + 0x10, static_cast<uint32_t>(g_idData)); // bucket 0: T + 0*8 + 0x10
    } else {
        const unsigned char tail[] = {0x8B, 0xC8, 0xC1, 0xE9, 0x10, 0x8D, 0x14, 0x49, 0x8B, 0x0D};
        memcpy(g_idCode + i, tail, sizeof(tail));
        i += sizeof(tail);
        memcpy(g_idCode + i, &g_idGlobal, 4);
        i += 4;
        const unsigned char tail2[] = {0x8B, 0x54, 0x91, 0x14};
        memcpy(g_idCode + i, tail2, sizeof(tail2));
        put32(g_idTable + 0x14, static_cast<uint32_t>(g_idData)); // bucket 0: T + 0*12 + 0x14
    }
}
uint32_t idOf(const char* name) {
    const size_t n = strlen(name) + 1;
    memcpy(reinterpret_cast<void*>(g_idPool + g_poolUsed), name, n);
    put32(g_idData + g_idCount * 4, g_poolUsed);
    g_poolUsed += static_cast<uint32_t>(n);
    return 0x40000000u | g_idCount++;
}

void putWStr(uintptr_t a, const wchar_t* s) {
    const size_t n = wcslen(s);
    const uintptr_t p = alloc((n + 1) * 2);
    memcpy(reinterpret_cast<void*>(p), s, (n + 1) * 2);
    put32(a, static_cast<uint32_t>(n));
    put32(a + 4, static_cast<uint32_t>(p));
}

// offsets (the plugins' defaults, see client-common/gameread.cpp)
struct Off {
    int players, target, speed, clock, ghosts, pInfo, pMobil, mScene, state, time, start, now, curCp, cpList, gTime, gCps, steer, gas, brake, gear, rpm,
        uid, name;
};
const Off kForever = {0x24, 0x98, 0x26c, 0x274, 0x520, 0x1c, 0x28, 0x14, 0x314, 0x2b0, 0x2ac, 0x308, 0x2e4, 0x2f8, 0xe8, 0x94,
                      0x58, 0x50, 0x54, 0x5c8, 0x5b8, 0xdc, 0x108};
const Off kLegacy = {0x20, 0x10c, -1, -1, -1, 0x34, 0x24, 0x14, -1, 0x1f4, -1, 0x258, 0x22c, -1, -1, -1, 0xc0, 0xb8, 0xbc, -1, -1, 0xc8, 0x104};
const Off& off() { return world.forever ? kForever : kLegacy; }
bool g_haveRace = false;

// The car under the player as each game keeps it: Forever player.Mobil -> CGameMobil -> SceneMobil (the car);
// ESWC/Sunrise player.Mobil is the car itself (CSceneToyVehicleTrackMania; its +0x14 is another scene object,
// left pointing at an empty one here so a reader that follows it reads zeros, as in the game).
void linkCar() {
    const Off& o = off();
    if (world.forever) {
        put32(world.player + static_cast<uintptr_t>(o.pMobil), static_cast<uint32_t>(world.gameMobil));
        put32(world.gameMobil + static_cast<uintptr_t>(o.mScene), static_cast<uint32_t>(world.vehicle));
    } else {
        put32(world.player + static_cast<uintptr_t>(o.pMobil), static_cast<uint32_t>(world.vehicle));
        put32(world.vehicle + static_cast<uintptr_t>(o.mScene), static_cast<uint32_t>(world.gameMobil));
    }
}

// ---------------------------------------------------------------- API
void HORIZON_CALL f_log(HorizonPluginHandle, int32_t level, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logs += buf;
    logs += '\n';
    if (level >= HORIZON_LOG_WARN) printf("  [plugin %s] %s\n", level >= HORIZON_LOG_ERROR ? "ERROR" : "warn", buf);
}
int32_t HORIZON_CALL f_get_string(HorizonPluginHandle, const char* key, const char* fallback, char* out, uint32_t size) {
    auto it = settings.find(key);
    const std::string v = it != settings.end() ? it->second : (fallback ? fallback : "");
    if (!out || !size) return static_cast<int32_t>(v.size());
    const size_t n = v.size() < size - 1 ? v.size() : size - 1;
    memcpy(out, v.data(), n);
    out[n] = 0;
    return static_cast<int32_t>(n);
}
void HORIZON_CALL f_set_string(HorizonPluginHandle, const char* key, const char* value) { settings[key] = value ? value : ""; }
int32_t HORIZON_CALL f_get_int(HorizonPluginHandle, const char* key, int32_t fallback) {
    auto it = settings.find(key);
    return it != settings.end() ? atoi(it->second.c_str()) : fallback;
}
void HORIZON_CALL f_set_int(HorizonPluginHandle, const char* key, int32_t value) { settings[key] = std::to_string(value); }
float HORIZON_CALL f_get_float(HorizonPluginHandle, const char* key, float fallback) {
    auto it = settings.find(key);
    return it != settings.end() ? static_cast<float>(atof(it->second.c_str())) : fallback;
}
void HORIZON_CALL f_set_float(HorizonPluginHandle, const char* key, float value) { settings[key] = std::to_string(value); }
const char* HORIZON_CALL f_data_folder(HorizonPluginHandle) { return g_dataFolder.c_str(); }
const wchar_t* HORIZON_CALL f_data_folder_w(HorizonPluginHandle) { return g_dataFolderW.c_str(); }
void* HORIZON_CALL f_imgui_context(void) { return ImGui::GetCurrentContext(); }
int32_t HORIZON_CALL f_imgui_version(void) { return IMGUI_VERSION_NUM; }
void* hostAlloc(size_t n, void*) { return malloc(n); }
void hostFree(void* p, void*) { free(p); }
void HORIZON_CALL f_imgui_allocators(void** a, void** f, void** u) {
    *a = reinterpret_cast<void*>(&hostAlloc);
    *f = reinterpret_cast<void*>(&hostFree);
    *u = nullptr;
}
const HorizonStyle* HORIZON_CALL f_style(void) { return &style; }
const HorizonGameInfo* HORIZON_CALL f_game_info(void) { return &g_info; }
void* HORIZON_CALL f_engine_pointer(uint32_t which) {
    switch (which) {
    case HORIZON_ENGINE_APP: return reinterpret_cast<void*>(world.app);
    case HORIZON_ENGINE_RACE: return g_haveRace ? reinterpret_cast<void*>(world.race) : nullptr;
    case HORIZON_ENGINE_CHALLENGE: return reinterpret_cast<void*>(world.challenge);
    default: return nullptr;
    }
}
int32_t HORIZON_CALL f_register_hotkey(HorizonPluginHandle, const char* action, const char*, const char* key, HorizonCallback, void*) {
    g_hotkeys.push_back({static_cast<int>(g_hotkeys.size() + 1), std::string(action) + "=" + (key ? key : "")});
    return static_cast<int32_t>(g_hotkeys.size());
}
const char* HORIZON_CALL f_hotkey_text(HorizonPluginHandle, int32_t) { return ""; }
void HORIZON_CALL f_subscribe(HorizonPluginHandle, uint32_t) {}
void HORIZON_CALL f_post_replay_saved(HorizonPluginHandle, const char*) {}
HorizonResult HORIZON_CALL f_play_map_file(HorizonPluginHandle, const char* path, uint32_t) {
    if (online) return HORIZON_ERR_BLOCKED_ONLINE;
    if (gameState != HORIZON_STATE_MAIN_MENU) return HORIZON_ERR_WRONG_STATE;
    playedMaps.push_back(path ? path : "");
    return HORIZON_OK;
}
HorizonResult HORIZON_CALL f_return_to_menu(HorizonPluginHandle) {
    if (online) return HORIZON_ERR_BLOCKED_ONLINE;
    menuReturns++;
    return HORIZON_OK;
}
int32_t HORIZON_CALL f_get_game_state(void) { return gameState; }
int32_t HORIZON_CALL f_is_online(void) { return online ? 1 : 0; }
int32_t HORIZON_CALL f_actions_allowed(HorizonPluginHandle) { return online ? 0 : 1; }
void HORIZON_CALL f_run_on_game_thread(HorizonPluginHandle, HorizonCallback cb, void* user) { g_queue.push_back({cb, user}); }
void* HORIZON_CALL f_device(void) { return nullptr; }
void HORIZON_CALL f_set_device_callbacks(HorizonPluginHandle, const HorizonDeviceCallbacks*) {}
int32_t HORIZON_CALL f_menu_open(void) { return menuIsOpen ? 1 : 0; }
void HORIZON_CALL f_restart_note(HorizonPluginHandle, const char*) {}
int32_t HORIZON_CALL f_reflect_offset(const char*, const char*) { return -1; } // no reflection: the plugins use their defaults
void* HORIZON_CALL f_reflect_method(const char*, const char*) { return nullptr; }
uint64_t g_nextTex = 0x1000;
uint64_t HORIZON_CALL f_create_texture(HorizonPluginHandle, uint32_t w, uint32_t h, const void* rgba);
void HORIZON_CALL f_release_texture(HorizonPluginHandle, uint64_t tex);
HorizonResult HORIZON_CALL f_save_replay(HorizonPluginHandle, const char*) { return HORIZON_ERR_NOT_SUPPORTED; }
HorizonResult HORIZON_CALL f_register_panel(HorizonPluginHandle, const HorizonPanelDef* d) {
    if (!d || !d->key || !d->title || !d->draw) return HORIZON_ERR_INVALID_ARG;
    for (Panel& p : panels)
        if (p.key == d->key) {
            p = Panel{d->key, d->title, d->flags, d->draw, d->user};
            return HORIZON_OK;
        }
    panels.push_back(Panel{d->key, d->title, d->flags, d->draw, d->user});
    return HORIZON_OK;
}
void HORIZON_CALL f_unregister_panel(HorizonPluginHandle, const char* key) {
    for (size_t i = 0; i < panels.size(); i++)
        if (panels[i].key == key) {
            panels.erase(panels.begin() + static_cast<long>(i));
            return;
        }
}
int32_t HORIZON_CALL f_panel_visible(HorizonPluginHandle, const char*) { return 1; }
void HORIZON_CALL f_set_host_flag(HorizonPluginHandle, uint32_t flag, int32_t on) { g_hostFlags = on ? (g_hostFlags | flag) : (g_hostFlags & ~flag); }
uint32_t HORIZON_CALL f_host_flags(void) { return g_hostFlags; }
void HORIZON_CALL f_notify(HorizonPluginHandle, const HorizonToast* t) {
    if (!t || !t->title) return;
    toasts.push_back(Toast{t->title, t->text ? t->text : "", t->kind});
}
HorizonResult HORIZON_CALL f_bridge_request(HorizonPluginHandle, const char* method, const char* path, const void* body, uint32_t len,
                                            HorizonBridgeCallback cb, void* user) {
    if (!method || !path || !cb || strncmp(path, "/v1/", 4) != 0) return HORIZON_ERR_INVALID_ARG;
    (void)body;
    (void)len;
    bridgeCalls.push_back(BridgeCall{method, path});
    Pending p{cb, user, HORIZON_BRIDGE_ERR_NOT_CONNECTED, "", ""};
    if (bridgeStateValue == HORIZON_BRIDGE_CONNECTED)
        p.status = bridgeHandler ? bridgeHandler(method, path, &p.type, &p.body) : fixtureAnswer(method, path, &p.type, &p.body);
    g_bridge.push_back(std::move(p));
    return HORIZON_OK;
}
int32_t HORIZON_CALL f_bridge_state(void) { return bridgeStateValue; }
} // namespace
HorizonUiMetrics metrics = {sizeof(HorizonUiMetrics), 1.0f, 1.0f, 1920.0f, 1080.0f, 1536.0f, 864.0f, 1100.0f, 700.0f};
namespace {
// ABI 1.4: chost::metrics (the previews set page_w/page_h to the window the plugin draws in)
const HorizonUiMetrics* HORIZON_CALL f_ui_metrics(void) { return &metrics; }

uint64_t HORIZON_CALL f_create_texture(HorizonPluginHandle, uint32_t w, uint32_t h, const void* rgba) {
    if (!w || !h || !rgba) return 0;
    return g_nextTex++;
}
void HORIZON_CALL f_release_texture(HorizonPluginHandle, uint64_t) {}

// ---------------------------------------------------------------- bridge fixtures
const char* kNames[] = {"Paradise Lost",  "Speedway Echo", "Tech Garden",  "Moonlit Dirt", "A01-Race Remake", "Coastline Drift",
                        "Lunatic Ladder", "Short & Sweet", "Glass Highway", "Night Shift",  "Bumper Cars",     "Long Haul"};
const char* kAuthors[] = {"Nadeo", "cheatoskar", "riolu", "Wirtual", "Hefest", "Spam", "eyebo", "Kappa"};
const char* kTags[] = {"Race", "Tech", "SpeedTech", "FullSpeed", "LOL", "Offroad", "Maze", "RPG"};
const char* kDiff[] = {"Beginner", "Intermediate", "Expert", "Lunatic"};

std::string trackJson(int i) {
    char b[512];
    snprintf(b, sizeof(b),
             "{\"id\":%d,\"name\":\"%s\",\"author\":\"%s\",\"awards\":%d,\"tags\":[\"%s\"],\"difficulty\":\"%s\",\"authorTime\":%d,"
             "\"environment\":\"Stadium\",\"uploaded\":\"2025-0%d-1%dT12:00:00Z\",\"thumbnail\":true,\"uid\":\"FakeUid%020d\"}",
             1000 + i, kNames[i % 12], kAuthors[i % 8], (i * 37) % 120, kTags[i % 8], kDiff[i % 4], 25000 + i * 3170, 1 + i % 9, i % 10,
             1000 + i);
    return b;
}

// A 24-bit BMP thumbnail (stb_image reads it like the real JPEGs): sky, grass and a road in the track's colour.
std::string thumbBmp(int id) {
    const int w = 192, h = 108;
    const int row = (w * 3 + 3) & ~3;
    std::string f(54 + static_cast<size_t>(row) * h, '\0');
    auto p16 = [&](size_t at, uint16_t v) { memcpy(&f[at], &v, 2); };
    auto p32 = [&](size_t at, uint32_t v) { memcpy(&f[at], &v, 4); };
    f[0] = 'B';
    f[1] = 'M';
    p32(2, static_cast<uint32_t>(f.size()));
    p32(10, 54);
    p32(14, 40);
    p32(18, static_cast<uint32_t>(w));
    p32(22, static_cast<uint32_t>(h));
    p16(26, 1);
    p16(28, 24);
    const float hue = static_cast<float>((id * 47) % 360);
    for (int y = 0; y < h; y++) {
        const int yy = h - 1 - y; // bottom-up
        for (int x = 0; x < w; x++) {
            float r, g, b;
            const float t = static_cast<float>(yy) / h;
            if (yy < h * 0.45f) {
                r = 60 + 110 * t; g = 110 + 100 * t; b = 200 + 40 * t; // sky
            } else {
                r = 50; g = 90 - 30 * t; b = 40; // grass
                const float c = w * 0.5f + (x - w * 0.5f) * 0.0f;
                const float half = 8 + (yy - h * 0.45f) * 1.4f;
                const float bend = sinf((yy + id * 13) * 0.05f) * 18.0f;
                if (fabsf(x - (c + bend)) < half) {
                    r = 70 + 0.4f * hue; g = 70; b = 80 + 0.2f * (360 - hue); // road
                }
            }
            char* px = &f[54 + static_cast<size_t>(y) * row + static_cast<size_t>(x) * 3];
            px[0] = static_cast<char>(b > 255 ? 255 : b);
            px[1] = static_cast<char>(g > 255 ? 255 : g);
            px[2] = static_cast<char>(r > 255 ? 255 : r);
        }
    }
    return f;
}

std::string query(const std::string& path, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t at = path.find('?');
    while (at != std::string::npos) {
        if (path.compare(at + 1, k.size(), k) == 0) {
            const size_t s = at + 1 + k.size(), e = path.find('&', s);
            return path.substr(s, e == std::string::npos ? std::string::npos : e - s);
        }
        at = path.find('&', at + 1);
    }
    return std::string();
}

} // namespace

std::string makeReplay(const std::string& uid, int raceTime, const std::vector<int>& cps, const std::string& driver) {
    std::string h; // header chunk 0x03093000
    auto u32 = [](std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); };
    auto str = [&](std::string& s, const std::string& v) {
        u32(s, static_cast<uint32_t>(v.size()));
        s += v;
    };
    u32(h, 2);          // version
    u32(h, 3);          // lookback version
    u32(h, 0x40000000); // new string
    str(h, uid);
    u32(h, 0x40000000);
    str(h, "Stadium");
    u32(h, 0x40000000);
    str(h, "nadeo");
    u32(h, static_cast<uint32_t>(raceTime));
    str(h, driver);
    std::string user;
    u32(user, 1);
    u32(user, 0x03093000);
    u32(user, static_cast<uint32_t>(h.size()));
    user += h;
    std::string body;
    u32(body, 0x03092005);
    u32(body, 0x534B4950);
    u32(body, 4);
    u32(body, static_cast<uint32_t>(raceTime));
    u32(body, 0x0309200B);
    u32(body, 0x534B4950);
    u32(body, static_cast<uint32_t>(4 + cps.size() * 8));
    u32(body, static_cast<uint32_t>(cps.size()));
    for (int c : cps) {
        u32(body, static_cast<uint32_t>(c));
        u32(body, 0);
    }
    u32(body, 0xFACADE01);
    std::string f = "GBX";
    f += '\x06';
    f += '\x00';
    f += "BUUR";
    u32(f, 0x03093000);
    u32(f, static_cast<uint32_t>(user.size()));
    f += user;
    u32(f, 3); // nodes
    u32(f, 0); // no external refs
    f += body;
    return f;
}

int fixtureAnswer(const std::string& method, const std::string& path, std::string* type, std::string* body) {
    (void)method;
    *type = "application/json";
    if (path.compare(0, 15, "/v1/tmx/search?") == 0) {
        const int page = atoi(query(path, "page").c_str());
        const std::string name = query(path, "name");
        std::string r = "{\"ok\":true,\"site\":\"tmnf.exchange\",\"page\":" + std::to_string(page > 0 ? page : 1) + ",\"more\":" +
                        (page <= 1 ? "true" : "false") + ",\"results\":[";
        int n = 0;
        for (int i = 0; i < 12; i++) {
            if (!name.empty() && std::string(kNames[i % 12]).find(name) == std::string::npos && name != "a") continue;
            if (n++) r += ",";
            r += trackJson(i + (page > 1 ? 12 : 0));
        }
        *body = r + "]}";
        return 200;
    }
    if (path.compare(0, 15, "/v1/tmx/random?") == 0) {
        *body = "{\"ok\":true,\"result\":" + trackJson(7) + "}";
        return 200;
    }
    if (path.compare(0, 14, "/v1/tmx/thumb?") == 0) {
        *type = "image/bmp";
        *body = thumbBmp(atoi(query(path, "id").c_str()));
        return 200;
    }
    if (path.compare(0, 12, "/v1/tmx/map?") == 0) {
        *type = "application/octet-stream";
        *body = "GBX\x06\x00" "BUCR fake challenge " + query(path, "id");
        return 200;
    }
    if (path.compare(0, 15, "/v1/tmx/record?") == 0) {
        const std::string uid = query(path, "uid");
        if (uid.empty()) {
            *body = "{\"ok\":false,\"error\":\"uid missing\"}";
            return 400;
        }
        *body = "{\"ok\":true,\"found\":true,\"site\":\"tmnf.exchange\",\"trackId\":2233,\"replayId\":77,\"time\":24420,\"player\":\"WR Holder\"}";
        return 200;
    }
    if (path.compare(0, 15, "/v1/tmx/replay?") == 0) {
        *type = "application/octet-stream";
        *body = makeReplay("UidOfTheTestMap000000000aB", 24420, {5110, 9870, 15030, 19880, 24420}, "WR Holder");
        return 200;
    }
    *body = "{\"ok\":false,\"error\":\"not found\"}";
    return 404;
}

const char* gameName(uint32_t game) {
    switch (game) {
    case HORIZON_GAME_ESWC: return "TrackMania Nations ESWC";
    case HORIZON_GAME_SUNRISE: return "TrackMania Sunrise";
    case HORIZON_GAME_ORIGINAL: return "TrackMania Original";
    case HORIZON_GAME_TMNF: return "TrackMania Nations Forever";
    case HORIZON_GAME_TMUF: return "TrackMania United Forever";
    default: return "?";
    }
}

void setup(uint32_t game, const std::string& dataRoot, const std::string& pluginId) {
    g_pluginId = pluginId;
    g_dataFolder = dataRoot + pluginId + "\\";
    CreateDirectoryA(dataRoot.c_str(), nullptr);
    CreateDirectoryA(g_dataFolder.c_str(), nullptr);
    g_dataFolderW.assign(g_dataFolder.begin(), g_dataFolder.end());
    g_gameFolder = dataRoot + "game\\";
    g_userFolder = dataRoot + "user\\";
    CreateDirectoryA(g_gameFolder.c_str(), nullptr);
    CreateDirectoryA(g_userFolder.c_str(), nullptr);
    g_info = HorizonGameInfo{};
    g_info.size = sizeof(g_info);
    g_info.game = game;
    g_info.game_name = gameName(game);
    g_info.exe_version = "test";
    g_info.exe_sha256 = "";
    g_info.exe_timestamp = 0;
    g_info.exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_info.language = "en";
    g_info.game_folder = g_gameFolder.c_str();
    g_info.user_folder = g_userFolder.c_str();
    g_info.host_version = "0.3.0";
    g_info.loader = 1;
    style = HorizonStyle{};
    style.size = sizeof(style);
    style.accent = {1.0f, 0.42f, 0.10f, 1.0f};
    style.accent2 = {1.0f, 0.54f, 0.27f, 1.0f};
    style.accent3 = {1.0f, 0.69f, 0.37f, 1.0f};
    style.accent_ink = {0.10f, 0.04f, 0.01f, 1.0f};
    style.ok = {0.24f, 0.86f, 0.52f, 1.0f};
    style.warn = {0.95f, 0.76f, 0.31f, 1.0f};
    style.bad = {1.0f, 0.36f, 0.36f, 1.0f};
    style.bg = {0.039f, 0.047f, 0.067f, 0.97f};
    style.bg2 = {0.063f, 0.075f, 0.102f, 1.0f};
    style.surface = {0.106f, 0.118f, 0.141f, 1.0f};
    style.text = {0.933f, 0.941f, 0.953f, 1.0f};
    style.text2 = {0.765f, 0.788f, 0.820f, 1.0f};
    style.muted = {0.557f, 0.592f, 0.639f, 1.0f};
    style.radius_sm = 10;
    style.radius = 14;
    style.radius_lg = 20;
    style.scale = 1.0f;
    settings.clear();
    panels.clear();
    toasts.clear();
    bridgeCalls.clear();
    playedMaps.clear();
    logs.clear();
    g_queue.clear();
    g_bridge.clear();
    online = false;
    gameState = HORIZON_STATE_MAIN_MENU;
    bridgeStateValue = HORIZON_BRIDGE_CONNECTED;
    bridgeHandler = nullptr;
}

HorizonHostApi api() {
    HorizonHostApi a = {};
    a.abi = HORIZON_ABI;
    a.size = sizeof(a);
    a.abi_minor = HORIZON_ABI_MINOR;
    a.plugin = reinterpret_cast<HorizonPluginHandle>(&settings);
    a.log = &f_log;
    a.get_string = &f_get_string;
    a.set_string = &f_set_string;
    a.get_int = &f_get_int;
    a.set_int = &f_set_int;
    a.get_float = &f_get_float;
    a.set_float = &f_set_float;
    a.data_folder = &f_data_folder;
    a.data_folder_w = &f_data_folder_w;
    a.imgui_context = &f_imgui_context;
    a.imgui_version = &f_imgui_version;
    a.imgui_allocators = &f_imgui_allocators;
    a.style = &f_style;
    a.game_info = &f_game_info;
    a.engine_pointer = &f_engine_pointer;
    a.register_hotkey = &f_register_hotkey;
    a.hotkey_text = &f_hotkey_text;
    a.subscribe = &f_subscribe;
    a.post_replay_saved = &f_post_replay_saved;
    a.play_map_file = &f_play_map_file;
    a.return_to_menu = &f_return_to_menu;
    a.get_game_state = &f_get_game_state;
    a.is_online = &f_is_online;
    a.actions_allowed = &f_actions_allowed;
    a.run_on_game_thread = &f_run_on_game_thread;
    a.device = &f_device;
    a.set_device_callbacks = &f_set_device_callbacks;
    a.menu_open = &f_menu_open;
    a.request_restart_note = &f_restart_note;
    a.reflect_offset = &f_reflect_offset;
    a.reflect_method = &f_reflect_method;
    a.create_texture_rgba = &f_create_texture;
    a.release_texture = &f_release_texture;
    a.save_replay = &f_save_replay;
    a.register_panel = &f_register_panel;
    a.unregister_panel = &f_unregister_panel;
    a.panel_visible = &f_panel_visible;
    a.set_host_flag = &f_set_host_flag;
    a.host_flags = &f_host_flags;
    a.notify = &f_notify;
    a.bridge_request = &f_bridge_request;
    a.bridge_state = &f_bridge_state;
    a.ui_metrics = &f_ui_metrics;
    return a;
}

void endFrame() {
    auto q = g_queue;
    g_queue.clear();
    for (auto& c : q) c.first(c.second);
    std::deque<Pending> b;
    b.swap(g_bridge);
    for (Pending& p : b) p.cb(p.user, p.status, p.type.c_str(), p.body.empty() ? nullptr : p.body.data(), static_cast<uint32_t>(p.body.size()));
}

void buildWorld(bool forever, const char* uid, const wchar_t* mapName) {
    world = World();
    world.forever = forever;
    const Off& o = off();
    idSetup(forever);
    world.app = obj(0x500);
    world.race = obj(0x600);
    world.challenge = obj(0x200);
    world.player = obj(0x80);
    world.info = obj(0x400);
    world.gameMobil = obj(0x40);
    world.vehicle = obj(0x800);
    if (forever) put32(world.vehicle, static_cast<uint32_t>(carVtable())); // CSceneVehicleCar, for setReflection
    world.ghost = obj(0x120);
    world.cpItems = alloc(8 * 64);
    const uintptr_t players = alloc(16);
    put32(players, static_cast<uint32_t>(world.player));
    put32(world.race + static_cast<uintptr_t>(o.players), 1);
    put32(world.race + static_cast<uintptr_t>(o.players) + 4, static_cast<uint32_t>(players));
    put32(world.race + static_cast<uintptr_t>(o.target), static_cast<uint32_t>(world.info));
    put32(world.player + static_cast<uintptr_t>(o.pInfo), static_cast<uint32_t>(world.info));
    linkCar();
    if (o.cpList >= 0) put32(world.info + static_cast<uintptr_t>(o.cpList) + 4, static_cast<uint32_t>(world.cpItems));
    if (o.ghosts >= 0) {
        const uintptr_t ghosts = alloc(8);
        put32(ghosts, static_cast<uint32_t>(world.ghost));
        put32(world.race + static_cast<uintptr_t>(o.ghosts), 1);
        put32(world.race + static_cast<uintptr_t>(o.ghosts) + 4, static_cast<uint32_t>(ghosts));
        put32(world.ghost + static_cast<uintptr_t>(o.gTime), 0xffffffffu);
    }
    // the map: UID, environment, author as ids in a row; its name
    put32(world.challenge + static_cast<uintptr_t>(o.uid), idOf(uid));
    put32(world.challenge + static_cast<uintptr_t>(o.uid) + 4, idOf("Stadium"));
    put32(world.challenge + static_cast<uintptr_t>(o.uid) + 8, idOf("cheatoskar"));
    putWStr(world.challenge + static_cast<uintptr_t>(o.name), mapName);
    g_haveRace = true;
    setRun(0, 0, {});
}

void setRun(int state, int timeMs, const std::vector<int>& cps) {
    const Off& o = off();
    g_haveRace = true;
    if (o.state >= 0) put32(world.info + static_cast<uintptr_t>(o.state), static_cast<uint32_t>(state));
    const uint32_t start = 500000;
    if (o.start >= 0) put32(world.info + static_cast<uintptr_t>(o.start), start);
    if (o.now >= 0) put32(world.info + static_cast<uintptr_t>(o.now), start + static_cast<uint32_t>(timeMs > 0 ? timeMs : 0));
    if (o.clock >= 0) put32(world.race + static_cast<uintptr_t>(o.clock), start + static_cast<uint32_t>(timeMs > 0 ? timeMs : 0));
    put32(world.info + static_cast<uintptr_t>(o.time), state == 2 ? static_cast<uint32_t>(timeMs) : 0u);
    put32(world.info + static_cast<uintptr_t>(o.curCp), static_cast<uint32_t>(cps.size()));
    if (o.cpList >= 0) {
        put32(world.info + static_cast<uintptr_t>(o.cpList), static_cast<uint32_t>(cps.size()));
        for (size_t i = 0; i < cps.size() && i < 64; i++) {
            put32(world.cpItems + i * 8, static_cast<uint32_t>(cps[i]));
            put32(world.cpItems + i * 8 + 4, 0);
        }
    }
}

void setCar(float steer, float gas, float brake, int speedKmh, int gear, float rpm) {
    const Off& o = off();
    putF(world.vehicle + static_cast<uintptr_t>(o.steer), steer);
    putF(world.vehicle + static_cast<uintptr_t>(o.gas), gas);
    putF(world.vehicle + static_cast<uintptr_t>(o.brake), brake);
    if (o.speed >= 0) put32(world.race + static_cast<uintptr_t>(o.speed), static_cast<uint32_t>(speedKmh));
    if (o.gear >= 0) put32(world.vehicle + static_cast<uintptr_t>(o.gear), static_cast<uint32_t>(gear));
    if (o.rpm >= 0) putF(world.vehicle + static_cast<uintptr_t>(o.rpm), rpm);
}

void setGhost(int raceTime, const std::vector<int>& cps) {
    const Off& o = off();
    if (o.ghosts < 0) return;
    put32(world.ghost + static_cast<uintptr_t>(o.gTime), static_cast<uint32_t>(raceTime));
    const uintptr_t items = alloc(8 * (cps.size() + 1));
    for (size_t i = 0; i < cps.size(); i++) put32(items + i * 8, static_cast<uint32_t>(cps[i]));
    put32(world.ghost + static_cast<uintptr_t>(o.gCps), static_cast<uint32_t>(cps.size()));
    put32(world.ghost + static_cast<uintptr_t>(o.gCps) + 4, static_cast<uint32_t>(items));
}

void clearRace() { g_haveRace = false; }

void setTargetInfo(bool on) {
    put32(world.race + static_cast<uintptr_t>(off().target), on ? static_cast<uint32_t>(world.info) : 0u);
}

uintptr_t moveCarHigh() {
    void* p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!p) return 0;
    memcpy(p, reinterpret_cast<const void*>(world.vehicle), 0x800);
    world.vehicle = reinterpret_cast<uintptr_t>(p);
    linkCar();
    return world.vehicle;
}

// The registry head the plugins' reflect module finds for an unknown exe: the registration code's pattern
// `mov ecx, [head]; mov [eax+18h], ecx; mov [head], eax` in an executable section of this exe.
uintptr_t g_reflectHead = 0;
#pragma pack(push, 1)
struct RegistryCode {
    uint8_t movEcx[2];
    uintptr_t* head1;
    uint8_t movNext[3];
    uint8_t movHead;
    uintptr_t* head2;
    uint8_t pad[32];
};
#pragma pack(pop)
#pragma section(".hzreg", read, execute)
extern const RegistryCode kRegistryCode;
__declspec(allocate(".hzreg")) const RegistryCode kRegistryCode = {{0x8b, 0x0d}, &g_reflectHead, {0x89, 0x48, 0x18}, 0xa3, &g_reflectHead, {0xc3}};

void setReflection(bool on) {
    volatile uint8_t keep = kRegistryCode.movHead; // keeps the pattern in the image
    (void)keep;
    carVtable();
    g_reflectHead = on ? g_carClasses : 0;
}

HorizonMapInfo mapInfo(const char* name, const char* uid, int authorTime) {
    HorizonMapInfo m = {};
    m.size = sizeof(m);
    m.path = "";
    m.uid = uid;
    m.name = name;
    m.author = "";
    m.environment = "Stadium";
    m.author_time = authorTime;
    m.gold_time = authorTime + 3000;
    m.silver_time = authorTime + 8000;
    m.bronze_time = authorTime + 15000;
    m.play_mode = HORIZON_MODE_SOLO;
    m.author_score = -1;
    return m;
}

HorizonEvent event(uint32_t type) {
    HorizonEvent e = {};
    e.size = sizeof(e);
    e.type = type;
    e.play_mode = HORIZON_MODE_SOLO;
    e.time_ms = -1;
    e.checkpoint_index = -1;
    e.respawns = -1;
    e.is_online = online ? 1u : 0u;
    e.engine_race = reinterpret_cast<void*>(world.race);
    e.engine_challenge = reinterpret_cast<void*>(world.challenge);
    return e;
}

} // namespace chost
