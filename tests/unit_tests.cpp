// Unit tests for the host's pure logic: profile.json, plugin.json, hotkeys, settings, JSON writer.
#include <windows.h>

#include <cstdio>
#include <string>

#include "common/hotkeys.h"
#include "common/json.h"
#include "common/profile.h"
#include "common/settings.h"
#include "common/util.h"
#include "horizon_plugin.h"

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

int main() {
    testProfile();
    testManifest();
    testHotkeys();
    testSettings();
    testJsonRoundTrip();
    testIds();
    printf("unit tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
