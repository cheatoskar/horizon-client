// Unit tests for the host's pure logic: profile.json, plugin.json, hotkeys, settings, JSON writer.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include "common/hotkeys.h"
#include "common/json.h"
#include "common/modules.h"
#include "common/profile.h"
#include "common/settings.h"
#include "common/setup.h"
#include "common/util.h"
#include "horizon_plugin.h"
#include "host/game/forever_poll.h"
#include "host/bridge_logic.h"
#include "host/exe_known.h"
#include "gamectl/gamectl_pick.h"
#include "host/stats_log.h"
#include "host/toast_queue.h"
#include "../tests/abi12/abi12_sizes.h"

// ABI 1.3 layout: one minor for three branches' additions, appended after the 1.2 tables in a fixed order
// (open_page, notify, bridge_request, bridge_state; post_present at the end of the device callbacks).
// ABI12_* are the sizes the 1.2 test plugin (tests/abi12) is built with.
static_assert(sizeof(void*) == 4, "the plugin ABI is 32-bit x86");
static_assert(HORIZON_ABI == 1 && HORIZON_ABI_MINOR == 4, "ABI 1.4");
static_assert(offsetof(HorizonHostApi, host_flags) + sizeof(void*) == ABI12_HOST_API_SIZE, "1.2 table ends with host_flags");
static_assert(offsetof(HorizonHostApi, open_page) == ABI12_HOST_API_SIZE, "open_page is the first 1.3 field");
static_assert(offsetof(HorizonHostApi, notify) == ABI12_HOST_API_SIZE + 4, "notify follows open_page");
static_assert(offsetof(HorizonHostApi, bridge_request) == ABI12_HOST_API_SIZE + 8, "bridge_request follows notify");
static_assert(offsetof(HorizonHostApi, bridge_state) == ABI12_HOST_API_SIZE + 12, "bridge_state follows bridge_request");
static_assert(offsetof(HorizonHostApi, ui_metrics) == ABI12_HOST_API_SIZE + 16, "ui_metrics (1.4) follows the 1.3 table");
static_assert(sizeof(HorizonHostApi) == ABI12_HOST_API_SIZE + 20, "nothing else in 1.4");
static_assert(offsetof(HorizonUiMetrics, page_h) == 32 && sizeof(HorizonUiMetrics) == 36, "HorizonUiMetrics layout");
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
    CHECK(d.valid && d.soloOnly && !d.enabledByDefault && d.dll == "x.dll" && !d.startupOnly);
    CHECK(parsePluginManifest(R"({"startup_only":true})", "x").startupOnly);
    CHECK(parsePluginManifest(R"({"icon":"icon.png"})", "x").icon == "icon.png");
    CHECK(parsePluginManifest(R"({"icon":"..\\x.png"})", "x").icon.empty()); // icon must stay in the folder
    CHECK(parsePluginManifest(R"({"icon":"sub/x.png"})", "x").icon.empty());
    // "ui": the size of the module's own window (logical px), defaults 480x320 / 820x560
    PluginManifest ui0 = parsePluginManifest(R"({})", "x");
    CHECK(ui0.uiMinW == 480.0f && ui0.uiMinH == 320.0f && ui0.uiPrefW == 820.0f && ui0.uiPrefH == 560.0f);
    PluginManifest ui1 = parsePluginManifest(R"({"ui":{"min_w":520,"min_h":360,"pref_w":860,"pref_h":600}})", "x");
    CHECK(ui1.valid && ui1.uiMinW == 520.0f && ui1.uiMinH == 360.0f && ui1.uiPrefW == 860.0f && ui1.uiPrefH == 600.0f);
    PluginManifest ui2 = parsePluginManifest(R"({"ui":{"min_w":700,"pref_w":600,"min_h":"x","pref_h":99999}})", "x");
    CHECK(ui2.valid && ui2.uiMinW == 700.0f && ui2.uiPrefW == 700.0f && ui2.uiMinH == 320.0f && ui2.uiPrefH == 560.0f);
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

// The shared setup: which keys stay per game, copy/replace, merging saves of two games, export/import, anchors.
static bool autosaveOnly(const std::string& id, void*) { return id == "eswc-autosave"; }
static bool localPick(const std::string& key, void*) { return setup::isLocalHostKey(key, &autosaveOnly, nullptr); }
static bool sharedPick(const std::string& key, void*) { return !setup::isLocalHostKey(key, &autosaveOnly, nullptr); }

static void testSharedSetup() {
    // key classes
    CHECK(setup::pluginOfKey("plugin.wr-hunter.enabled") == "wr-hunter");
    CHECK(setup::pluginOfKey("panel.scripts.w-clock-clock-d7a888") == "scripts");
    CHECK(setup::pluginOfKey("hotkey.horizon.menu") == "horizon");
    CHECK(setup::pluginOfKey("ui.content_scale").empty() && setup::pluginOfKey("plugin.").empty());
    CHECK(setup::pluginGameSpecific(HORIZON_GAME_LEGACY));
    CHECK(setup::pluginGameSpecific(HORIZON_GAME_ESWC));
    CHECK(!setup::pluginGameSpecific(HORIZON_GAME_ALL));
    CHECK(!setup::pluginGameSpecific(0));
    CHECK(localPick("bridge.key", nullptr) && localPick("bridge.instance", nullptr) && localPick("bridge.enabled", nullptr));
    CHECK(localPick("setup.shared", nullptr) && localPick("OnlineFix", nullptr) && localPick("pwin.tmx-browser", nullptr));
    CHECK(localPick("plugin.eswc-autosave.enabled", nullptr) && localPick("panel.eswc-autosave.status", nullptr));
    CHECK(!localPick("plugin.scripts.enabled", nullptr) && !localPick("panel.scripts.w-clock-clock-d7a888", nullptr));
    CHECK(!localPick("ui.content_scale", nullptr) && !localPick("widgets.opacity", nullptr) && !localPick("hotkey.horizon.menu", nullptr));
    CHECK(!localPick("toasts.dnd", nullptr) && !localPick("bar", nullptr));
    CHECK(!setup::isLocalHostKey("plugin.eswc-autosave.enabled", nullptr, nullptr)); // no plugin list: shared
    CHECK(!setup::isExportableHostKey("bridge.key") && !setup::isExportableHostKey("setup.shared"));
    CHECK(setup::isExportableHostKey("OnlineFix") && setup::isExportableHostKey("panel.scripts.w-x"));
    // new install shared, existing install its own
    CHECK(setup::sharedDefault(false) == 1 && setup::sharedDefault(true) == 0);

    // "Copy from this game": the shared host.json gets this game's shared keys, nothing per game
    SettingsFile game, shared;
    CHECK(game.loadText(R"({"bridge.key":"secret","bridge.instance":"abc","setup.shared":1,"OnlineFix":0,"ui.content_scale":110,
        "panel.scripts.w-clock-clock-d7a888":"1,1,0,0,1,100,1110,18,0,0,0,1.00","plugin.eswc-autosave.enabled":1,"plugin.wr-hunter.enabled":1})"));
    CHECK(shared.loadText(R"({"ui.content_scale":90,"widgets.opacity":40,"bridge.key":"stray"})"));
    setup::copyKeys(game, shared, &sharedPick, nullptr, true);
    CHECK(shared.getInt("ui.content_scale", 0) == 110);
    CHECK(!shared.has("widgets.opacity")); // replaced: the game had none
    CHECK(shared.has("bridge.key"));        // not a shared key: left alone by the copy
    CHECK(shared.getInt("plugin.wr-hunter.enabled", 0) == 1 && !shared.has("plugin.eswc-autosave.enabled"));
    CHECK(shared.getString("panel.scripts.w-clock-clock-d7a888", "") == "1,1,0,0,1,100,1110,18,0,0,0,1.00");
    CHECK(!shared.has("OnlineFix") && !shared.has("setup.shared") && !shared.has("bridge.instance"));
    // values keep their type
    SettingsFile typed;
    typed.copyKey(game, "ui.content_scale");
    CHECK(typed.toText().find("\"ui.content_scale\": 110") != std::string::npos);
    typed.copyKey(game, "missing");
    CHECK(!typed.has("missing"));
    // the merged view a game reads: shared keys from the shared file, the rest from its own
    SettingsFile merged;
    setup::copyKeys(game, merged, &localPick, nullptr, false);
    setup::copyKeys(shared, merged, &sharedPick, nullptr, false);
    CHECK(merged.getString("bridge.key", "") == "secret" && merged.getInt("ui.content_scale", 0) == 110 && merged.getInt("OnlineFix", 1) == 0);

    // merging saves: two games write the same shared file, neither loses the other's change
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"horizon_unit_test\\Horizon Client\\shared\\host.json";
    DeleteFileW(path.c_str());
    SettingsFile a, b;
    a.setMerging(true);
    b.setMerging(true);
    CHECK(a.load(path) && b.load(path));
    a.setNumber("ui.content_scale", 105);
    CHECK(a.saveIfDirty());
    b.setNumber("widgets.opacity", 50);
    CHECK(b.saveIfDirty());
    CHECK(b.getInt("ui.content_scale", 0) == 105); // b picked up a's key while saving
    a.remove("ui.content_scale");
    a.setString("hotkey.horizon.menu", "F2");
    CHECK(a.saveIfDirty());
    SettingsFile c;
    CHECK(c.load(path));
    CHECK(!c.has("ui.content_scale") && c.getInt("widgets.opacity", 0) == 50 && c.getString("hotkey.horizon.menu", "") == "F2");
    // Reset: clear() drops what is on disk too
    a.clear();
    a.setNumber("bar", 0);
    CHECK(a.saveIfDirty());
    CHECK(c.load(path) && c.keys().size() == 1 && c.getInt("bar", 1) == 0);
    // a normal (not merging) file still writes exactly what it holds
    SettingsFile plain;
    CHECK(plain.load(path));
    plain.setNumber("x", 1);
    CHECK(plain.saveIfDirty() && c.load(path) && c.keys().size() == 2);

    // export / import
    std::vector<setup::PluginSettings> ps = {{"scripts", R"({"enabled.clock":1,"clock.hideAfter":"0"})"}, {"wr-hunter", "{}"}, {"Bad Id", R"({"a":1})"}};
    const std::string file = setup::exportText(R"({"ui.content_scale":110,"bridge.key":"secret","nested":{"x":1}})", ps, "TrackMania Nations ESWC", "0.3.1");
    setup::Imported im;
    std::string err;
    CHECK(setup::importText(file, im, err));
    CHECK(im.game == "TrackMania Nations ESWC" && im.hostVersion == "0.3.1");
    SettingsFile ih;
    CHECK(ih.loadText(im.hostJson));
    CHECK(ih.getInt("ui.content_scale", 0) == 110 && !ih.has("bridge.key") && !ih.has("nested"));
    CHECK(im.plugins.size() == 1 && im.plugins[0].id == "scripts"); // empty and bad ids left out
    SettingsFile is;
    CHECK(is.loadText(im.plugins[0].json) && is.getInt("enabled.clock", 0) == 1 && is.getString("clock.hideAfter", "") == "0");
    CHECK(!setup::importText(R"({"format":"something-else","version":1})", im, err) && !err.empty());
    CHECK(!setup::importText(R"({"format":"horizon-setup","version":2})", im, err) && err.find("newer") != std::string::npos);
    CHECK(!setup::importText("not json", im, err));
    CHECK(setup::importText(R"({"format":"horizon-setup","version":1})", im, err) && im.plugins.empty());

    // anchors: a widget keeps its edge at any window size
    float x = 0, y = 0;
    setup::Anchor tr = setup::anchorOf(1110, 18, 152, 60, 1280, 960); // top right at 1280x960
    CHECK(tr.valid && tr.ax == 1.0f && tr.ay == 0.0f);
    setup::placeAt(tr, 152, 60, 1280, 960, x, y);
    CHECK(x == 1110.0f && y == 18.0f);
    setup::placeAt(tr, 152, 60, 1920, 1080, x, y);
    CHECK(std::fabs((1920.0f - x - 152.0f) - 18.0f * 1920.0f / 1280.0f) < 1.0f && std::fabs(y - 18.0f * 1080.0f / 960.0f) < 1.0f);
    setup::placeAt(tr, 152, 60, 800, 600, x, y); // smaller window: still at the right edge, on screen
    CHECK(x + 152.0f <= 800.0f && x > 600.0f && y >= 0.0f);
    setup::Anchor bl = setup::anchorOf(10, 900, 200, 50, 1280, 960);
    CHECK(bl.ax == 0.0f && bl.ay == 1.0f);
    setup::placeAt(bl, 200, 50, 2560, 1440, x, y);
    CHECK(std::fabs(x - 20.0f) < 1.0f && std::fabs((1440.0f - y - 50.0f) - 10.0f * 1440.0f / 960.0f) < 1.0f);
    setup::Anchor mid = setup::anchorOf(565, 400, 150, 100, 1280, 960);
    CHECK(mid.ax == 0.5f && mid.ay == 0.5f);
    setup::placeAt(mid, 150, 100, 1280, 960, x, y);
    CHECK(x == 565.0f && y == 400.0f);
    CHECK(!setup::anchorOf(-1, -1, 10, 10, 1280, 960).valid && !setup::anchorOf(10, 10, 10, 10, 0, 0).valid);
}

// modules.json: groups and first-start defaults; plugin.json "enabled_by_default": "paired".
static void testModulesConfig() {
    std::string text;
    CHECK(readFile(L"packaging\\modules.json", text) || readFile(L"..\\packaging\\modules.json", text));
    ModulesConfig c = parseModulesConfig(text);
    CHECK(c.valid);
    const ModuleGroup* le = c.group("legacy-essentials");
    CHECK(le && le->name == "Legacy Essentials" && le->icon == "icons/legacy-essentials.png");
    CHECK(le && le->members.size() == 4 && le->members[2].host && le->members[2].id == "login-fix" && le->members[0].label == "Autosave");
    CHECK(c.groupOf("controller-hotplug") == le && c.groupOf("game-settings") == le && !c.groupOf("wr-hunter") && !c.groupOf("login-fix"));
    CHECK(c.defaultFor("legacy-randomizer") == 0 && c.defaultFor("wr-hunter") == -1);
    bool prevScripts = false;
    for (const auto& d : c.previousDefaults) prevScripts = prevScripts || (d.first == "scripts" && d.second);
    CHECK(prevScripts);
    // a module in two groups stays in the first; bad ids and empty groups are dropped; newer format refused
    ModulesConfig two = parseModulesConfig(R"({"format":1,"groups":[{"id":"a","name":"A","members":[{"id":"x"}]},
        {"id":"b","name":"B","members":[{"id":"x"},{"id":"Bad Id"}]},{"id":"c","name":"C","icon":"../x.png","members":[{"id":"y"}]}]})");
    CHECK(two.valid && two.groups.size() == 1 && two.groupOf("x") && two.groupOf("x")->id == "a");
    CHECK(!parseModulesConfig(R"({"format":2})").valid && !parseModulesConfig("nope").valid);
    // "paired": off until this game is paired with TM Horizon
    PluginManifest up = parsePluginManifest(R"({"id":"tmx-upload","enabled_by_default":"paired"})", "tmx-upload");
    CHECK(up.valid && !up.enabledByDefault && up.defaultWhenPaired);
    PluginManifest on = parsePluginManifest(R"({"id":"x"})", "x");
    CHECK(on.enabledByDefault && !on.defaultWhenPaired);
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
    // play_map: extra fields, kept without a title, other games ignored
    ev.clear();
    CHECK(bridge::parseEvents(R"({"ok":true,"last":9,"events":[{"id":8,"kind":"play_map","level":"info","title":"A01-Race","text":"",
                                  "at":1794000000000,"game":"tmn","path":"Challenges/Downloaded/12345.Challenge.Gbx"},
                                  {"id":9,"kind":"play_map","title":"","game":"tms","path":"x.Challenge.Gbx"}]})", &ev, &last));
    CHECK(ev.size() == 2 && last == 9);
    CHECK(ev[0].game == "tmn" && ev[0].path == "Challenges/Downloaded/12345.Challenge.Gbx" && ev[0].at == 1794000000000ll && ev[0].title == "A01-Race");
    CHECK(bridge::playMapFor(ev[0], "tmn") && !bridge::playMapFor(ev[0], "tms") && !bridge::playMapFor(ev[0], ""));
    CHECK(bridge::playMapFor(ev[1], "tms") && !bridge::playMapFor(ev[1], "tmnf"));
    bridge::Event up;
    up.kind = "upload";
    up.game = "tmn";
    CHECK(!bridge::playMapFor(up, "tmn"));
    CHECK(bridge::playMapGame(HORIZON_GAME_ESWC) && bridge::playMapGame(HORIZON_GAME_SUNRISE) && bridge::playMapGame(HORIZON_GAME_ORIGINAL));
    CHECK(!bridge::playMapGame(HORIZON_GAME_TMNF) && !bridge::playMapGame(HORIZON_GAME_TMUF));
    CHECK(bridge::playMapFresh(0, 5000000) && bridge::playMapFresh(4000000, 4100000) && !bridge::playMapFresh(1000000, 1000000 + 120001));
    CHECK(bridge::eventsPath(false, -1, true) == "/v1/client/events?caps=play_map");
    CHECK(bridge::eventsPath(true, 12, true) == "/v1/client/events?since=12&wait=20&caps=play_map");
    CHECK(bridge::eventsPath(true, 12, false) == "/v1/client/events?since=12&wait=20" && bridge::eventsPath(false, 0, false) == "/v1/client/events");
    // map paths: relative to Tracks, '/' or '\', .Challenge.Gbx in any case
    CHECK(bridge::mapRelPath("Challenges/Downloaded/12345.Challenge.Gbx") == "Challenges\\Downloaded\\12345.Challenge.Gbx");
    CHECK(bridge::mapRelPath("Challenges\\Downloaded\\A01 Race (1).challenge.gbx") == "Challenges\\Downloaded\\A01 Race (1).challenge.gbx");
    CHECK(bridge::mapRelPath("x.CHALLENGE.GBX") == "x.CHALLENGE.GBX");
    CHECK(bridge::mapRelPath("Challenges/Downloaded/\xc3\xa9t\xc3\xa9.Challenge.Gbx") != ""); // UTF-8 names
    CHECK(bridge::mapRelPath("") == "" && bridge::mapRelPath(".Challenge.Gbx") == "" && bridge::mapRelPath("a/.Challenge.Gbx") == "");
    CHECK(bridge::mapRelPath("../x.Challenge.Gbx") == "" && bridge::mapRelPath("a/../../x.Challenge.Gbx") == "" && bridge::mapRelPath("a\\..\\x.Challenge.Gbx") == "");
    CHECK(bridge::mapRelPath("./x.Challenge.Gbx") == "" && bridge::mapRelPath("a//x.Challenge.Gbx") == "" && bridge::mapRelPath("a./x.Challenge.Gbx") == "");
    CHECK(bridge::mapRelPath("C:/x.Challenge.Gbx") == "" && bridge::mapRelPath("C:x.Challenge.Gbx") == "" && bridge::mapRelPath("/x.Challenge.Gbx") == "");
    CHECK(bridge::mapRelPath("\\x.Challenge.Gbx") == "" && bridge::mapRelPath("\\\\server\\share\\x.Challenge.Gbx") == "" && bridge::mapRelPath("//server/x.Challenge.Gbx") == "");
    CHECK(bridge::mapRelPath("x.Challenge.Gbx:stream") == "" && bridge::mapRelPath("x.Challenge.Gbx::$DATA") == "");
    CHECK(bridge::mapRelPath("x.Replay.Gbx") == "" && bridge::mapRelPath("x.Challenge.Gbx.exe") == "" && bridge::mapRelPath("x.Challenge.Gbx ") == "");
    CHECK(bridge::mapRelPath("a\x01/x.Challenge.Gbx") == "" && bridge::mapRelPath("a?/x.Challenge.Gbx") == "" && bridge::mapRelPath("a\"b.Challenge.Gbx") == "");
    CHECK(bridge::mapRelPath("CON.Challenge.Gbx") == "" && bridge::mapRelPath("nul/x.Challenge.Gbx") == "" && bridge::mapRelPath("Com1.Challenge.Gbx") == "");
    CHECK(bridge::mapRelPath("Console.Challenge.Gbx") != "" && bridge::mapRelPath("COM10.Challenge.Gbx") != "");
    CHECK(bridge::mapRelPath(std::string(390, 'a') + ".Challenge.Gbx") == "");
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

// play_map_file: the copied map is found among the infos the rescan added by its gold time, never by counting.
// TM Sunrise (owner's log): "challenge infos 207 -> 208, 2 new", the old code refused with "expected 1, got 2".
static void testPickNewChallenge() {
    int objs[16] = {};
    std::map<void*, int> gold;
    auto P = [&](int i) { return static_cast<void*>(&objs[i]); };
    std::vector<void*> before = {P(0), P(1), P(2), P(3)};
    for (int i = 0; i < 16; i++) gold[P(i)] = 30000 + i * 1000;
    gold[P(9)] = 44000; // the map just copied in
    auto goldOf = [&](void* info, int* ms) {
        auto it = gold.find(info);
        if (it == gold.end()) return false;
        *ms = it->second;
        return true;
    };
    int fresh = 0;
    // 4 -> 5 infos, but two pointers are new (the game rebuilt the info of another file): pick by gold time
    std::vector<void*> after = {P(0), P(8), P(2), P(3), P(9)};
    CHECK(gamectl::pickNewChallenge(before, after, goldOf, 44000, &fresh) == 4 && fresh == 2);
    // sorted by name: the new one can be anywhere
    after = {P(9), P(0), P(1), P(8), P(2), P(3), P(10), P(11)};
    CHECK(gamectl::pickNewChallenge(before, after, goldOf, 44000, &fresh) == 0 && fresh == 4);
    // one new info and the gold time does not match (offset unknown): the single new one
    after = {P(0), P(1), P(2), P(3), P(12)};
    CHECK(gamectl::pickNewChallenge(before, after, goldOf, 44000, &fresh) == 4 && fresh == 1);
    // several new, none matches: refused
    after = {P(0), P(12), P(13)};
    CHECK(gamectl::pickNewChallenge(before, after, goldOf, 44000, &fresh) == gamectl::kPickAmbiguous);
    // map without a gold time: only a single new info counts
    CHECK(gamectl::pickNewChallenge(before, {P(0), P(1), P(2), P(3), P(14)}, goldOf, 0, &fresh) == 4);
    CHECK(gamectl::pickNewChallenge(before, {P(0), P(14), P(15)}, goldOf, 0, &fresh) == gamectl::kPickAmbiguous);
    // nothing new: the game did not list the copy
    CHECK(gamectl::pickNewChallenge(before, before, goldOf, 44000, &fresh) == gamectl::kPickNoNew && fresh == 0);

    // By file name (what play_map_file uses): the list after a rescan, as the game leaves it.
    std::map<void*, std::string> name;
    auto nameOf = [&](void* info, char* buf, size_t cap) {
        auto it = name.find(info);
        if (it == name.end() || it->second.size() >= cap) return false;
        memcpy(buf, it->second.c_str(), it->second.size() + 1);
        return true;
    };
    const char* want = "HZ_0006124000.Challenge.Gbx";
    for (int i = 0; i < 16; i++) name[P(i)] = "Map" + std::to_string(i) + ".Challenge.Gbx";
    name[P(5)] = "A01-Race(00'59''12).Replay.Gbx"; // replay infos (never in the list, but must never match)
    name[P(6)] = "HZ_0006124000.Replay.Gbx";
    gold[P(5)] = gold[P(6)] = 44000;
    name.erase(P(7)); // an info without a file (Kind 7)
    gamectl::PickReport rep;
    // several new challenge infos (rebuilt ones with the same gold time too), replays around: the file name decides
    gold[P(10)] = gold[P(11)] = 44000;
    name[P(11)] = want;
    after = {P(0), P(5), P(10), P(1), P(6), P(7), P(12), P(11), P(2), P(3)};
    CHECK(gamectl::pickCopiedChallenge(before, after, nameOf, want, goldOf, 44000, &rep) == 7 &&
          rep.method == gamectl::PickMethod::FileName && rep.matches == 1 && rep.named == 9 && rep.newCount == 6);
    // the copy's info at an old address (freed and reused, or an old info of the same UID reused by Build)
    name[P(11)] = "Map11.Challenge.Gbx";
    name[P(2)] = "c:\\games\\tmn\\GameData\\Tracks\\Challenges\\Downloaded\\_Horizon\\hz_0006124000.challenge.gbx";
    CHECK(gamectl::pickCopiedChallenge(before, after, nameOf, want, goldOf, 44000, &rep) == 8 && rep.matches == 1);
    // names readable but none is the copy: not listed, no guessing by gold time
    name[P(2)] = "Map2.Challenge.Gbx";
    CHECK(gamectl::pickCopiedChallenge(before, after, nameOf, want, goldOf, 44000, &rep) == gamectl::kPickNoNew &&
          rep.method == gamectl::PickMethod::FileName);
    // a name that only ends like the copy's is another file
    name[P(12)] = "XHZ_0006124000.Challenge.Gbx";
    CHECK(gamectl::pickCopiedChallenge(before, after, nameOf, want, goldOf, 44000, &rep) == gamectl::kPickNoNew);
    // two infos with the copy's name: refused
    name[P(12)] = want;
    name[P(10)] = want;
    CHECK(gamectl::pickCopiedChallenge(before, after, nameOf, want, goldOf, 44000, &rep) == gamectl::kPickAmbiguous &&
          rep.matches == 2);
    // no file name readable at all (unknown layout): the gold time among the new pointers, as before
    auto noName = [](void*, char*, size_t) { return false; };
    after = {P(0), P(10), P(1), P(9), P(2), P(3)};
    gold[P(10)] = 31000;
    CHECK(gamectl::pickCopiedChallenge(before, after, noName, want, goldOf, 44000, &rep) == 3 &&
          rep.method == gamectl::PickMethod::GoldTime && rep.named == 0);
    CHECK(gamectl::sameFileName("HZ_1.Challenge.Gbx", "hz_1.challenge.gbx") && gamectl::sameFileName("a/HZ_1.Challenge.Gbx", "HZ_1.Challenge.Gbx") &&
          !gamectl::sameFileName("HZ_1.Challenge.Gbx.bak", "HZ_1.Challenge.Gbx") && !gamectl::sameFileName("", "x"));
}

int main() {
    testProfile();
    testManifest();
    testHotkeys();
    testSettings();
    testSharedSetup();
    testModulesConfig();
    testJsonRoundTrip();
    testIds();
    testForeverObserve();
    testForeverTracker();
    testForeverTrackerEdges();
    testToasts();
    testBridgeLogic();
    testStatsLines();
    testExeIdentification();
    testPickNewChallenge();
    printf("unit tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
