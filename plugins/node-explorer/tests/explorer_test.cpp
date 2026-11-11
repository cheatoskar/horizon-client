// Node Explorer tests, no game needed:
//   1. unit tests: type table, value formatting, path builder/parser, history and pins
//   2. browsing a fake engine object graph (fake_graph.cpp) with the plugin's own catalog/inspector code
//   3. the plugin DLL inside a small fake host whose engine pointers lead into the same fake graph
//
//   explorer_test.exe [plugin dll]    (without a dll, part 3 is skipped)
// Exit code 0 = all checks passed.
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <string>

#include "imgui.h"
#include "horizon_plugin.h"

#include "catalog.h"
#include "fake_graph.h"
#include "path.h"
#include "reflect/reflect.h"
#include "safe_read.h"
#include "stub_host.h"
#include "values.h"

using namespace nx;

namespace {

int g_fail = 0, g_pass = 0;

void check(bool ok, const char* what) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s\n", what);
    }
}
void checkEq(const std::string& got, const std::string& want, const char* what) {
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s: got [%s] want [%s]\n", what, got.c_str(), want.c_str());
    }
}

// ---------------------------------------------------------------- 1. units ----------------------

void testTypes() {
    printf("types\n");
    auto is = [](uint32_t t, Engine e, Kind k, Shape s) {
        TypeInfo ti = classify(t, e);
        return ti.kind == k && ti.shape == s;
    };
    const Engine L = Engine::Legacy, F = Engine::Forever;
    // examples from tm-modding-knowledge data/reflection/*_classes.json
    check(is(0, L, Kind::Proc, Shape::Single), "ESWC 0 method (CGameApp.Start)");
    check(is(1, F, Kind::Bool, Shape::Single), "TMNF 1 bool (CSystemKeyboard.IsDownP)");
    check(is(5, L, Kind::Nod, Shape::Single), "ESWC 5 nod (CTrackMania.Switcher)");
    check(is(7, F, Kind::Nod, Shape::BufferCat), "TMNF 7 nod list (CGameCtnApp.PlayerProfiles)");
    check(is(9, L, Kind::Color, Shape::Single), "ESWC 9 color (CHmsCamera.ClearColor)");
    check(is(13, F, Kind::Enum, Shape::Single), "TMNF 13 enum");
    check(is(14, L, Kind::Int, Shape::Single), "ESWC 14 int (CMwCmdScriptVarInt.Int)");
    check(is(19, F, Kind::Iso4, Shape::Single), "TMNF 19 iso4 (CTrackManiaPlayerInfo.VehicleSpawnLoc)");
    check(is(27, L, Kind::Natural, Shape::Single), "ESWC 27 uint (CGameApp.NbPlayers)");
    check(is(27, F, Kind::Id, Shape::Single), "TMNF 27 id (CGameCtnGhost.PlayerMobilId)");
    check(is(29, F, Kind::Id, Shape::BufferCat), "TMNF 29 id list (CBoatTeamDesc.TeamMateIds)");
    check(is(31, L, Kind::Natural, Shape::Range), "ESWC 31 uint range (CPlugSound.Priority)");
    check(is(31, F, Kind::Natural, Shape::Single), "TMNF 31 uint (CGameCtnApp.VertexCount)");
    check(is(32, L, Kind::Real, Shape::Single), "ESWC 32 float (CTrackMania.MasterJumpFactor)");
    check(is(36, F, Kind::Real, Shape::Single), "TMNF 36 float (CTrackMania.PillarOffset)");
    check(is(36, L, Kind::Real, Shape::Range), "ESWC 36 float range (CSceneToyVehicleMaterial.Grip)");
    check(is(40, F, Kind::Real, Shape::Range), "TMNF 40 float range");
    check(is(37, L, Kind::String, Shape::Single), "ESWC 37 string");
    check(is(41, F, Kind::String, Shape::Single), "TMNF 41 string (CNetMasterServer.Login)");
    check(is(41, L, Kind::WString, Shape::Single), "ESWC 41 wstring (CGamePlayer.Name)");
    check(is(45, F, Kind::WString, Shape::Single), "TMNF 45 wstring (CGameHighScore.Name)");
    check(is(43, L, Kind::WString, Shape::BufferCat), "ESWC 43 wstring list (CGameNetwork.ChatHistoryText)");
    check(is(45, L, Kind::Vec2, Shape::Single), "ESWC 45 vec2 (CControlBase.BoxMax)");
    check(is(49, L, Kind::Vec3, Shape::Single), "ESWC 49 vec3 (CPlugVisual.BBoxCenter)");
    check(is(53, F, Kind::Vec3, Shape::Single), "TMNF 53 vec3");
    check(is(53, L, Kind::Vec4, Shape::Single), "ESWC 53 vec4 (TransfoMat4U)");
    check(is(57, F, Kind::Vec4, Shape::Single), "TMNF 57 vec4 (PlaneEq)");
    check(is(61, L, Kind::ProcEx, Shape::Single), "ESWC 61 method with args (CGameApp.ShowMenu)");
    check(is(65, F, Kind::ProcEx, Shape::Single), "TMNF 65 method with args");
    check(is(200, F, Kind::Unknown, Shape::Single), "200 unknown");
    checkEq(typeLabel(classify(7, F), "CGameCtnBlock"), "CGameCtnBlock list", "label nod list");
    checkEq(typeLabel(classify(36, F), nullptr), "float", "label float");
    checkEq(typeLabel(classify(40, F), nullptr), "float range", "label range");
    checkEq(typeLabel(classify(56, F), nullptr), "vec3 array", "label array");
}

void testFormat() {
    printf("formatting\n");
    checkEq(fmtFloat(1.5f), "1.5", "float 1.5");
    checkEq(fmtFloat(0.0f), "0", "float 0");
    checkEq(fmtFloat(-0.0f), "0", "float -0");
    checkEq(fmtFloat(0.1f), "0.1", "float 0.1");
    checkEq(fmtFloat(-220.5f), "-220.5", "float -220.5");
    checkEq(fmtFloat(1e7f), "1e+07", "float 1e7");
    float nanv;
    const uint32_t nanBits = 0x7fc00000u;
    memcpy(&nanv, &nanBits, 4);
    checkEq(fmtFloat(nanv), "nan", "float nan");
    float infv;
    const uint32_t infBits = 0xff800000u;
    memcpy(&infv, &infBits, 4);
    checkEq(fmtFloat(infv), "-inf", "float -inf");
    const float v3[] = {1.0f, 2.5f, -3.0f};
    checkEq(fmtFloats(v3, 3), "1, 2.5, -3", "floats");
    checkEq(fmtBool(0), "false", "bool 0");
    checkEq(fmtBool(1), "true", "bool 1");
    checkEq(fmtBool(7), "7 (bool?)", "bool 7");
    checkEq(fmtInt(-1), "-1", "int");
    checkEq(fmtUint(4294967295u), "4294967295", "uint");
    checkEq(fmtHex(42), "0x0000002A", "hex");
    checkEq(fmtPtr(0), "null", "ptr null");
    checkEq(fmtText("ab", 2, 10), "\"ab\"", "text");
    checkEq(fmtText("a\nb\"c\\", 6, 10), "\"a\\nb\\\"c\\\\\"", "text escapes");
    checkEq(fmtText("a\x01z", 3, 10), "\"a\\x01z\"", "text control char");
    checkEq(fmtText("abcdef", 6, 3), "\"abc\"...", "text cut");
    checkEq(fmtText("\xc3\xa4", 2, 10), "\"\xc3\xa4\"", "text utf-8 passes");
    const wchar_t w[] = L"ä\U0001F3C1x";
    checkEq(fmtWText(w, wcslen(w), 10), "\"\xc3\xa4\xf0\x9f\x8f\x81x\"", "wtext umlaut + surrogate pair");
    const wchar_t lone[] = {0xd800, L'a', 0};
    checkEq(fmtWText(lone, 2, 10), "\"?a\"", "wtext lone surrogate");
    float iso[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 5, 6, 7};
    checkEq(fmtValue(Kind::Iso4, iso, sizeof(iso)), "pos 5, 6, 7", "iso4 short");
    checkEq(fmtValueLong(Kind::Iso4, iso, sizeof(iso)), "x 1, 0, 0\ny 0, 1, 0\nz 0, 0, 1\npos 5, 6, 7", "iso4 long");
    const float col[] = {1.0f, 0.5f, 0.25f};
    checkEq(fmtValue(Kind::Color, col, sizeof(col)), "1, 0.5, 0.25", "color");
    const int32_t i3[] = {32, -1, 7};
    checkEq(fmtValue(Kind::Int3, i3, sizeof(i3)), "32, -1, 7", "int3");
    const uint32_t id = 0x40000003u;
    checkEq(fmtValue(Kind::Id, &id, 4), "0x40000003", "id");
    checkEq(fmtValue(Kind::Real, &id, 2), "?", "too few bytes");
    checkEq(fmtValue(Kind::Proc, &id, 4), "?", "method has no value");
}

void testPath() {
    printf("path builder\n");
    Path p;
    p.root = "App";
    checkEq(p.text(), "App", "root only");
    Path q = p.child("CurrentPlayground", -1, 0x1000).child("GameTerminals", 0, 0x2000);
    checkEq(q.text(), "App.CurrentPlayground.GameTerminals[0]", "path text (task example)");
    checkEq(q.text(1), "App.CurrentPlayground", "text depth 1");
    checkEq(q.text(0), "App", "text depth 0");
    checkEq(q.crumb(-1), "App", "crumb root");
    checkEq(q.crumb(1), "GameTerminals[0]", "crumb item");
    checkEq(q.crumb(5), "", "crumb out of range");
    check(q.object() == 0x2000, "object = last step");
    check(q.prefix(1).object() == 0x1000 && q.prefix(1).steps.size() == 1, "prefix");
    check(q.prefix(9).sameAs(q), "prefix longer than path");
    Path r;
    check(parsePath("App.CurrentPlayground.GameTerminals[0]", &r) && r.sameAs(q), "parse round trip");
    check(parsePath("Race.Players[12].Mobil", &r) && r.root == "Race" && r.steps.size() == 2 && r.steps[0].index == 12, "parse index");
    check(parsePath("Challenge", &r) && r.steps.empty(), "parse root only");
    check(!parsePath("", &r), "parse empty");
    check(!parsePath("Foo.Bar", &r), "parse unknown root");
    check(!parsePath("App.", &r), "parse trailing dot");
    check(!parsePath("App..X", &r), "parse double dot");
    check(!parsePath("App.X[", &r), "parse open bracket");
    check(!parsePath("App.X[a]", &r), "parse bad index");
    check(!parsePath("App.X[]", &r), "parse empty index");
    check(!parsePath("App X", &r), "parse space");
    check(rootEngineId("App") == HORIZON_ENGINE_APP && rootEngineId("Race") == HORIZON_ENGINE_RACE &&
              rootEngineId("Challenge") == HORIZON_ENGINE_CHALLENGE && rootEngineId("Map") == 0,
          "root ids match the host's engine pointers");

    printf("history and pins\n");
    Navigator n;
    check(!n.current() && !n.canBack() && !n.canForward(), "empty navigator");
    Path a, b, c;
    parsePath("App", &a);
    parsePath("App.CurrentPlayground", &b);
    parsePath("Race", &c);
    n.open(a);
    n.open(b);
    n.open(b); // same path: no new entry
    check(n.historySize() == 2, "same path is not added twice");
    n.open(c);
    check(n.back() && n.current()->sameAs(b), "back");
    check(n.back() && n.current()->sameAs(a) && !n.canBack(), "back to start");
    check(n.forward() && n.current()->sameAs(b), "forward");
    n.open(c.child("Players", 1, 0));
    check(!n.canForward() && n.historySize() == 3, "opening drops the forward entries");
    for (int i = 0; i < 100; i++) n.open(a.child("X", i, 0));
    check(n.historySize() == Navigator::kMaxHistory, "history is capped");
    check(n.pin(b) && !n.pin(b) && n.pin(c), "pin, no duplicate");
    check(n.isPinned(b) && n.pins().size() == 2, "pinned");
    checkEq(n.pinsText(), "App.CurrentPlayground\nRace", "pins as text");
    Navigator m;
    m.setPinsText("Race\n\nbad path\nChallenge.Blocks[3]");
    check(m.pins().size() == 2 && m.pins()[1].text() == "Challenge.Blocks[3]", "pins from text skip bad lines");
    n.unpin(0);
    check(n.pins().size() == 1 && n.pins()[0].sameAs(c), "unpin");
}

// ---------------------------------------------------------------- 2. fake graph -----------------

const Row* rowOf(const ObjectView& v, const char* name) {
    for (const Row& r : v.rows)
        if (r.m->name == name) return &r;
    return nullptr;
}

std::string valueOf(const ObjectView& v, const char* name) {
    const Row* r = rowOf(v, name);
    return r ? r->value : "<missing>";
}

void browseGraph(bool forever) {
    printf("fake graph, %s type numbers\n", forever ? "Forever" : "ESWC/Sunrise");
    const fake::Graph& g = fake::build(forever);
    check(reflect::init(0) && reflect::ready(), "reflect finds the fake class list (registry scan)");
    Catalog cat;
    const Engine engine = forever ? Engine::Forever : Engine::Legacy;
    check(cat.load(engine), "catalog loads");
    check(static_cast<int>(cat.classes().size()) == g.classCount, "every class listed");
    const ClassEntry* tmp = cat.byName("CTrackManiaPlayer");
    check(tmp != nullptr, "class by name");
    if (!tmp) return;
    const auto ch = cat.chain(tmp);
    check(ch.size() == 3 && ch[1]->name == "CGamePlayer" && ch[2]->name == "CMwNod", "parent chain");
    const MemberEntry* login = cat.findMember(tmp, "Login");
    check(login && login->owner->name == "CGamePlayer" && login->offset == 8, "inherited member found by name");
    const MemberEntry* mobil = cat.findMember(tmp, "Mobil");
    check(mobil && mobil->declared && mobil->declared->name == "CSceneMobil", "declared class of a nod member");
    const MemberEntry* respawn = cat.findMember(tmp, "Respawn");
    check(respawn && isMethod(respawn->ti.kind) && respawn->offset == -1, "method has no offset");

    beginFrame(5000);
    check(cat.classOf(g.app) == cat.byName("CTrackMania"), "class of App");
    check(cat.classOf(g.player) == tmp, "class of a player");
    check(cat.classOf(0) == nullptr && cat.classOf(0x1234) == nullptr && cat.classOf(0xdead0000u) == nullptr, "no class for junk pointers");
    check(cat.classOf(g.player + 0x58) == nullptr, "no class for plain data");
    check(cat.classOf(g.guardPage + 64) == nullptr && fake::guardIntact(), "guard page is never touched");

    ReadOptions opt;
    ObjectView v;
    check(readObject(cat, g.app, opt, &v), "read App");
    checkEq(valueOf(v, "PillarOffset"), "8", "App.PillarOffset");
    checkEq(valueOf(v, "PlayTest"), "2", "App.PlayTest (enum)");
    checkEq(valueOf(v, "MenuManager"), "0x00001234 (not a nod)", "junk pointer");
    check(valueOf(v, "Switcher").find("(not a nod)") != std::string::npos, "pointer to data is not a nod");
    checkEq(valueOf(v, "Races"), "0xDEAD0000 (not a nod)", "unmapped pointer");
    checkEq(valueOf(v, "MatchSettings"), "0 items", "empty list");
    checkEq(valueOf(v, "GameScene"), "getter (not read)", "getter is not called");
    check(rowOf(v, "ShowContextHelp") == nullptr, "methods hidden by default");
    const Row* pg = rowOf(v, "CurrentPlayground");
    check(pg && pg->nod && pg->nodClass && pg->nodClass->name == "CGamePlayground", "nod link with its class");
    check(v.rows.size() > 0 && v.rows[0].m->owner->name == "CTrackMania", "own members first");
    opt.methods = true;
    check(readObject(cat, g.app, opt, &v) && valueOf(v, "ShowContextHelp") == "method (not called)", "methods shown on request");
    opt.methods = false;

    Path p;
    check(parsePath("App.CurrentPlayground.GameTerminals[0].ControlledPlayer", &p), "parse player path");
    p.rootObj = g.app;
    int failed = 0;
    check(resolvePath(cat, &p, &failed) && p.object() == g.player, "path resolves by names to the player");
    check(readObject(cat, p.object(), opt, &v), "read player");
    checkEq(valueOf(v, "Login"), "\"oskar_tm\"", "string");
    checkEq(valueOf(v, "Name"), "\"$f80Osk\xc3\xa4r \xf0\x9f\x8f\x81\"", "wstring with umlaut and emoji");
    checkEq(valueOf(v, "IsSpectator"), "false", "bool");
    checkEq(valueOf(v, "CurRaceTime"), "31415", "uint");
    checkEq(valueOf(v, "TeamId"), "-1", "int");
    checkEq(valueOf(v, "Location"), "pos 512, 9.5, 140.75", "iso4");
    const Row* color = rowOf(v, "Color");
    check(color && color->hasColor && color->value == "1, 0.42, 0.1", "color with swatch");
    const Row* cps = rowOf(v, "CheckpointTimes");
    check(cps && cps->count == 5 && cps->items.size() == 5 && cps->items[4].text == "22500", "uint list");
    const Row* tags = rowOf(v, "Tags");
    check(tags && tags->count == 2 && tags->items[1].text == "\"line\\nbreak \\\"quoted\\\"\"", "string list escaped");
    const Row* mob = rowOf(v, "Mobil");
    check(mob && mob->nod && mob->nodClass && mob->nodClass->name == "CSceneMobil", "player.Mobil link");

    ObjectView mv;
    check(readObject(cat, mob ? mob->nod : 0, opt, &mv), "read mobil");
    checkEq(valueOf(mv, "Position"), "512, 9.5, 140.75", "vec3");
    checkEq(valueOf(mv, "Speed"), "220.5", "float");
    checkEq(valueOf(mv, "WheelContacts"), "array (layout unknown)", "array is not guessed");
    check(valueOf(mv, "Item").find("(not a nod)") != std::string::npos && fake::guardIntact(), "pointer into a guard page");
    check(valueOf(mv, "Corpus").find("(not a nod)") != std::string::npos, "pointer into freed memory");

    Path blocks;
    parsePath("Challenge.Blocks[3]", &blocks);
    blocks.rootObj = g.challenge;
    check(resolvePath(cat, &blocks, &failed), "Challenge.Blocks[3] resolves");
    ObjectView bv;
    check(readObject(cat, blocks.object(), opt, &bv) && valueOf(bv, "Coord") == "13, 1, 23", "block coord (int3)");
    checkEq(valueOf(bv, "BlockId"), forever ? "0x40000003" : "1073741827", "id (Forever) / uint (ESWC)");
    ObjectView cv;
    check(readObject(cat, g.challenge, opt, &cv), "read challenge");
    const Row* bl = rowOf(cv, "Blocks");
    check(bl && bl->count == 400 && bl->items.size() == 8 && bl->items[0].nod != 0, "list shows the first 8 of 400");
    checkEq(valueOf(cv, "ChallengeName"), "\"A01-Race\"", "challenge name");

    Path bad;
    parsePath("App.CurrentPlayground.GameTerminals[5].ControlledPlayer", &bad);
    bad.rootObj = g.app;
    check(!resolvePath(cat, &bad, &failed) && failed == 1, "index past the end fails at that step");
    parsePath("App.Nope", &bad);
    bad.rootObj = g.app;
    check(!resolvePath(cat, &bad, &failed) && failed == 0, "unknown member fails");
    parsePath("App.PillarOffset", &bad);
    bad.rootObj = g.app;
    check(!resolvePath(cat, &bad, &failed) && failed == 0, "a float is not a step");
    parsePath("Race", &bad);
    bad.rootObj = 0;
    check(!resolvePath(cat, &bad, &failed) && failed == -1, "missing root");

    // read limit
    beginFrame(10);
    ObjectView lv;
    readObject(cat, g.player, opt, &lv);
    check(budgetHit() && lv.skipped > 0 && readsUsed() == 10, "read limit stops reading");
    beginFrame(5000);
    readObject(cat, g.player, opt, &lv);
    check(!budgetHit() && lv.skipped == 0, "enough budget: nothing skipped");
    printf("  player view: %d rows, %d reads\n", static_cast<int>(lv.rows.size()), lv.reads);

    // dump
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir.resize(dir.find_last_of('\\') + 1);
    const std::string file = dir + (forever ? "classes-test-forever.json" : "classes-test-legacy.json");
    DumpInfo di;
    di.game = "Fake";
    di.exeSha256 = "00";
    const int n = dumpClasses(cat, di, std::wstring(file.begin(), file.end()));
    check(n == g.classCount, "dump writes every class");
    FILE* f = fopen(file.c_str(), "rb");
    std::string body;
    if (f) {
        char buf[4096];
        size_t k;
        while ((k = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, k);
        fclose(f);
    }
    check(body.find("\"name\": \"CTrackManiaPlayer\"") != std::string::npos && body.find("\"kind\": \"wstring\"") != std::string::npos &&
              body.find("\"class\": \"CSceneMobil\"") != std::string::npos,
          "dump has names, kinds and declared classes");
    check(fake::guardIntact(), "guard page still intact after browsing");
}

// ---------------------------------------------------------------- 3. plugin in a fake host -------

void testPlugin(const char* dll) {
    printf("plugin in a fake host: %s\n", dll);
    stubhost::setup();
    stubhost::graph = fake::build(true);
    stubhost::settings["Path"] = "App.CurrentPlayground.GameTerminals[0].ControlledPlayer";
    stubhost::settings["Pins"] = "Race\nChallenge.Blocks[2]";
    stubhost::settings["Refresh"] = "2"; // every frame
    stubhost::settings["Class"] = "CTrackManiaPlayer";

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* px = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);

    HMODULE mod = LoadLibraryA(dll);
    check(mod != nullptr, "LoadLibrary");
    if (!mod) return;
    auto info = reinterpret_cast<HorizonPlugin_InfoFn>(GetProcAddress(mod, HORIZON_EXPORT_INFO));
    auto init = reinterpret_cast<HorizonPlugin_InitFn>(GetProcAddress(mod, HORIZON_EXPORT_INIT));
    auto shutdown = reinterpret_cast<HorizonPlugin_ShutdownFn>(GetProcAddress(mod, HORIZON_EXPORT_SHUTDOWN));
    auto drawMenu = reinterpret_cast<HorizonPlugin_DrawMenuFn>(GetProcAddress(mod, HORIZON_EXPORT_DRAW_MENU));
    auto setEnabled = reinterpret_cast<HorizonPlugin_SetEnabledFn>(GetProcAddress(mod, HORIZON_EXPORT_SET_ENABLED));
    check(info && init && shutdown && drawMenu && setEnabled, "exports");
    check(!GetProcAddress(mod, HORIZON_EXPORT_DRAW_OVERLAY) && !GetProcAddress(mod, HORIZON_EXPORT_ON_EVENT), "no overlay, no event handler");
    if (!info || !init || !shutdown || !drawMenu || !setEnabled) return;
    const HorizonPluginInfo* pi = info();
    check(pi->abi == HORIZON_ABI && strcmp(pi->id, "node-explorer") == 0, "info id and abi");
    check((pi->flags & HORIZON_FLAG_ONLINE_OK) && pi->caps == HORIZON_CAP_MENU, "online-ok, menu only (no overlay, events, device or game control)");
    check((pi->games & HORIZON_GAME_FOREVER) == HORIZON_GAME_FOREVER && (pi->games & HORIZON_GAME_ESWC) && (pi->games & HORIZON_GAME_SUNRISE),
          "games eswc, sunrise, tmnf, tmuf");

    const HorizonHostApi host = stubhost::api();
    ImDrawData* dd = nullptr;
    for (int tab = 0; tab < 3; tab++) {
        stubhost::settings["Tab"] = std::to_string(tab); // the plugin opens the tab of last time (read in Init)
        if (tab) shutdown();
        check(init(&host) == 0, "Init returns 0");
        for (int frame = 0; frame < 4; frame++) {
            ImGui::NewFrame();
            ImGui::SetNextWindowSize(ImVec2(1200, 680));
            ImGui::Begin("plugin page");
            drawMenu();
            ImGui::End();
            ImGui::Render();
            dd = ImGui::GetDrawData();
        }
        check(dd && dd->TotalVtxCount > 0, "tab draws");
        check(stubhost::settings["Tab"] == std::to_string(tab), "tab of last time opened and kept");
    }
    check(stubhost::logs.find("13 classes in the engine reflection") != std::string::npos, "plugin found the fake class list");
    check(stubhost::settings["Path"] == "App.CurrentPlayground.GameTerminals[0].ControlledPlayer", "last path kept");
    check(fake::guardIntact(), "guard page intact after the plugin's frames");
    setEnabled(0);
    setEnabled(1);
    shutdown();
    check(FreeLibrary(mod) != 0, "FreeLibrary");
    ImGui::DestroyContext();
}

} // namespace

int main(int argc, char** argv) {
    testTypes();
    testFormat();
    testPath();
    browseGraph(true);
    browseGraph(false);
    if (argc > 1) testPlugin(argv[1]);
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    printf("node explorer tests: %s\n", g_fail ? "FAILED" : "passed");
    return g_fail ? 1 : 0;
}
