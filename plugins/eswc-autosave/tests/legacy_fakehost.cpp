// eswc-autosave.dll (Autosave & Login-Fix) in a fake host, on Sunrise and Original: a fake game image at the
// exe's link addresses with the game functions the save calls as stubs, a fake app/replay/map, and the host
// events the real host sends on these games (FINISH from OnFinishLine, RACE_END with end_screen 1 and no
// replay from the solo race end). Checks: identity (hash + timestamp), save only after a local solo finish
// that is a new best, the game's file name in My Replays, no overwrite, never online/LAN, the save arguments,
// the replay object and the written file left as the game made them, post_replay_saved, hotkey, Mode=all.
//   legacy_fakehost.exe <eswc-autosave.dll> [sunrise|original ...]
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "horizon_plugin.h"

namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const std::string& what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (ok) g_pass++;
    else g_fail++;
}

// ---------------------------------------------------------------- the fake game
struct Game {
    uint32_t bit;
    const char* key;
    const char* sha256;
    uint32_t timestamp;
    uintptr_t strSet, strEmpty, filter, exists, bestTime, bestScore, ctor, save, dtor, unbind, scan, emptyStr, mainEngine;
};
const Game kGames[] = {
    {HORIZON_GAME_SUNRISE, "sunrise", "e59dfc7eaa5314e85b2b0df6f40ef7ba01a23bd5a9525f84407f5dff301df4fe", 0x5cc8f0cf,
     0x0041a2a0, 0x00401280, 0x00480ae0, 0x0044e860, 0x007abd30, 0x007abe10, 0x0041e220, 0x00421e60, 0x0041e330,
     0x00414e00, 0x00821f30, 0x00abc0e8, 0x00b8b494},
    {HORIZON_GAME_ORIGINAL, "original", "75e129d769f4595ddde54a8ef484162915bc2bf998c1a2139cc963294fe8175e", 0x5d337477,
     0x004055b0, 0x00401280, 0x00480c10, 0x0044ea70, 0x007aaab0, 0x007aab90, 0x0041e2b0, 0x00421ee0, 0x0041e3b0,
     0x00414e60, 0x00820410, 0x00ab80e8, 0x00b866f4},
};
// members (Sunrise engine)
constexpr size_t kRaceApp = 0x3c, kAppReplay = 0x130, kAppMyReplays = 0x1d8, kReplayChallenge = 0x58, kReplayRecording = 0xac,
                 kChallengePlayMode = 0x11c, kChallengeName = 0x128, kFidsGetFullPath = 0xac;

constexpr size_t kImage = 0x900000;
unsigned char* g_img = nullptr;
uintptr_t at(uintptr_t link) { return reinterpret_cast<uintptr_t>(g_img) - 0x400000 + link; }

struct FastStringInt { uint32_t len; wchar_t* ptr; };
struct StringParamInt { const wchar_t* ptr; uint32_t len; uint32_t pad; };
wchar_t kEmpty[1] = {0};
std::wstring g_dir;
alignas(16) unsigned char app[0x400], race[0x100], otherRace[0x100], otherApp[0x400], replay[0x200], challenge[0x200];
alignas(16) unsigned char engineMain[0x40], engineSub[0x40], systemEngine[0x40], folder[0x20];
void* folderVt[64];
wchar_t mapName[64] = L"$s$08fTest Map";
int g_best = -1, g_stunt = 0, g_live = 0, g_saves = 0, g_scan = 0, g_unbind = 0, g_args[4] = {};
void* g_saveNod = nullptr;
std::string g_content;

void freeStr(FastStringInt* s) {
    if (s->ptr && s->ptr != kEmpty) {
        free(s->ptr);
        g_live--;
    }
    s->ptr = kEmpty;
    s->len = 0;
}
void __fastcall strSet(FastStringInt* self, void*, const StringParamInt* p) {
    wchar_t* n = static_cast<wchar_t*>(malloc((p->len + 1) * sizeof(wchar_t)));
    g_live++;
    memcpy(n, p->ptr, p->len * sizeof(wchar_t));
    n[p->len] = 0;
    freeStr(self);
    self->ptr = n;
    self->len = p->len;
}
void __fastcall strEmpty(FastStringInt* self, void*) { freeStr(self); }
void __cdecl filter(FastStringInt* dst, const FastStringInt* src, int, int) {
    std::wstring s(src->ptr, src->len);
    for (auto& c : s)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    StringParamInt p = {s.c_str(), static_cast<uint32_t>(s.size()), 0};
    strSet(dst, nullptr, &p);
}
int __cdecl exists(const FastStringInt* name, void* f) {
    if (f != folder) return 1;
    return GetFileAttributesW((g_dir + std::wstring(name->ptr, name->len)).c_str()) != INVALID_FILE_ATTRIBUTES;
}
void __fastcall bestTime(void*, void*, int* t, int* o) { *t = g_best, *o = 0; }
int __fastcall bestScore(void*, void*) { return g_stunt; }
void __fastcall nop(void*, void*) {}
int __fastcall archSave(void*, void*, FastStringInt* name, void* nod, void* f, int a, int b, int c, int d) {
    g_saves++;
    g_args[0] = a, g_args[1] = b, g_args[2] = c, g_args[3] = d;
    g_saveNod = nod;
    if (f != folder) return 0;
    char xml[200];
    _snprintf_s(xml, _TRUNCATE, "<header type=\"replay\" version=\"TMr.4\" exever=\"0.1.5.0\"><times best=\"%d\" validable=\"1\"/></header>", g_best);
    g_content.assign("GBX\x06\x00", 5);
    g_content += xml;
    HANDLE h = CreateFileW((g_dir + std::wstring(name->ptr, name->len)).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    WriteFile(h, g_content.data(), static_cast<DWORD>(g_content.size()), &w, nullptr);
    CloseHandle(h);
    return 1;
}
void __fastcall unbind(void* s, void*, void*) {
    if (s == systemEngine) g_unbind++;
}
void __fastcall scan(void* a, void*) {
    if (a == app) g_scan++;
}
void __fastcall getFullPath(void*, void*, FastStringInt* out) {
    StringParamInt p = {g_dir.c_str(), static_cast<uint32_t>(g_dir.size()), 0};
    strSet(out, nullptr, &p);
}
void jmpTo(uintptr_t from, const void* to) {
    unsigned char* p = reinterpret_cast<unsigned char*>(from);
    p[0] = 0xE9;
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (from + 5));
}

void buildGame(const Game& g) {
    if (!g_img) g_img = static_cast<unsigned char*>(VirtualAlloc(nullptr, kImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    memset(g_img, 0xCC, kImage);
    jmpTo(at(g.strSet), reinterpret_cast<void*>(&strSet));
    jmpTo(at(g.strEmpty), reinterpret_cast<void*>(&strEmpty));
    jmpTo(at(g.filter), reinterpret_cast<void*>(&filter));
    jmpTo(at(g.exists), reinterpret_cast<void*>(&exists));
    jmpTo(at(g.bestTime), reinterpret_cast<void*>(&bestTime));
    jmpTo(at(g.bestScore), reinterpret_cast<void*>(&bestScore));
    jmpTo(at(g.ctor), reinterpret_cast<void*>(&nop));
    jmpTo(at(g.save), reinterpret_cast<void*>(&archSave));
    jmpTo(at(g.dtor), reinterpret_cast<void*>(&nop));
    jmpTo(at(g.unbind), reinterpret_cast<void*>(&unbind));
    jmpTo(at(g.scan), reinterpret_cast<void*>(&scan));
    *reinterpret_cast<wchar_t**>(at(g.emptyStr)) = kEmpty;
    *reinterpret_cast<void**>(at(g.mainEngine)) = engineMain;
    *reinterpret_cast<void**>(engineMain + 0x28) = engineSub;
    *reinterpret_cast<void**>(engineSub + 0x2c) = systemEngine;
    memset(app, 0, sizeof(app));
    memset(replay, 0, sizeof(replay));
    memset(challenge, 0, sizeof(challenge));
    folderVt[kFidsGetFullPath / 4] = reinterpret_cast<void*>(&getFullPath);
    *reinterpret_cast<void**>(folder) = folderVt;
    *reinterpret_cast<void**>(race + kRaceApp) = app;
    *reinterpret_cast<void**>(otherRace + kRaceApp) = otherApp;
    *reinterpret_cast<void**>(app + kAppReplay) = replay;
    *reinterpret_cast<void**>(app + kAppMyReplays) = folder;
    *reinterpret_cast<void**>(replay + kReplayChallenge) = challenge;
    *reinterpret_cast<uint32_t*>(challenge + kChallengeName) = static_cast<uint32_t>(wcslen(mapName));
    *reinterpret_cast<wchar_t**>(challenge + kChallengeName + 4) = mapName;
    g_saves = g_scan = g_unbind = 0;
}

// ---------------------------------------------------------------- the fake host
HorizonGameInfo g_gi;
std::map<std::string, std::string> g_settings;
std::vector<std::string> g_log, g_posted;
uint32_t g_mask = 0;
HorizonCallback g_hotkeyCb = nullptr;
void* g_hotkeyUser = nullptr;
int g_online = 0;

void HORIZON_CALL hLog(HorizonPluginHandle, int32_t, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log.push_back(buf);
    printf("      log: %s\n", buf);
}
int32_t HORIZON_CALL hGetString(HorizonPluginHandle, const char* k, const char* fb, char* out, uint32_t n) {
    auto it = g_settings.find(k);
    strncpy_s(out, n, it != g_settings.end() ? it->second.c_str() : fb, _TRUNCATE);
    return static_cast<int32_t>(strlen(out));
}
void HORIZON_CALL hSetString(HorizonPluginHandle, const char* k, const char* v) { g_settings[k] = v; }
int32_t HORIZON_CALL hGetInt(HorizonPluginHandle, const char* k, int32_t fb) {
    auto it = g_settings.find(k);
    return it != g_settings.end() ? atoi(it->second.c_str()) : fb;
}
void HORIZON_CALL hSetInt(HorizonPluginHandle, const char* k, int32_t v) { g_settings[k] = std::to_string(v); }
const HorizonGameInfo* HORIZON_CALL hGameInfo(void) { return &g_gi; }
void* HORIZON_CALL hEngine(uint32_t which) { return which == HORIZON_ENGINE_APP ? app : nullptr; }
int32_t HORIZON_CALL hHotkey(HorizonPluginHandle, const char*, const char*, const char*, HorizonCallback cb, void* user) {
    g_hotkeyCb = cb;
    g_hotkeyUser = user;
    return 1;
}
const char* HORIZON_CALL hHotkeyText(HorizonPluginHandle, int32_t) { return "F9"; }
void HORIZON_CALL hSubscribe(HorizonPluginHandle, uint32_t mask) { g_mask = mask; }
void HORIZON_CALL hPosted(HorizonPluginHandle, const char* path) { g_posted.push_back(path); }
void HORIZON_CALL hRun(HorizonPluginHandle, HorizonCallback cb, void* user) { cb(user); }
int32_t HORIZON_CALL hOnline(void) { return g_online; }
void HORIZON_CALL hNotify(HorizonPluginHandle, const HorizonToast*) {}

HorizonHostApi makeApi() {
    HorizonHostApi a = {};
    a.abi = HORIZON_ABI;
    a.size = sizeof(a);
    a.abi_minor = HORIZON_ABI_MINOR;
    a.plugin = reinterpret_cast<HorizonPluginHandle>(1);
    a.log = hLog;
    a.get_string = hGetString;
    a.set_string = hSetString;
    a.get_int = hGetInt;
    a.set_int = hSetInt;
    a.game_info = hGameInfo;
    a.engine_pointer = hEngine;
    a.register_hotkey = hHotkey;
    a.hotkey_text = hHotkeyText;
    a.subscribe = hSubscribe;
    a.post_replay_saved = hPosted;
    a.run_on_game_thread = hRun;
    a.is_online = hOnline;
    a.notify = hNotify;
    return a; // imgui_context NULL: the module runs without its menu page
}

struct Plugin {
    HMODULE mod = nullptr;
    HorizonPlugin_InfoFn info = nullptr;
    HorizonPlugin_InitFn init = nullptr;
    HorizonPlugin_ShutdownFn shutdown = nullptr;
    HorizonPlugin_OnEventFn onEvent = nullptr;
};
Plugin load(const char* dll) {
    Plugin p;
    p.mod = LoadLibraryA(dll);
    if (!p.mod) return p;
    p.info = reinterpret_cast<HorizonPlugin_InfoFn>(GetProcAddress(p.mod, HORIZON_EXPORT_INFO));
    p.init = reinterpret_cast<HorizonPlugin_InitFn>(GetProcAddress(p.mod, HORIZON_EXPORT_INIT));
    p.shutdown = reinterpret_cast<HorizonPlugin_ShutdownFn>(GetProcAddress(p.mod, HORIZON_EXPORT_SHUTDOWN));
    p.onEvent = reinterpret_cast<HorizonPlugin_OnEventFn>(GetProcAddress(p.mod, HORIZON_EXPORT_ON_EVENT));
    return p;
}
void unload(Plugin& p) {
    if (p.shutdown) p.shutdown();
    if (p.mod) FreeLibrary(p.mod);
    p = Plugin{};
}

HorizonEvent ev(uint32_t type) {
    HorizonEvent e = {};
    e.size = sizeof(e);
    e.type = type;
    e.play_mode = HORIZON_MODE_SOLO;
    e.respawns = -1;
    e.engine_race = race;
    return e;
}
// A run as the host reports it on Sunrise/Original: RACE_START, FINISH, (the game stops recording), RACE_END.
void run(const Plugin& p, int time, bool pb, uint32_t mode = HORIZON_MODE_SOLO, uint32_t endScreen = 1, bool finish = true,
         void* endRace = race) {
    *reinterpret_cast<int*>(replay + kReplayRecording) = 1;
    HorizonEvent s = ev(HORIZON_EVENT_RACE_START);
    p.onEvent(&s);
    if (finish) {
        if (pb) g_best = time;
        HorizonEvent f = ev(HORIZON_EVENT_FINISH);
        f.time_ms = time;
        f.is_local_player = 1;
        f.is_pb = pb ? 1 : 0;
        f.play_mode = mode;
        f.is_online = static_cast<uint32_t>(g_online);
        p.onEvent(&f);
    }
    *reinterpret_cast<int*>(replay + kReplayRecording) = 0; // the solo race end stops the recording
    HorizonEvent r = ev(HORIZON_EVENT_RACE_END);
    r.end_screen = endScreen;
    r.play_mode = endScreen ? HORIZON_MODE_SOLO : HORIZON_MODE_NET;
    r.engine_race = endRace;
    r.is_online = static_cast<uint32_t>(g_online);
    p.onEvent(&r);
}

bool there(const std::wstring& name) { return GetFileAttributesW((g_dir + name).c_str()) != INVALID_FILE_ATTRIBUTES; }
std::string readAll(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    char buf[1024];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf), &got, nullptr);
    CloseHandle(h);
    return std::string(buf, got);
}
std::string utf8(const std::wstring& w) {
    char b[1200];
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof(b), nullptr, nullptr);
    return b;
}
bool logHas(const char* s) {
    for (auto& l : g_log)
        if (l.find(s) != std::string::npos) return true;
    return false;
}

void testGame(const char* dll, const Game& g, const std::string& root) {
    printf("===== %s\n", g.key);
    g_dir = std::wstring(root.begin(), root.end()) + L"legacy-" + std::wstring(g.key, g.key + strlen(g.key)) + L"\\";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW((g_dir + L"*").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do DeleteFileW((g_dir + fd.cFileName).c_str());
        while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    buildGame(g);
    const HorizonHostApi api = makeApi();
    memset(&g_gi, 0, sizeof(g_gi));
    g_gi.size = sizeof(g_gi);
    g_gi.game = g.bit;
    g_gi.exe_sha256 = g.sha256;
    g_gi.exe_timestamp = g.timestamp;
    g_gi.exe_base = reinterpret_cast<uintptr_t>(g_img); // link 0x400000 is the image start
    g_gi.game_folder = "C:\\fake-legacy\\";
    g_settings.clear();
    g_settings["IniImported"] = "1";
    g_online = 0;

    Plugin p = load(dll);
    check(p.mod && p.info && p.init && p.onEvent, "plugin loaded, exports present");
    if (!p.mod) return;
    const HorizonPluginInfo* pi = p.info();
    check((pi->games & g.bit) != 0, "plugin info lists this game");
    // identity: another hash, then the right hash with another PE timestamp
    g_gi.exe_sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    check(p.init(&api) == 2, "another exe (hash): refused with 2");
    g_gi.exe_sha256 = g.sha256;
    g_gi.exe_timestamp = g.timestamp + 1;
    check(p.init(&api) == 2, "right hash, other PE timestamp: refused with 2");
    g_gi.exe_timestamp = g.timestamp;
    unload(p);

    p = load(dll);
    g_log.clear();
    g_posted.clear();
    check(p.init(&api) == 0, "supported exe: Init 0");
    check((g_mask & HORIZON_EVENT_BIT(HORIZON_EVENT_FINISH)) && (g_mask & HORIZON_EVENT_BIT(HORIZON_EVENT_RACE_END)),
          "FINISH and RACE_END subscribed");
    HorizonEvent m = ev(HORIZON_EVENT_MAP_LOADED);
    p.onEvent(&m);

    unsigned char replayBefore[sizeof(replay)];
    memcpy(replayBefore, replay, sizeof(replay));
    run(p, 46259, true);
    const std::wstring first = L"$s$08fTest Map(00'46''25).Replay.Gbx";
    check(g_saves == 1 && there(first), "new best: saved as <map>(00'46''25).Replay.Gbx in My Replays");
    check(g_args[0] == 10 && g_args[1] == 7 && g_args[2] == 1 && g_args[3] == 1 && g_saveNod == replay,
          "  the game's save with (10, 7, 1, 1) and the race replay (app+0x130)");
    check(g_unbind == 1 && g_scan == 1, "  replay unbound, replay list rescanned");
    check(memcmp(replayBefore, replay, sizeof(replay)) == 0, "  replay object not changed by the module");
    check(readAll(g_dir + first) == g_content, "  file exactly as the game's save wrote it (validable untouched)");
    check(g_posted.size() == 1 && g_posted[0] == utf8(g_dir + first), "  post_replay_saved with the full path");
    check(g_live == 0, "  game strings freed");

    run(p, 50000, false);
    check(g_saves == 1, "no new best (Mode=best): not saved");
    run(p, 40000, true, HORIZON_MODE_NET);
    check(g_saves == 1, "finish in a network race: not saved");
    g_online = 1;
    run(p, 39000, true);
    g_online = 0;
    check(g_saves == 1, "online: not saved");
    run(p, 38000, true, HORIZON_MODE_SOLO, 0);
    check(g_saves == 1, "race end without the solo end screen: not saved");
    run(p, 0, false, HORIZON_MODE_SOLO, 1, false);
    check(g_saves == 1, "race end without a finish: not saved");
    // finish, then a restart before the race end
    {
        g_best = 37000;
        HorizonEvent f = ev(HORIZON_EVENT_FINISH);
        f.time_ms = 37000, f.is_local_player = 1, f.is_pb = 1;
        p.onEvent(&f);
        HorizonEvent s = ev(HORIZON_EVENT_RACE_START);
        p.onEvent(&s);
        HorizonEvent r = ev(HORIZON_EVENT_RACE_END);
        r.end_screen = 1;
        p.onEvent(&r);
        check(g_saves == 1, "restart between finish and race end: not saved");
    }
    run(p, 36000, true, HORIZON_MODE_SOLO, 1, true, otherRace);
    check(g_saves == 1 && logHas("do not match"), "race of another app object: not saved");
    // still recording at the race end (should not happen)
    {
        g_best = 35000;
        HorizonEvent f = ev(HORIZON_EVENT_FINISH);
        f.time_ms = 35000, f.is_local_player = 1, f.is_pb = 1;
        p.onEvent(&f);
        *reinterpret_cast<int*>(replay + kReplayRecording) = 1;
        HorizonEvent r = ev(HORIZON_EVENT_RACE_END);
        r.end_screen = 1;
        p.onEvent(&r);
        *reinterpret_cast<int*>(replay + kReplayRecording) = 0;
        check(g_saves == 0 + 1 && logHas("still recording"), "replay still recording: skipped");
    }
    // new best whose name is taken
    {
        HANDLE h = CreateFileW((g_dir + L"$s$08fTest Map(00'34''00).Replay.Gbx").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        DWORD w = 0;
        WriteFile(h, "MINE", 4, &w, nullptr);
        CloseHandle(h);
        run(p, 34000, true);
        check(g_saves == 2 && there(L"$s$08fTest Map(00'34''00) (2).Replay.Gbx") &&
                  readAll(g_dir + L"$s$08fTest Map(00'34''00).Replay.Gbx") == "MINE",
              "name taken: \"... (2).Replay.Gbx\", the existing file untouched");
    }
    // stunts
    *reinterpret_cast<int*>(challenge + kChallengePlayMode) = 5;
    g_stunt = 77;
    run(p, 33000, true);
    *reinterpret_cast<int*>(challenge + kChallengePlayMode) = 0;
    check(there(L"$s$08fTest Map(77).Replay.Gbx"), "stunts: <map>(score).Replay.Gbx");
    // hotkey: saves the race replay now
    check(g_hotkeyCb != nullptr, "save hotkey registered");
    if (g_hotkeyCb) {
        const int before = g_saves;
        g_hotkeyCb(g_hotkeyUser);
        check(g_saves == before + 1, "hotkey: saved");
        g_online = 1;
        g_hotkeyCb(g_hotkeyUser);
        g_online = 0;
        check(g_saves == before + 1, "hotkey online: nothing saved");
    }
    check(g_live == 0, "no game string left allocated");
    unload(p);

    // Mode=all
    buildGame(g);
    p = load(dll);
    g_settings["Mode"] = "all";
    check(p.init(&api) == 0, "Mode=all: Init 0");
    run(p, 60000, false);
    check(g_saves == 1, "Mode=all: a finish that is no new best is saved too");
    unload(p);
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        printf("usage: legacy_fakehost <eswc-autosave.dll> [sunrise|original ...]\n");
        return 2;
    }
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string root = exe;
    root.resize(root.find_last_of('\\') + 1);
    for (const Game& g : kGames) {
        bool want = argc == 2;
        for (int i = 2; i < argc; i++) want = want || _stricmp(argv[i], g.key) == 0;
        if (want) testGame(argv[1], g, root);
    }
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
