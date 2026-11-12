// Unit tests for the game-settings core (gs_game, gs_master) against a fake game in memory: a buffer laid out
// like the exe (the table's own signature bytes, globals and reflection entries put where the table expects
// them) and fake objects behind the globals.
//   gs_tests.exe
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../gs_game.h"
#include "../gs_master.h"

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what) {
    if (ok) g_pass++;
    else g_fail++;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

static std::vector<std::string> g_log;
static void logCb(void*, int level, const char* text) {
    g_log.push_back(text);
    printf("      [%s] %s\n", level == 2 ? "error" : level == 1 ? "warn" : "info", text);
}
static bool logHas(const char* s) {
    for (const auto& l : g_log)
        if (l.find(s) != std::string::npos) return true;
    return false;
}

// a fake exe image + the objects the game would have made
struct Fake {
    const gs::GameTable* t;
    uint8_t* img = nullptr;
    size_t size = 0x900000;
    uint8_t app[0x40] = {};
    uint8_t config[0x120] = {};
    uint8_t display[0x40] = {};
    uint8_t mgr[0x40] = {};
    uint8_t engines[0x60] = {};
    uint8_t audioEngine[0x40] = {};
    uint8_t port[0x40] = {};
    uint8_t viewport[0x300] = {};

    explicit Fake(const gs::GameTable* table) : t(table) {
        img = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        for (const gs::Part* p : {&t->audio, &t->display, &t->toggle}) {
            for (int i = 0; i < p->codeCount; i++) memcpy(at(p->code[i].addr), p->code[i].bytes, p->code[i].len);
            for (int i = 0; i < p->ptrCount; i++) put(at(p->ptrs[i].addr), rel(p->ptrs[i].value));
            for (int i = 0; i < p->paramCount; i++) {
                const gs::ParamSig& s = p->params[i];
                const uint32_t w[4] = {s.type, s.id, 0, s.offset};
                memcpy(at(s.addr), w, sizeof(w));
            }
        }
        // objects: app -> config -> display; engines -> array -> audio engine -> port
        put(at(t->app), ptr(app));
        put(app + 0x18, ptr(config));
        put(config + 0x20, ptr(display));
        setF(config + t->cfgSound, 0.8f);
        setF(config + t->cfgMusic, 0.6f);
        put(display + 0x34, 0);
        put(at(t->engines), ptr(mgr));
        put(mgr + 0x28, ptr(engines));
        put(engines + 0x40, ptr(audioEngine));
        put(audioEngine + 0x2c, ptr(port));
        setF(port + 0x14, 0.8f);
        setF(port + 0x18, 0.6f);
        // the viewport with the game's vtable; at StartFullscreen / StopFullscreen tiny stand-ins (before the
        // checked bytes at +0x18): mov dword [ecx+0x260],1 (0); mov eax,1; ret
        put(app + t->vpOffset, ptr(viewport));
        put(viewport, rel(t->vpVtable));
        static const uint8_t kStart[] = {0xc7, 0x81, 0x60, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};
        static const uint8_t kStop[] = {0xc7, 0x81, 0x60, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc3};
        memcpy(at(t->toggle.ptrs[0].value), kStart, sizeof(kStart));
        memcpy(at(t->toggle.ptrs[1].value), kStop, sizeof(kStop));
    }
    ~Fake() { VirtualFree(img, 0, MEM_RELEASE); }
    uintptr_t base() const { return reinterpret_cast<uintptr_t>(img); }
    uint8_t* at(uintptr_t link) { return img + (link - 0x400000); }
    uint32_t rel(uintptr_t link) const { return static_cast<uint32_t>(link - 0x400000 + base()); }
    static uint32_t ptr(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
    static void put(void* p, uint32_t v) { memcpy(p, &v, 4); }
    static void setF(void* p, float v) { memcpy(p, &v, 4); }
    static float getF(const void* p) {
        float v;
        memcpy(&v, p, 4);
        return v;
    }
    static uint32_t get(const void* p) {
        uint32_t v;
        memcpy(&v, p, 4);
        return v;
    }
};

static void tableTests() {
    check(gs::tableForTimestamp(0x5ce7272f) == &gs::eswcTable(), "table: ESWC by timestamp");
    check(gs::tableForTimestamp(0x5cc8f0cf) == &gs::sunriseTable(), "table: Sunrise by timestamp");
    check(gs::tableForTimestamp(0x5d337477) == &gs::originalTable(), "table: Original by timestamp");
    check(gs::tableForTimestamp(0x12345678) == nullptr, "table: unknown exe -> none");
    check(gs::toPercent(0.0f) == 0 && gs::toPercent(1.0f) == 100 && gs::toPercent(0.555f) == 56 && gs::toPercent(-3.0f) == 0 &&
              gs::toPercent(7.0f) == 100,
          "percent: rounding and clamping");
    check(gs::fromPercent(50) == 0.5f && gs::fromPercent(-1) == 0.0f && gs::fromPercent(120) == 1.0f, "percent: back to 0..1");
}

// a drag at 60 frames per second for one second, 100 -> 40 %: applied at most every 100 ms, the end value on release
static void throttleTests() {
    gs::Throttle t(100);
    t.begin(100);
    int applied = 0, last = -1;
    DWORD now = 1000;
    for (int f = 0; f < 60; f++, now += 16) {
        const int v = 100 - f; // a new value every frame
        if (t.due(v, false, now)) {
            applied++;
            last = v;
        }
    }
    char what[96];
    _snprintf_s(what, _TRUNCATE, "throttle: 60 frames of dragging -> %d applies (at most 11)", applied);
    check(applied >= 9 && applied <= 11, what);
    check(t.due(40, true, now) && last != 40, "throttle: the value at release is applied at once");
    check(!t.due(40, true, now + 1), "throttle: the same value is not applied twice");
    gs::Throttle u(100);
    u.begin(50);
    check(!u.due(50, false, 0) && !u.due(50, true, 5), "throttle: a click without a change applies nothing");
    check(u.due(51, false, 7), "throttle: the first change of a drag is applied at once");
    check(!u.due(52, false, 50) && u.due(52, false, 108), "throttle: a held value is applied once 100 ms have passed");
}

static void gameTests(const gs::GameTable& t) {
    char what[160];
    printf("===== %s\n", t.name);
    {
        Fake f(&t);
        gs::Game g(&t, f.base(), &logCb, nullptr);
        _snprintf_s(what, _TRUNCATE, "%s: audio and window-mode signatures match (relocated image)", t.name);
        check(g.audioOk() && g.displayOk(), what);
        float m = -1, s = -1;
        check(g.readMusic(&m) && g.readSounds(&s) && m == 0.6f && s == 0.8f, "read: volumes from the audio port");
        check(g.setMusic(0.25f) && Fake::getF(f.port + 0x18) == 0.25f && Fake::getF(f.config + t.cfgMusic) == 0.25f,
              "set music: port and config");
        check(g.setSounds(1.5f) && Fake::getF(f.port + 0x14) == 1.0f && Fake::getF(f.config + t.cfgSound) == 1.0f, "set sounds: clamped to 1");
        bool fs = true;
        check(g.readFullscreen(&fs) && !fs, "window mode: read windowed");
        check(g.setFullscreen(true) && Fake::get(f.display + 0x34) == 1, "window mode: fullscreen written to the config");
        Fake::put(f.display + 0x34, 7);
        check(!g.readFullscreen(&fs) && !g.setFullscreen(false) && Fake::get(f.display + 0x34) == 7, "window mode: odd value -> not touched");
    }
    {
        // at start: no audio port yet (engines global empty), chosen values go into the config only
        Fake f(&t);
        Fake::put(f.at(t.engines), 0);
        gs::Game g(&t, f.base(), &logCb, nullptr);
        gs::Prefs p;
        p.music = 30;
        p.sounds = 70;
        p.window = 1;
        check(g.applyStart(p), "start: applied");
        check(Fake::getF(f.config + t.cfgMusic) == 0.3f && Fake::getF(f.config + t.cfgSound) == 0.7f && Fake::get(f.display + 0x34) == 1,
              "start: config holds the chosen values");
        check(Fake::getF(f.port + 0x18) == 0.6f, "start: port untouched (the game's own init copies the config)");
        gs::Prefs none;
        check(g.applyStart(none), "start: nothing chosen -> nothing to do");
    }
    {
        // the port holds something else -> only the config is written
        Fake f(&t);
        Fake::setF(f.port + 0x18, 5.0f);
        gs::Game g(&t, f.base(), &logCb, nullptr);
        g_log.clear();
        check(g.setMusic(0.4f) && Fake::getF(f.port + 0x18) == 5.0f && Fake::getF(f.config + t.cfgMusic) == 0.4f,
              "odd port values: port left alone, config written");
        check(logHas("left alone"), "odd port values: logged");
        Fake::setF(f.config + t.cfgSound, -2.0f);
        Fake::setF(f.port + 0x18, 0.5f);
        check(g.setSounds(0.3f) && Fake::getF(f.config + t.cfgSound) == -2.0f && Fake::getF(f.port + 0x14) == 0.3f,
              "odd config value: config left alone, port written");
    }
    {
        // a changed byte in the window-mode code: only that part is off
        Fake f(&t);
        f.at(t.display.code[t.display.codeCount - 1].addr)[0] ^= 0xff;
        gs::Game g(&t, f.base(), &logCb, nullptr);
        bool fs = false;
        check(!g.displayOk() && !g.readFullscreen(&fs) && !g.setFullscreen(true) && Fake::get(f.display + 0x34) == 0,
              "changed window-mode code: window mode off, nothing written");
        check(g.audioOk() && g.setMusic(0.1f), "changed window-mode code: volumes still on");
    }
    {
        // a changed member offset in the reflection: audio off
        Fake f(&t);
        Fake::put(f.at(t.audio.params[0].addr) + 12, t.audio.params[0].offset + 4);
        gs::Game g(&t, f.base(), &logCb, nullptr);
        float m = -1;
        check(!g.audioOk() && !g.readMusic(&m) && !g.setMusic(0.9f) && Fake::getF(f.port + 0x18) == 0.6f, "changed member offset: volumes off");
        check(g.displayOk(), "changed member offset: window mode still on");
        gs::Prefs p;
        p.music = 10;
        check(!g.applyStart(p) && Fake::getF(f.config + t.cfgMusic) == 0.6f, "changed member offset: start applies nothing");
    }
    {
        // another global in the code: off
        Fake f(&t);
        Fake::put(f.at(t.audio.ptrs[0].addr), 0x00123456);
        gs::Game g(&t, f.base(), &logCb, nullptr);
        check(!g.audioOk(), "other engine global in the code: volumes off");
    }
    {
        // no settings object yet (application global empty)
        Fake f(&t);
        Fake::put(f.at(t.app), 0);
        gs::Game g(&t, f.base(), &logCb, nullptr);
        gs::Prefs p;
        p.window = 1;
        p.music = 50;
        check(!g.applyStart(p), "no settings object: start reports it");
        bool fs = false;
        check(!g.readFullscreen(&fs), "no settings object: window mode unavailable");
        float m = -1;
        check(g.readMusic(&m) && m == 0.6f, "no settings object: volumes still read from the port");
    }
}

static void toggleTests(const gs::GameTable& t) {
    printf("===== window mode switch, %s\n", t.name);
    {
        Fake f(&t);
        gs::Game g(&t, f.base(), &logCb, nullptr);
        bool fs = true;
        check(g.toggleOk(), "switch: signatures match (relocated image)");
        check(g.fullscreenNow(&fs) && !fs, "switch: windowed now (the viewport's flag)");
        check(g.switchWindowMode(true) && Fake::get(f.viewport + 0x260) == 1 && Fake::get(f.display + 0x34) == 1,
              "switch: StartFullscreen called, config follows");
        check(g.fullscreenNow(&fs) && fs, "switch: fullscreen now");
        check(g.switchWindowMode(true), "switch: already fullscreen -> nothing to do");
        check(g.switchWindowMode(false) && Fake::get(f.viewport + 0x260) == 0 && Fake::get(f.display + 0x34) == 0,
              "switch: StopFullscreen called, back to windowed");
    }
    {
        // the game refuses (StartFullscreen returns 0 and stays windowed)
        Fake f(&t);
        static const uint8_t kRefuse[] = {0xb8, 0x00, 0x00, 0x00, 0x00, 0xc3};
        memcpy(f.at(t.toggle.ptrs[0].value), kRefuse, sizeof(kRefuse));
        gs::Game g(&t, f.base(), &logCb, nullptr);
        check(!g.switchWindowMode(true) && Fake::get(f.display + 0x34) == 0, "switch refused by the game: reported, config untouched");
    }
    {
        // the viewport is another class: never called
        Fake f(&t);
        Fake::put(f.viewport, f.rel(t.vpVtable) + 4);
        gs::Game g(&t, f.base(), &logCb, nullptr);
        bool fs = false;
        check(!g.fullscreenNow(&fs) && !g.switchWindowMode(true) && Fake::get(f.viewport + 0x260) == 0, "other viewport class: not called");
    }
    {
        // StopFullscreen's code differs: the switch is off, the volumes are not affected
        Fake f(&t);
        f.at(t.toggle.code[1].addr)[3] ^= 0xff;
        gs::Game g(&t, f.base(), &logCb, nullptr);
        bool fs = false;
        check(!g.toggleOk() && !g.fullscreenNow(&fs) && !g.switchWindowMode(true), "changed switch code: window mode off");
        check(g.audioOk(), "changed switch code: volumes still on");
    }
    {
        // a vtable slot that leads elsewhere
        Fake f(&t);
        Fake::put(f.at(t.toggle.ptrs[1].addr), f.rel(0x00401000));
        gs::Game g(&t, f.base(), &logCb, nullptr);
        check(!g.toggleOk(), "changed vtable slot: window mode off");
    }
}

static int g_ran = 0;
static void countRun(void* user) { g_ran += *static_cast<int*>(user); }

static void betweenFramesTest() {
    int one = 1;
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {}
    check(gs::runBetweenFrames(&countRun, &one), "between frames: posted");
    check(!gs::runBetweenFrames(&countRun, &one), "between frames: a second call waits for the first");
    check(g_ran == 0, "between frames: nothing runs before the game takes its messages");
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {}
    check(g_ran == 1, "between frames: runs once when the message is taken");
    check(gs::runBetweenFrames(&countRun, &one), "between frames: posted again");
    gs::stopBetweenFrames();
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {}
    check(g_ran == 1, "between frames: stopped: a waiting call is dropped");
}

static void masterTest() {
    gs::MasterVolume mv;
    check(mv.start(&logCb, nullptr), "volume: worker starts");
    check(mv.start(&logCb, nullptr), "volume: second start is a no-op");
    const DWORD t0 = GetTickCount();
    while (mv.get() < 0.0f && GetTickCount() - t0 < 2000) Sleep(50);
    const float v = mv.get();
    printf("      session volume: %.3f%s\n", v, v < 0 ? " (no audio session here)" : "");
    check(v == -1.0f || (v >= 0.0f && v <= 1.0f), "volume: -1 or 0..1");
    if (v >= 0.0f) {
        mv.set(v); // same value back: nothing audible changes
        Sleep(200);
        check(mv.get() >= 0.0f, "volume: still reachable after a set");
    }
    mv.stop();
    mv.stop();
    check(true, "volume: stops (twice)");
}

int main() {
    tableTests();
    throttleTests();
    gameTests(gs::eswcTable());
    gameTests(gs::sunriseTable());
    gameTests(gs::originalTable());
    toggleTests(gs::eswcTable());
    toggleTests(gs::sunriseTable());
    toggleTests(gs::originalTable());
    betweenFramesTest();
    masterTest();
    printf("gs_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
