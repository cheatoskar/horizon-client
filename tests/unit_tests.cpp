// Unit tests for the host's pure logic: profile.json, plugin.json, hotkeys, settings, JSON writer.
#include <windows.h>

#include <cstdio>
#include <map>
#include <string>

#include "common/hotkeys.h"
#include "common/json.h"
#include "common/profile.h"
#include "common/settings.h"
#include "common/util.h"
#include "horizon_plugin.h"
#include "host/game/forever_poll.h"
#include "host/bridge_logic.h"
#include "host/exe_known.h"
#include "host/stats_log.h"
#include "host/toast_queue.h"
#include "../tests/abi12/abi12_sizes.h"

// ABI 1.3 layout: one minor for three branches' additions, appended after the 1.2 tables in a fixed order
// (open_page, notify, bridge_request, bridge_state; post_present at the end of the device callbacks).
// ABI12_* are the sizes the 1.2 test plugin (tests/abi12) is built with.
static_assert(sizeof(void*) == 4, "the plugin ABI is 32-bit x86");
static_assert(HORIZON_ABI == 1 && HORIZON_ABI_MINOR == 3, "ABI 1.3");
static_assert(offsetof(HorizonHostApi, host_flags) + sizeof(void*) == ABI12_HOST_API_SIZE, "1.2 table ends with host_flags");
static_assert(offsetof(HorizonHostApi, open_page) == ABI12_HOST_API_SIZE, "open_page is the first 1.3 field");
static_assert(offsetof(HorizonHostApi, notify) == ABI12_HOST_API_SIZE + 4, "notify follows open_page");
static_assert(offsetof(HorizonHostApi, bridge_request) == ABI12_HOST_API_SIZE + 8, "bridge_request follows notify");
static_assert(offsetof(HorizonHostApi, bridge_state) == ABI12_HOST_API_SIZE + 12, "bridge_state follows bridge_request");
static_assert(sizeof(HorizonHostApi) == ABI12_HOST_API_SIZE + 16, "nothing else in 1.3");
static_assert(offsetof(HorizonDeviceCallbacks, post_present) == ABI12_DEVICE_CALLBACKS_SIZE, "post_present appended");
static_assert(sizeof(HorizonDeviceCallbacks) == ABI12_DEVICE_CALLBACKS_SIZE + 4, "nothing else in the 1.3 callbacks");
static_assert(sizeof(HorizonPluginInfo) == ABI12_PLUGIN_INFO_SIZE, "HorizonPluginInfo unchanged");

using namespace horizon;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                          \
    } while (0)

static void testProfile() {
    // the full format
    Profile p = parseProfile(R"({
      "format": 1, "profile": "My ESWC", "game": "eswc", "safe_mode": false, "menu_key": "Ctrl+Tab", "bar": false,
      "plugins": [ {"id": "eswc-autosave", "version": "2.0.0", "enabled": true},
                   {"id": "legacy-randomizer", "enabled": false},
                   "sample" ],
      "written_by": "TM Horizon 0.3.0", "written_at": "2026-11-11T19:00:00Z", "unknown_key": 5 })");
    CHECK(p.valid);
    CHECK(p.name == "My ESWC");
    CHECK(p.menuKey == "Ctrl+Tab");
    CHECK(!p.bar);
    CHECK(p.plugins.size() == 3);
    CHECK(p.find("eswc-autosave") && p.find("eswc-autosave")->version == "2.0.0");
    CHECK(p.find("legacy-randomizer") && !p.find("legacy-randomizer")->enabled);
    CHECK(p.find("sample") && p.find("sample")->enabled);
    CHECK(p.writtenBy == "TM Horizon 0.3.0");
    // UTF-8 BOM (PowerShell's Out-File) is fine
    CHECK(parseProfile("\xEF\xBB\xBF{\"format\":1}").valid);
    // safe mode, empty list
    Profile s = parseProfile(R"({"format":1,"safe_mode":true})");
    CHECK(s.valid && s.safeMode && s.plugins.empty() && s.bar);
    // errors: not JSON, no format, newer format, plugins not an array
    CHECK(!parseProfile("{nope").valid);
    CHECK(!parseProfile(R"({"plugins":[]})").valid);
    Profile newer = parseProfile(R"({"format":2})");
    CHECK(!newer.valid && newer.error.find("newer") != std::string::npos);
    CHECK(!parseProfile(R"({"format":1,"plugins":{}})").valid);
    // bad entries are skipped with a warning, the rest loads
    Profile w = parseProfile(R"({"format":1,"plugins":[{"id":"../evil"},{"id":"Upper"},5,{"id":"ok-1"},{"id":"ok-1"}]})");
    CHECK(w.valid && w.plugins.size() == 1 && w.warnings.size() == 4);
}

static void testManifest() {
    PluginManifest m = parsePluginManifest(R"({"id":"eswc-autosave","name":"ESWC Autosave","version":"2.0.0",
        "games":["eswc"],"online":"online-ok","abi":1})", "eswc-autosave");
    CHECK(m.valid && m.dll == "eswc-autosave.dll" && m.games == HORIZON_GAME_ESWC && !m.soloOnly && m.abi == 1 && m.enabledByDefault);
    CHECK(!parsePluginManifest(R"({"id":"other"})", "eswc-autosave").valid);  // id must match the folder
    CHECK(!parsePluginManifest(R"({"dll":"..\\x.dll"})", "a").valid);         // dll must stay in the folder
    PluginManifest d = parsePluginManifest(R"({"enabled_by_default":false})", "x");
    CHECK(d.valid && d.soloOnly && !d.enabledByDefault && d.dll == "x.dll");
    CHECK(parsePluginManifest(R"({"icon":"icon.png"})", "x").icon == "icon.png");
    CHECK(parsePluginManifest(R"({"icon":"..\\x.png"})", "x").icon.empty()); // icon must stay in the folder
    CHECK(parsePluginManifest(R"({"icon":"sub/x.png"})", "x").icon.empty());
    // save_replay names
    CHECK(replayStemFromName(L"My run.Replay.Gbx") == L"My run");
    CHECK(replayStemFromName(L"a\\b/c:d*?.replay.gbx") == L"a_b_c_d__");
    CHECK(replayStemFromName(L"trailing. . ") == L"trailing");
    CHECK(replayStemFromName(std::wstring(300, L'x')).size() == 120);
    CHECK(gameBitFromName("TMUF") == HORIZON_GAME_TMUF && gameBitFromName("x") == 0);
}

static void testHotkeys() {
    KeyCombo k;
    CHECK(parseKey("Insert", k) && k.vk == VK_INSERT && !k.ctrl && keyText(k) == "Insert");
    CHECK(parseKey("ctrl+tab", k) && k.vk == VK_TAB && k.ctrl && keyText(k) == "Ctrl+Tab");
    CHECK(parseKey("Shift+F11", k) && k.vk == VK_F11 && k.shift && keyText(k) == "Shift+F11");
    CHECK(parseKey("Ctrl+Shift+Alt+A", k) && k.vk == 'A' && k.ctrl && k.shift && k.alt);
    CHECK(parseKey("", k) && k.empty());
    CHECK(parseKey("None", k) && k.empty());
    CHECK(!parseKey("Ctrl", k));            // a modifier alone
    CHECK(!parseKey("F9+F10", k));          // two keys
    CHECK(!parseKey("Hyper+Q", k));         // unknown name
    CHECK(parseKey("0x78", k) && k.vk == VK_F9);

    HotkeyRegistry r;
    KeyCombo f9, ins;
    parseKey("F9", f9);
    parseKey("Insert", ins);
    int fired = 0;
    auto cb = [](void* u) { ++*static_cast<int*>(u); };
    const int menu = r.add("host", "Horizon", "menu", "Open menu", ins, nullptr, cb, &fired);
    const int save = r.add("eswc-autosave", "ESWC Autosave", "save", "Save replay now", f9, nullptr, cb, &fired);
    const int other = r.add("x", "X Mod", "go", "Go", f9, nullptr, cb, &fired);
    CHECK(r.find(save)->active && !r.find(save)->conflict.empty());     // first wins, conflict shown
    CHECK(!r.find(other)->active && r.find(other)->conflict.find("ESWC Autosave") != std::string::npos);
    CHECK(r.matching(f9).size() == 1);
    // a stored user binding wins over the default
    const std::string stored = "Ctrl+F9";
    const int third = r.add("y", "Y", "act", "Act", f9, &stored, cb, &fired);
    CHECK(keyText(r.find(third)->key) == "Ctrl+F9" && r.find(third)->active);
    // rebinding solves the conflict
    CHECK(r.rebind(other, "F10"));
    CHECK(r.find(other)->active && r.find(save)->conflict.empty());
    CHECK(!r.rebind(other, "Bogus"));
    // keys of other mods are marked, not disabled
    std::vector<ExternalKey> ext;
    KeyCombo f8;
    parseKey("F8", f8);
    ext.push_back(ExternalKey{f8, "TM Vibrant Shaders"});
    r.setExternal(ext);
    CHECK(r.rebind(menu, "F8"));
    CHECK(r.find(menu)->active && r.find(menu)->conflict.find("TM Vibrant Shaders") != std::string::npos);
    r.removeOwner("x");
    CHECK(r.find(other) == nullptr);
}

static void testSettings() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"horizon_unit_test\\settings\\p.json";
    DeleteFileW(path.c_str());
    {
        SettingsFile s;
        CHECK(s.load(path)); // missing = empty
        CHECK(s.getInt("Mode", 7) == 7);
        s.setString("Mode", "best \"quoted\" \\ \xC3\xA4");
        s.setNumber("ScanReplays", 1);
        s.setNumber("Volume", 0.25);
        CHECK(s.dirty());
        CHECK(s.saveIfDirty());
        CHECK(!s.dirty());
    }
    SettingsFile t;
    CHECK(t.load(path));
    CHECK(t.getString("Mode", "") == "best \"quoted\" \\ \xC3\xA4");
    CHECK(t.getInt("ScanReplays", 0) == 1);
    CHECK(t.getFloat("Volume", 0) == 0.25f);
    CHECK(t.getInt("Mode", 3) == 3); // not a number
    CHECK(!t.loadText("[1,2]"));
}

static void testJsonRoundTrip() {
    Json o;
    std::string err;
    CHECK(JsonParser::parse(R"({"a":"x\ny\t\"z\"","b":[1,2.5,true,null],"c":{}})", o, err));
    std::string out;
    jsonWrite(out, o);
    Json back;
    CHECK(JsonParser::parse(out, back, err));
    CHECK(back.get("a") && back.get("a")->str == "x\ny\t\"z\"");
    CHECK(back.get("b") && back.get("b")->arr.size() == 4 && back.get("b")->arr[1].num == 2.5);
}

static void testIds() {
    CHECK(validId("eswc-autosave") && validId("legacy-randomizer") && validId("a.b_c"));
    CHECK(!validId("") && !validId(".x") && !validId("A") && !validId("a/b") && !validId("a..b") && !validId("a b"));
}

// ---------------------------------------------------------------- Forever polling (fake game memory)
namespace {
struct FakeMem : fpoll::Mem {
    std::map<uint32_t, uint32_t> w;
    std::map<uint32_t, fpoll::Kind> k;
    std::wstring text;
    uint32_t textAddr = 0;
    bool read32(uintptr_t a, uint32_t* o) override {
        auto it = w.find(static_cast<uint32_t>(a));
        if (it == w.end()) return false;
        *o = it->second;
        return true;
    }
    fpoll::Kind kindOf(uintptr_t o) override {
        auto it = k.find(static_cast<uint32_t>(o));
        return it == k.end() ? fpoll::Kind::Unknown : it->second;
    }
    bool readWide(uintptr_t a, uint32_t len, wchar_t* out) override {
        if (a != textAddr || len > text.size()) return false;
        for (uint32_t i = 0; i < len; i++) out[i] = text[i];
        return true;
    }
};
const uint32_t kApp = 0x100000, kCh = 0x200000, kRace = 0x300000, kInfo = 0x400000, kPar = 0x500000;

// A menu: app only. Then a map (challenge with a name), a race, the player info.
void setApp(FakeMem& m, bool challenge, uint32_t race, fpoll::Kind raceKind) {
    m.k[kApp] = fpoll::Kind::App;
    m.w[kApp + 0x198] = challenge ? kCh : 0;
    m.w[kApp + 0x414] = 0;
    m.w[kApp + 0x454] = race;
    if (challenge) {
        m.k[kCh] = fpoll::Kind::Challenge;
        m.w[kCh + 0x108] = 6; // name length
        m.w[kCh + 0x10c] = 0x600000;
        m.w[kCh + 0xb4] = kPar;
        m.w[kCh + 0xf8] = 0;
        m.w[kPar + 0x20] = 41000; m.w[kPar + 0x1c] = 43000; m.w[kPar + 0x18] = 48000; m.w[kPar + 0x14] = 60000; m.w[kPar + 0x28] = 0;
        m.textAddr = 0x600000;
        m.text = L"$o$f00A01 x";
    }
    if (race) {
        m.k[kRace] = raceKind;
        m.w[kRace + 0x98] = kInfo;
        m.k[kInfo] = fpoll::Kind::PlayerInfo;
    }
}
void setInfo(FakeMem& m, uint32_t state, int32_t time, uint32_t best) {
    m.w[kInfo + 0x314] = state;
    m.w[kInfo + 0x2b0] = static_cast<uint32_t>(time);
    m.w[kInfo + 0x2b4] = best;
}
fpoll::Obs poll(FakeMem& m) {
    fpoll::Obs o;
    fpoll::Layout L;
    fpoll::observe(m, L, kApp, &o);
    return o;
}
} // namespace

static void testForeverObserve() {
    FakeMem m;
    // menu: app only
    setApp(m, false, 0, fpoll::Kind::Unknown);
    fpoll::Obs o = poll(m);
    CHECK(o.app && !o.challenge && !o.race && !o.haveInfo);
    CHECK(fpoll::stateOf(o) == HORIZON_STATE_MAIN_MENU);
    // no app yet (startup) or a wrong object at the app global
    fpoll::Obs none;
    fpoll::Layout L;
    FakeMem empty;
    CHECK(!fpoll::observe(empty, L, kApp, &none) && !none.app);
    CHECK(fpoll::stateOf(none) == HORIZON_STATE_LOADING);
    // map set, race not there yet: loading
    setApp(m, true, 0, fpoll::Kind::Unknown);
    o = poll(m);
    CHECK(o.challenge == kCh && o.nameLen == 6 && fpoll::stateOf(o) == HORIZON_STATE_LOADING);
    // solo race: driving, then the end screen
    setApp(m, true, kRace, fpoll::Kind::RaceSolo);
    setInfo(m, 1, 12345, fpoll::kNoTime);
    o = poll(m);
    CHECK(o.haveInfo && o.infoPath == 1 && o.raceState == 1 && o.raceTime == 12345 && o.best == fpoll::kNoTime);
    CHECK(fpoll::stateOf(o) == HORIZON_STATE_SOLO_RACE && fpoll::playModeOf(o) == HORIZON_MODE_SOLO);
    setInfo(m, 2, 45123, 45123);
    CHECK(fpoll::stateOf(poll(m)) == HORIZON_STATE_END_RACE);
    // a race state outside 0..2 is not trusted
    setInfo(m, 7, 0, 0);
    o = poll(m);
    CHECK(!o.haveInfo && fpoll::stateOf(o) == HORIZON_STATE_SOLO_RACE);
    // network race, replay race, editors
    setApp(m, true, kRace, fpoll::Kind::RaceNet);
    setInfo(m, 1, 100, fpoll::kNoTime);
    o = poll(m);
    CHECK(fpoll::stateOf(o) == HORIZON_STATE_NET_RACE && fpoll::playModeOf(o) == HORIZON_MODE_NET);
    setApp(m, true, kRace, fpoll::Kind::RaceOther);
    o = poll(m);
    CHECK(fpoll::stateOf(o) == HORIZON_STATE_REPLAY && !o.haveInfo);
    setApp(m, true, 0, fpoll::Kind::Unknown);
    m.w[kApp + 0x414] = 0x700000;
    m.k[0x700000] = fpoll::Kind::EditorOther;
    CHECK(fpoll::stateOf(poll(m)) == HORIZON_STATE_EDITOR && fpoll::playModeOf(poll(m)) == HORIZON_MODE_EDITOR_TEST);
    m.k[0x700000] = fpoll::Kind::EditorReplay;
    CHECK(fpoll::stateOf(poll(m)) == HORIZON_STATE_REPLAY && fpoll::playModeOf(poll(m)) == HORIZON_MODE_REPLAY);
    m.k[0x700000] = fpoll::Kind::Unknown; // an object that is not a known editor class is ignored
    CHECK(fpoll::stateOf(poll(m)) == HORIZON_STATE_LOADING);
    // the fallback path: target info missing, player array race+0x28 -> [0] -> +0x1c
    setApp(m, true, kRace, fpoll::Kind::RaceSolo);
    m.w[kRace + 0x98] = 0;
    m.w[kRace + 0x24] = 1;
    m.w[kRace + 0x28] = 0x800000;
    m.w[0x800000] = 0x810000;
    m.w[0x810000 + 0x1c] = kInfo;
    setInfo(m, 0, -2000, fpoll::kNoTime);
    o = poll(m);
    CHECK(o.haveInfo && o.infoPath == 2 && o.raceState == 0 && o.raceTime == -2000);
    // the target points at a wrong object class: fallback as well
    m.w[kRace + 0x98] = 0x900000;
    m.k[0x900000] = fpoll::Kind::App;
    CHECK(poll(m).infoPath == 2);
    // a bad player count blocks the fallback
    m.w[kRace + 0x24] = 0;
    CHECK(!poll(m).haveInfo);
}

static void testForeverTracker() {
    using fpoll::Obs;
    auto frame = [](uintptr_t ch, uint32_t nameLen, uintptr_t race, uint32_t state, int32_t time, uint32_t best) {
        Obs o;
        o.app = true;
        o.challenge = ch;
        o.nameLen = nameLen;
        if (race) {
            o.race = race;
            o.raceKind = fpoll::Kind::RaceSolo;
            o.info = 0x1000;
            o.haveInfo = true;
            o.raceState = state;
            o.raceTime = time;
            o.best = best;
        }
        return o;
    };
    fpoll::Tracker t;
    // menu: nothing
    fpoll::Step s = t.step(frame(0, 0, 0, 0, 0, 0));
    CHECK(!s.mapLoaded && !s.mapGone && !s.raceStart && !s.finish);
    // map set with its name: MAP_LOADED once
    s = t.step(frame(0x2000, 6, 0, 0, 0, 0));
    CHECK(s.mapLoaded);
    CHECK(!t.step(frame(0x2000, 6, 0, 0, 0, 0)).mapLoaded);
    // race appears, countdown (0), start (1)
    s = t.step(frame(0x2000, 6, 0x3000, 0, -1500, fpoll::kNoTime));
    CHECK(!s.raceStart && !s.finish);
    s = t.step(frame(0x2000, 6, 0x3000, 1, 10, fpoll::kNoTime));
    CHECK(s.raceStart);
    CHECK(!t.step(frame(0x2000, 6, 0x3000, 1, 5000, fpoll::kNoTime)).raceStart);
    // finish 1 -> 2: first finish is a PB, the time is read once
    s = t.step(frame(0x2000, 6, 0x3000, 2, 45123, 45123));
    CHECK(s.finish && s.finishTime == 45123 && s.pb);
    CHECK(!t.step(frame(0x2000, 6, 0x3000, 2, 45123, 45123)).finish); // stays on the end screen: no second finish
    // restart through the countdown, slower run: finish, not a PB
    s = t.step(frame(0x2000, 6, 0x3000, 0, -1500, 45123));
    CHECK(!s.raceStart);
    s = t.step(frame(0x2000, 6, 0x3000, 1, 5, 45123));
    CHECK(s.raceStart);
    s = t.step(frame(0x2000, 6, 0x3000, 2, 46000, 45123));
    CHECK(s.finish && s.finishTime == 46000 && !s.pb);
    // restart from the end screen with a faster run: PB
    t.step(frame(0x2000, 6, 0x3000, 0, -1500, 45123));
    CHECK(t.step(frame(0x2000, 6, 0x3000, 1, 5, 45123)).raceStart);
    s = t.step(frame(0x2000, 6, 0x3000, 2, 44000, 45123));
    CHECK(s.finish && s.pb);
    // restart while driving, without a countdown frame in between: the time goes back
    t.step(frame(0x2000, 6, 0x3000, 0, -1500, 44000));
    t.step(frame(0x2000, 6, 0x3000, 1, 100, 44000));
    t.step(frame(0x2000, 6, 0x3000, 1, 9000, 44000));
    CHECK(t.step(frame(0x2000, 6, 0x3000, 1, 40, 44000)).raceStart);
    // back to the menu, then another map: map gone, new MAP_LOADED, the session best starts over
    s = t.step(frame(0, 0, 0, 0, 0, 0));
    CHECK(s.mapGone);
    s = t.step(frame(0x2800, 5, 0, 0, 0, 0));
    CHECK(s.mapLoaded);
    t.step(frame(0x2800, 5, 0x3800, 0, -1500, fpoll::kNoTime));
    t.step(frame(0x2800, 5, 0x3800, 1, 5, fpoll::kNoTime));
    s = t.step(frame(0x2800, 5, 0x3800, 2, 60000, 60000));
    CHECK(s.finish && s.pb);
    // a best time the game already had (earlier run) that is not worse: not a PB even as first finish of this map
    t.step(frame(0, 0, 0, 0, 0, 0));
    t.step(frame(0x2900, 5, 0, 0, 0, 0));
    t.step(frame(0x2900, 5, 0x3900, 0, -1500, 40000));
    t.step(frame(0x2900, 5, 0x3900, 1, 5, 40000));
    s = t.step(frame(0x2900, 5, 0x3900, 2, 41000, 40000));
    CHECK(s.finish && !s.pb);
}

static void testForeverTrackerEdges() {
    using fpoll::Obs;
    fpoll::Tracker t;
    auto race = [](uint32_t state, int32_t time, fpoll::Kind kind, uintptr_t info) {
        Obs o;
        o.app = true;
        o.challenge = 0x2000;
        o.nameLen = 4;
        o.race = 0x3000;
        o.raceKind = kind;
        o.info = info;
        o.haveInfo = kind == fpoll::Kind::RaceSolo || kind == fpoll::Kind::RaceNet;
        o.raceState = state;
        o.raceTime = time;
        return o;
    };
    // the host starts on the end screen: a stale finished state is not a finish
    fpoll::Step s = t.step(race(2, 30000, fpoll::Kind::RaceSolo, 0x1000));
    CHECK(s.mapLoaded && !s.finish && !s.raceStart);
    // the host starts mid-run: counts as a start
    fpoll::Tracker u;
    s = u.step(race(1, 8000, fpoll::Kind::RaceSolo, 0x1000));
    CHECK(s.raceStart);
    // replays and video races give no run events
    fpoll::Tracker r;
    r.step(race(0, 0, fpoll::Kind::RaceOther, 0));
    s = r.step(race(1, 5, fpoll::Kind::RaceOther, 0));
    CHECK(!s.raceStart && !s.finish);
    // a new info object (new race) is a new run: no finish from the old state
    fpoll::Tracker n;
    n.step(race(1, 9000, fpoll::Kind::RaceSolo, 0x1000));
    s = n.step(race(2, 100, fpoll::Kind::RaceSolo, 0x1200));
    CHECK(!s.finish);
    // the map name never shows: MAP_LOADED after the wait
    fpoll::Tracker w;
    Obs noName = race(0, 0, fpoll::Kind::Unknown, 0);
    noName.nameLen = 0;
    noName.race = 0;
    bool got = false;
    int at = 0;
    for (int i = 1; i <= 200 && !got; i++) {
        got = w.step(noName).mapLoaded;
        at = i;
    }
    CHECK(got && at == fpoll::Tracker::kMapWaitFrames);
    // pieces
    CHECK(utf8(fpoll::stripCodes(L"$o$f00A01 $$ W$wor$ld", 21)) == "A01 $ Word");
    CHECK(fpoll::stripCodes(L"ab$", 3) == L"ab");
    CHECK(fpoll::mapTime(0) == -1 && fpoll::mapTime(45000) == 45000 && fpoll::mapTime(0xffffffffu) == -1);
    CHECK(fpoll::validTime(1) && !fpoll::validTime(0) && !fpoll::validTime(fpoll::kNoTime));
}

// ABI 1.3: toasts (queue, do not disturb, hold while driving, capture pause) and the bridge's pure parts
static void testToasts() {
    ToastQueue q;
    auto item = [](const char* t, bool important = false) {
        ToastItem i;
        i.title = t;
        i.durationMs = 1000;
        i.important = important;
        return i;
    };
    ToastConditions c;
    for (int i = 0; i < 5; i++) q.push(item("t"));
    q.step(0, c);
    CHECK(q.visible().size() == ToastQueue::kMaxVisible && q.pending().size() == 2);
    q.step(1300, c); // the first three ran out
    CHECK(q.history().size() == 3 && !q.history()[0].missed);
    q.step(0, c);
    CHECK(q.visible().size() == 2 && q.pending().empty());
    // capture: timers paused, nothing moves
    c.hidden = true;
    q.push(item("during capture"));
    q.step(5000, c);
    CHECK(q.visible().size() == 2 && q.pending().size() == 1);
    c.hidden = false;
    // do not disturb: straight into the history as missed; important ones still show
    q.step(1300, c);
    ToastQueue d;
    ToastConditions dnd;
    dnd.dnd = true;
    d.push(item("quiet"));
    d.push(item("loud", true));
    d.step(0, dnd);
    CHECK(d.visible().size() == 1 && d.visible()[0].title == "loud" && d.history().size() == 1 && d.history()[0].missed);
    // hold while driving
    ToastQueue h;
    ToastConditions drive;
    drive.driving = true;
    drive.holdWhileDriving = true;
    h.push(item("later"));
    h.step(100, drive);
    CHECK(h.visible().empty() && h.pending().size() == 1);
    drive.driving = false;
    h.step(100, drive);
    CHECK(h.visible().size() == 1);
    // fade
    ToastItem f = item("f");
    f.shownMs = 0;
    CHECK(ToastQueue::alpha(f) == 0.0f);
    f.shownMs = 500;
    CHECK(ToastQueue::alpha(f) == 1.0f);
    // limits
    ToastQueue big;
    for (int i = 0; i < 40; i++) big.push(item("x"));
    CHECK(big.pending().size() == ToastQueue::kMaxPending && big.history().size() == 16);
    ToastItem longOne = item("y");
    longOne.durationMs = 999999;
    big.push(longOne);
    CHECK(big.pending().back().durationMs == 30000);
}

static void testBridgeLogic() {
    bridge::HttpAnswer a;
    CHECK(bridge::parseAnswer("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: 4\r\n\r\nabcdEXTRA", &a));
    CHECK(a.status == 200 && a.contentType == "image/jpeg" && a.body == "abcd");
    CHECK(bridge::parseAnswer("HTTP/1.1 429 Too Many\r\nRetry-After: 3\r\n\r\n", &a) && a.status == 429 && a.retryAfterS == 3);
    CHECK(!bridge::parseAnswer("garbage", &a));
    CHECK(!bridge::parseAnswer("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", &a));
    bridge::RateLimit rl(4.0, 8.0);
    int ok = 0;
    for (int i = 0; i < 20; i++) ok += rl.take(1000) ? 1 : 0;
    CHECK(ok == 8);
    CHECK(!rl.take(1100) && rl.take(1300)); // 0.4 then 1.2 tokens
    std::vector<bridge::Event> ev;
    long long last = 0;
    CHECK(bridge::parseEvents(R"({"ok":true,"last":7,"events":[{"id":5,"kind":"upload","level":"ok","title":"Upload done","text":"A01"},
                                  {"id":7,"kind":"mod","level":"info","title":""}]})", &ev, &last));
    CHECK(ev.size() == 1 && ev[0].title == "Upload done" && last == 7);
    CHECK(!bridge::parseEvents("{oops", &ev, &last));
    std::string key;
    CHECK(bridge::parsePair(R"({"ok":true,"key":"0123456789abcdef0123"})", &key) == "ok" && key.size() == 20);
    CHECK(bridge::parsePair(R"({"ok":false,"status":"pending"})", &key) == "pending");
    CHECK(bridge::parsePair(R"({"ok":true,"key":"short"})", &key) == "");
    CHECK(bridge::validPath("/v1/tmx/search?game=tmnf&name=a%20b"));
    CHECK(!bridge::validPath("/v1/client/pair") && !bridge::validPath("/v2/x") && !bridge::validPath("/v1/a b") && !bridge::validPath(nullptr));
    CHECK(std::string(bridge::siteCode(HORIZON_GAME_ESWC)) == "tmn" && std::string(bridge::siteCode(HORIZON_GAME_TMUF)) == "tmuf");
    CHECK(bridge::toastKind("error") == HORIZON_TOAST_ERROR && bridge::toastKind("x") == HORIZON_TOAST_INFO);
}

// Statistics lines for TM Horizon (profile.json stats_dir): a whole run as the host dispatches it
static void testStatsLines() {
    Profile p = parseProfile(R"({"format":1,"plugins":[],"stats_dir":"  C:\\Users\\me\\host-stats\\tmnf  "})");
    CHECK(p.valid && p.statsDir == "C:\\Users\\me\\host-stats\\tmnf");
    CHECK(parseProfile(R"({"format":1,"plugins":[]})").statsDir.empty());

    stats::Recorder r;
    HorizonMapInfo mi = {};
    HorizonEvent e = {};
    e.map = &mi;
    e.play_mode = HORIZON_MODE_SOLO;
    e.type = HORIZON_EVENT_MAP_LOADED;
    const std::string uid = "7GK45NEyMOif7S9jicImvQNz7gf";
    std::string l = r.line(e, uid, "A01 \"Race\"", "", "Stadium", 1794326400123LL);
    CHECK(l == "{\"v\":1,\"t\":1794326400123,\"ev\":\"map\",\"uid\":\"7GK45NEyMOif7S9jicImvQNz7gf\",\"name\":\"A01 \\\"Race\\\"\",\"env\":\"Stadium\"}\n");
    e.type = HORIZON_EVENT_RACE_START;
    CHECK(r.line(e, uid, "", "", "", 1) == "{\"v\":1,\"t\":1,\"ev\":\"start\",\"uid\":\"7GK45NEyMOif7S9jicImvQNz7gf\"}\n");
    e.type = HORIZON_EVENT_RESPAWN;
    e.respawns = -1; // unknown: counted here
    r.line(e, uid, "", "", "", 2);
    l = r.line(e, uid, "", "", "", 3);
    CHECK(l.find("\"ev\":\"respawn\"") != std::string::npos && l.find("\"respawns\":2}") != std::string::npos);
    e.type = HORIZON_EVENT_FINISH;
    e.time_ms = 45230;
    e.is_local_player = 1;
    e.is_pb = 1;
    l = r.line(e, uid, "", "", "", 4);
    CHECK(l == "{\"v\":1,\"t\":4,\"ev\":\"finish\",\"uid\":\"7GK45NEyMOif7S9jicImvQNz7gf\",\"time_ms\":45230,\"respawns\":2,\"local\":true,\"pb\":true,\"mode\":\"solo\"}\n");
    // the host's own respawn count wins when it has one; a restart resets it
    e.type = HORIZON_EVENT_RACE_START;
    r.line(e, uid, "", "", "", 5);
    e.type = HORIZON_EVENT_FINISH;
    e.respawns = 0;
    e.is_pb = 0;
    e.play_mode = HORIZON_MODE_NET;
    l = r.line(e, uid, "", "", "", 6);
    CHECK(l.find("\"respawns\":0,") != std::string::npos && l.find("\"pb\":false") != std::string::npos && l.find("\"mode\":\"net\"") != std::string::npos);
    // replays and editor test drives write nothing, nor do other events
    e.play_mode = HORIZON_MODE_REPLAY;
    CHECK(r.line(e, uid, "", "", "", 7).empty());
    e.play_mode = HORIZON_MODE_EDITOR_TEST;
    CHECK(r.line(e, uid, "", "", "", 7).empty());
    e.play_mode = HORIZON_MODE_SOLO;
    e.type = HORIZON_EVENT_CHECKPOINT;
    CHECK(r.line(e, uid, "", "", "", 7).empty());
    e.type = HORIZON_EVENT_FINISH;
    e.time_ms = 0;
    CHECK(r.line(e, uid, "", "", "", 7).empty());
    // no uid known: the line goes out without it (TM Horizon skips it)
    e.type = HORIZON_EVENT_RACE_START;
    CHECK(r.line(e, "", "", "", "", 8).find("uid") == std::string::npos);
    // file per month, appended
    const std::string f = stats::fileName(stats::nowMs());
    CHECK(f.size() == 13 && f[4] == '-' && f.substr(7) == ".jsonl");
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"horizon-stats-test\\nested";
    DeleteFileW((dir + L"\\t.jsonl").c_str());
    CHECK(stats::append(dir, "t.jsonl", "{\"a\":1}\n") && stats::append(dir, "t.jsonl", "{\"b\":2}\n"));
    std::string back;
    CHECK(readFile(dir + L"\\t.jsonl", back) && back == "{\"a\":1}\n{\"b\":2}\n");
    DeleteFileW((dir + L"\\t.jsonl").c_str());
    CHECK(!stats::append(L"", "t.jsonl", "x"));
}

// Exe identification: only the pinned builds run; TM Original only as the Online Patch v2 exe.
static void testExeIdentification() {
    const std::string tmo = "75e129d769f4595ddde54a8ef484162915bc2bf998c1a2139cc963294fe8175e";
    ExeMatch m = matchExe(tmo, 0x5d337477, 7883264, "TMORIGINAL.EXE");
    CHECK(m.exe && m.exe->game == HORIZON_GAME_ORIGINAL && m.why.empty());
    CHECK(std::string(bridge::siteCode(m.exe->game)) == "tmo");
    m = matchExe(tmo, 0x5d337477, 7883265, "TMORIGINAL.EXE"); // same hash, other size: refused
    CHECK(!m.exe && m.why.find("file size") != std::string::npos);
    m = matchExe(tmo, 0x5d337478, 7883264, "TMORIGINAL.EXE");
    CHECK(!m.exe && m.why.find("timestamp") != std::string::npos);
    // another TM Original exe (retail, patch v1, a re-patched copy): named as TM Original, refused
    m = matchExe(std::string(64, 'a'), 0x5d337477, 7883264, "TMORIGINAL.EXE");
    CHECK(!m.exe && m.why.find("TrackMania Original, but not the supported exe") == 0);
    m = matchExe(std::string(64, 'a'), 0x12345678, 1, "tmoriginal.exe");
    CHECK(!m.exe && m.why.find("TrackMania Original") == 0);
    m = matchExe(std::string(64, 'a'), 0x12345678, 1, "Other.exe");
    CHECK(!m.exe && m.why.find("unknown exe") == 0);
    // the other games still match, with and without a size check
    m = matchExe("983bd12724739dcb0f876aeb3a4259154b068940f5167f57ede750ca452d8068", 0x5ce7272f, 0, "TmNationsESWC.exe");
    CHECK(m.exe && m.exe->game == HORIZON_GAME_ESWC);
    m = matchExe("e59dfc7eaa5314e85b2b0df6f40ef7ba01a23bd5a9525f84407f5dff301df4fe", 0x5cc8f0cf, 7904768, "TmSunrise.exe");
    CHECK(m.exe && m.exe->game == HORIZON_GAME_SUNRISE);
    m = matchExe("8c5325d04a10d5e6921afb9931770279cca8cdfc7f66425b6365d0a870f5694a", 0x4d431494, 10685290, "TmForever.exe");
    CHECK(m.exe && m.exe->game == HORIZON_GAME_TMNF);
    size_t n = 0;
    const KnownExe* k = knownExes(&n);
    int originals = 0;
    for (size_t i = 0; i < n; i++)
        if (k[i].game == HORIZON_GAME_ORIGINAL) originals++;
    CHECK(originals == 1 && gameBitFromName("original") == HORIZON_GAME_ORIGINAL && std::string(gameKey(HORIZON_GAME_ORIGINAL)) == "original");
}

int main() {
    testProfile();
    testManifest();
    testHotkeys();
    testSettings();
    testJsonRoundTrip();
    testIds();
    testForeverObserve();
    testForeverTracker();
    testForeverTrackerEdges();
    testToasts();
    testBridgeLogic();
    testStatsLines();
    testExeIdentification();
    printf("unit tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
