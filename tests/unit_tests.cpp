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
    printf("unit tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
