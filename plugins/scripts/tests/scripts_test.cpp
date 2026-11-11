// Horizon Scripts tests without a game: manifest, gate (permissions, online, Competition Patch), time
// budget, [Setting] metadata, IO names, and the engine itself with real scripts on disk (load, compile
// errors with file:line, time budget, exceptions, UI guard, reflection over the fake engine graph,
// settings persistence, hot reload, enable/disable).
//
//   scripts_test.exe            (from the repo root, after build.cmd)
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "budget.h"
#include "engine.h"
#include "fake_graph.h"
#include "fsutil.h"
#include "gate.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "manifest.h"
#include "settings_meta.h"
#include "stub_host.h"
#include "test_host.h"

using namespace hs;

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
#define CHECK(x) check((x), #x)

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// ---------------------------------------------------------------- pure parts ----------------------

void testManifest() {
    printf("manifest\n");
    Manifest m;
    std::string err;
    const char* full = "# a comment\n"
                       "name = \"Clock\"\n"
                       "author = 'me'   # trailing comment\n"
                       "version = \"1.2.3\"\n"
                       "games = [\"eswc\", \"tmnf\",\n"
                       "         \"sunrise\"]\n"
                       "online = \"online-ok\"\n"
                       "permissions = [\"ui\", \"settings\", \"read-game\"]\n"
                       "future = 3\n";
    CHECK(parseManifestToml(full, &m, &err));
    CHECK(m.name == "Clock" && m.author == "me" && m.version == "1.2.3");
    CHECK(m.games == (HORIZON_GAME_ESWC | HORIZON_GAME_TMNF | HORIZON_GAME_SUNRISE));
    CHECK(m.online == OnlineMode::OnlineOk);
    CHECK(m.perms == (PermUi | PermSettings | PermReadGame));
    CHECK(m.warnings.size() == 1 && contains(m.warnings[0], "future"));

    CHECK(parseManifestToml("name = \"x\"\r\n", &m, &err));
    CHECK(m.games == (HORIZON_GAME_ESWC | HORIZON_GAME_SUNRISE | HORIZON_GAME_TMNF | HORIZON_GAME_TMUF)); // default: all
    CHECK(m.online == OnlineMode::SoloOnly && m.perms == 0 && m.version == "0.0.0");
    CHECK(parseManifestToml("name = \"x\"\ngames = \"all\"\n", &m, &err) && m.games == gameBits("all"));
    CHECK(parseManifestToml("name = \"x\"\ngames = [\"forever\"]\n", &m, &err) && m.games == HORIZON_GAME_FOREVER);
    CHECK(parseManifestToml("name = \"x\"\ngames = [\"tmnf\", \"original\"]\n", &m, &err) && m.games == HORIZON_GAME_TMNF && m.warnings.size() == 1);

    CHECK(!parseManifestToml("author = \"x\"\n", &m, &err) && contains(err, "name is missing"));
    CHECK(!parseManifestToml("name = \"x\"\npermissions = [\"ui\", \"write-memory\"]\n", &m, &err) && contains(err, "line 2") &&
          contains(err, "write-memory"));
    CHECK(!parseManifestToml("name = \"x\"\ngames = [\"tm2\"]\n", &m, &err) && contains(err, "tm2"));
    CHECK(!parseManifestToml("name = \"x\"\nonline = \"sometimes\"\n", &m, &err) && contains(err, "online"));
    CHECK(!parseManifestToml("name = \"x\n", &m, &err) && contains(err, "line 1"));
    CHECK(!parseManifestToml("name = \"x\"\nname = \"y\"\n", &m, &err) && contains(err, "twice"));
    CHECK(!parseManifestToml("[script]\nname = \"x\"\n", &m, &err) && contains(err, "sections"));
    CHECK(!parseManifestToml("name \"x\"\n", &m, &err) && contains(err, "expected ="));
    CHECK(!parseManifestToml("name = \"x\"\ngames = [\"eswc\"\n", &m, &err) && contains(err, "not closed"));
    CHECK(!parseManifestToml("name = \"x\" junk\n", &m, &err) && contains(err, "unexpected"));

    CHECK(parseManifestJson("\xEF\xBB\xBF{\"name\": \"J\", \"games\": [\"tmuf\"], \"permissions\": [\"game-control\"], \"online\": \"solo-only\"}", &m, &err));
    CHECK(m.name == "J" && m.games == HORIZON_GAME_TMUF && m.perms == PermGameControl);
    CHECK(!parseManifestJson("{\"name\": \"J\", \"permissions\": [1]}", &m, &err));
    CHECK(!parseManifestJson("[1, 2]", &m, &err));
    CHECK(!parseManifestJson("{\"name\": ", &m, &err) && contains(err, "JSON"));
    CHECK(std::string(permName(PermReadGame)) == "read-game" && permBit("game-control") == PermGameControl && permBit("x") == 0);
}

void testGate() {
    printf("gate\n");
    Manifest m;
    m.games = HORIZON_GAME_ESWC | HORIZON_GAME_TMNF;
    m.online = OnlineMode::SoloOnly;
    GateInput in;
    in.game = HORIZON_GAME_SUNRISE;
    CHECK(decide(m, in).state == GateState::WrongGame);
    in.game = HORIZON_GAME_ESWC;
    CHECK(decide(m, in).state == GateState::Run && !decide(m, in).gameControl);
    in.online = true;
    CHECK(decide(m, in).state == GateState::PausedOnline);
    m.online = OnlineMode::OnlineOk;
    CHECK(decide(m, in).state == GateState::Run);
    // game control: never online, never with CP, never when the host says no
    m.perms = PermGameControl;
    CHECK(decide(m, in).state == GateState::Run && !decide(m, in).gameControl);
    in.online = false;
    CHECK(decide(m, in).gameControl);
    in.hostAllows = false;
    CHECK(!decide(m, in).gameControl);
    in.hostAllows = true;
    in.game = HORIZON_GAME_TMNF;
    in.cpLoaded = true;
    CHECK(decide(m, in).state == GateState::BlockedCp && !decide(m, in).gameControl);
    m.perms = PermUi | PermReadGame; // reading stays allowed with CP
    CHECK(decide(m, in).state == GateState::Run);
    in.game = HORIZON_GAME_ESWC; // CP is a Forever mod; on ESWC the flag has no effect on game control scripts
    m.perms = PermGameControl;
    CHECK(decide(m, in).state == GateState::Run && !decide(m, in).gameControl);
}

uint64_t g_fakeUs = 0;
uint64_t fakeClock() { return g_fakeUs; }

void testBudget() {
    printf("budget\n");
    Budget b(fakeClock);
    g_fakeUs = 1000;
    b.start(10.0);
    int calls = 0;
    while (!b.expired() && calls < 100000) {
        calls++;
        g_fakeUs += 100; // 0.1 ms per line
    }
    CHECK(b.expired());
    CHECK(b.elapsedMs() >= 10.0 && b.elapsedMs() < 14.0); // checked every 32nd line: at most 3.2 ms late
    b.start(10.0);
    CHECK(!b.expired());
    Budget real;
    real.start(1000.0);
    for (int i = 0; i < 1000; i++) CHECK(!real.expired() || i < 0);
}

void testSettingMeta() {
    printf("setting metadata\n");
    SettingMeta sm;
    std::string err;
    CHECK(!parseSettingMeta("Other name=\"x\"", &sm, &err));
    CHECK(parseSettingMeta("Setting", &sm, &err) && err.empty() && sm.name.empty());
    CHECK(parseSettingMeta("Setting name=\"Text size\" min=0.5 max=3 description=\"Bigger \\\"text\\\"\"", &sm, &err) && err.empty());
    CHECK(sm.name == "Text size" && sm.hasMin && sm.hasMax && sm.min == 0.5 && sm.max == 3 && sm.description == "Bigger \"text\"");
    CHECK(parseSettingMeta("Setting options=\"A|B|C\" hidden", &sm, &err) && sm.options.size() == 3 && sm.options[2] == "C" && sm.hidden);
    CHECK(parseSettingMeta("Setting min=abc", &sm, &err) && contains(err, "number"));
    CHECK(parseSettingMeta("Setting min=5 max=1", &sm, &err) && contains(err, "larger") && !sm.hasMin);
    CHECK(parseSettingMeta("Setting wat=1", &sm, &err) && contains(err, "wat"));
}

void testNames() {
    printf("names\n");
    CHECK(safeDataName("notes.txt") && safeDataName("a/b/c.json") && safeDataName("My file 2.txt"));
    CHECK(!safeDataName("") && !safeDataName("../x") && !safeDataName("a/../b") && !safeDataName("C:/x") && !safeDataName("a\\b"));
    CHECK(!safeDataName("/abs") && !safeDataName("a//b") && !safeDataName(".hidden") && !safeDataName("x.") && !safeDataName("a/b/c/d/e"));
    CHECK(!safeDataName("con") && !safeDataName("NUL.txt") && !safeDataName("dir/com1.log") && safeDataName("console.txt"));
    CHECK(!safeDataName(std::string(129, 'a')));
    CHECK(validScriptId("map-info") && validScriptId("My Script_2") && !validScriptId(".git") && !validScriptId("a:b") && !validScriptId(""));
}

// ---------------------------------------------------------------- engine with scripts on disk -----

std::string g_root; // build\test\scripts\run\ (absolute, trailing backslash)

void writeText(const std::string& path, const std::string& text) {
    std::wstring w = widen(path);
    const size_t cut = w.find_last_of(L'\\');
    makeDirs(w.substr(0, cut));
    writeFile(w, text);
}

void addScript(const std::string& game, const std::string& id, const std::string& toml, const std::string& code, const char* file = "main.as") {
    const std::string dir = g_root + game + "\\horizon\\scripts\\" + id + "\\";
    if (!toml.empty()) writeText(dir + "info.toml", toml);
    writeText(dir + file, code);
}

void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            const std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

std::string toml(const char* name, const char* perms, const char* online = "solo-only", const char* games = "all") {
    return std::string("name = \"") + name + "\"\nversion = \"1.0.0\"\ngames = \"" + games + "\"\nonline = \"" + online + "\"\npermissions = [" + perms + "]\n";
}

struct ImGuiFrame {
    ImGuiFrame() {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920, 1080);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
    }
    ~ImGuiFrame() {
        ImGui::EndFrame();
        ImGui::Render();
    }
};

// After the scripts drew: nothing they pushed is still on ImGui's stacks.
bool imguiBalanced() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    return g.CurrentWindowStack.Size == 1 && g.ColorStack.Size == 0 && g.StyleVarStack.Size == 0 && g.FontStack.Size == 0 &&
           g.GroupStack.Size == 0 && g.CurrentWindow && g.CurrentWindow->IDStack.Size == 1 && g.CurrentTable == nullptr && !g.DisabledStackSize;
}

Script* get(Manager& m, const char* id) {
    Script* s = m.find(id);
    if (!s) printf("  (script %s missing)\n", id);
    return s;
}

bool hasMessage(const Script* s, const char* text, const char* where = nullptr) {
    if (!s) return false;
    for (const Message& m : s->messages)
        if (contains(m.text, text) && (!where || contains(m.where, where))) return true;
    return false;
}

void dumpMessages(const Script* s) {
    if (!s) return;
    for (const Message& m : s->messages) printf("    [%s] %s %s\n", s->id.c_str(), m.where.c_str(), m.text.c_str());
}

void testEngine() {
    printf("engine (TMNF, fake engine graph)\n");
    const std::string game = "tmnf";
    removeTree(widen(g_root + game));
    stubhost::settings.clear();
    stubhost::settings["s.good.label"] = "saved";

    addScript(game, "good", toml("Good", "\"ui\", \"settings\", \"read-game\""), R"(
[Setting name="Count" min=0 max=100]
int count = 5;
[Setting name="Label"]
string label = "hello";
[Setting name="Mode" options="A|B"]
int mode = 7;
int renders = 0;
void Main() { print("main " + label + " " + count + " mode " + mode); count = count + 1; }
void Render() {
    renders++;
    if (UI::Begin("Good")) {
        UI::Text("frame " + renders);
        if (UI::BeginTable("t", 2)) { UI::TableNextRow(); UI::TableNextColumn(); UI::Text("a"); UI::EndTable(); }
    }
    UI::End();
}
void OnMapLoaded(Game::Map@ map) { print("map " + map.name + " at " + map.authorTime + " " + Time::FormatRace(map.authorTime)); }
void OnRaceStart() { print("start"); }
void OnCheckpoint(int time, int index) { print("cp " + index + " " + time); }
void OnFinish(int time, bool pb) { print("finish " + time + (pb ? " pb" : "")); }
void OnMenu() { print("menu " + (Game::CurrentMap() is null ? "no map" : "map")); }
)");
    addScript(game, "broken", toml("Broken", ""), "void Main() {\n    int x = ;\n}\n");
    addScript(game, "loop", toml("Loop", "\"ui\""), "void Render() {\n    int i = 0;\n    while (true) { i++; }\n}\n");
    addScript(game, "mainloop", toml("Main loop", ""), "void Main() { for (;;) {} }\n");
    addScript(game, "exc", toml("Exception", ""), "void Main() {\n    array<int> a;\n    a[3] = 1;\n}\n");
    addScript(game, "noperm", toml("No permission", ""), "void Render() {}\nvoid Main() { Reflection::GetInt(\"App.Challenge.TMObjective_AuthorTime\"); }\n");
    addScript(game, "uileft", toml("UI left open", "\"ui\""), R"(
void Render() {
    UI::Begin("Left open");
    UI::PushStyleColor(UI::Col::Text, vec4(1, 0, 0, 1));
    UI::PushID("x");
    UI::BeginGroup();
    UI::BeginDisabled();
    UI::PushFont(UI::Font::Mono);
    if (UI::BeginTable("t", 2)) { UI::TableNextColumn(); }
    UI::Text("still fine");
}
)");
    addScript(game, "wdraw", toml("Window draw", "\"ui\""), R"(
void Render() {
    if (UI::Begin("Draw")) {
        vec2 p = UI::GetCursorScreenPos();
        UI::Dummy(vec2(40, 20));
        UI::WindowDrawCircleFilled(p + vec2(10, 10), 8, vec4(1, 0.8f, 0.2f, 1));
        UI::WindowDrawCircle(p + vec2(10, 10), 8, vec4(0.5f, 0.4f, 0.1f, 1), 1.5f);
        UI::WindowDrawTriangleFilled(p + vec2(20, 0), p + vec2(30, 0), p + vec2(25, 8), vec4(1, 0, 0, 1));
        UI::WindowDrawRectFilled(p + vec2(32, 2), p + vec2(38, 18), vec4(0, 1, 0, 1), 2);
        UI::WindowDrawLine(p, p + vec2(40, 20), vec4(1, 1, 1, 1), 1);
    }
    UI::End();
}
)");
    addScript(game, "uiend", toml("UI end", "\"ui\""), "void Render() {\n    UI::End();\n}\n");
    addScript(game, "uievent", toml("UI in event", "\"ui\", \"read-game\""), "void OnRaceStart() { UI::Text(\"x\"); }\n");
    addScript(game, "solo", toml("Solo", "\"ui\""), "int n = 0;\nvoid Render() { n++; if (n % 2 == 0) print(\"solo \" + n); }\n");
    addScript(game, "io", toml("IO", ""), R"(
void Main() {
    IO::WriteText("notes/a.txt", "hi");
    IO::AppendText("notes/a.txt", " there");
    print("read " + IO::ReadText("notes/a.txt") + " exists " + IO::Exists("notes/a.txt") + " list " + IO::List().length());
    IO::WriteText("../evil.txt", "x");
}
)");
    addScript(game, "refl", toml("Reflection", "\"read-game\""), R"(
void Main() {
    print("name " + Reflection::GetString("App.Challenge.ChallengeName"));
    Nod ch = Reflection::GetNod("App.Challenge");
    print("class " + ch.ClassName() + " kind " + ch.IsKindOf("CMwNod") + " " + ch.IsKindOf("CGamePlayer"));
    print("blocks " + ch.GetCount("Blocks") + " at " + ch.GetUint("TMObjective_AuthorTime"));
    print("login " + Reflection::GetString("App.CurrentPlayground.GameTerminals[0].ControlledPlayer.Login"));
    print("wname " + Reflection::GetString("App.CurrentPlayground.GameTerminals[0].ControlledPlayer.Name"));
    print("speed " + Reflection::GetFloat("App.CurrentPlayground.GameTerminals[0].ControlledPlayer.Mobil.Speed"));
    vec3 p = Reflection::GetVec3("App.CurrentPlayground.GameTerminals[0].ControlledPlayer.Location");
    print("pos " + p.x + " " + p.z);
    Nod player = Reflection::GetNod("App.CurrentPlayground.GameTerminals[1].ControlledPlayer");
    print("p2 " + player.GetString("Login") + " root " + Reflection::Root("Race").ClassName());
    int v = Reflection::GetInt("App.Nope.X");
    print("err1 " + v + " " + Reflection::LastError());
    Reflection::GetBool("App.CurrentPlayground.GameTerminals[0].ControlledPlayer.Mobil.IsTurbo");
    print("err2 " + Reflection::LastError());
    Reflection::GetNod("App.Switcher");
    print("err3 " + Reflection::LastError());
    Reflection::GetNod("App.Races");
    print("err4 " + Reflection::LastError());
    print("cp3 " + Reflection::GetInt("Race.Players[0].CheckpointTimes[2]") + " count " + Reflection::GetCount("Race.Players[0].CheckpointTimes"));
}
)");
    addScript(game, "globalloop", toml("Global loop", ""), "int Forever() { while (true) {} return 1; }\nint x = Forever();\n");
    addScript(game, "wronggame", toml("Wrong game", "\"ui\"", "solo-only", "eswc"), "void Main() {}\n");
    addScript(game, "peek", toml("Peek", ""), "int n = 0;\nvoid OnPeek() { n++; print(\"peeked \" + n); }\n");
    {
        // the real Clock example plus a global initialiser that prints its hide-timer function
        std::string clock;
        readFile(widen("plugins\\scripts\\examples\\clock\\main.as"), &clock, 64 * 1024);
        clock += "\nint probe = Probe();\nint Probe() { print(\"alpha \" + TargetAlpha(0, 10) + \" \" + TargetAlpha(9700, 10) + \" \" + TargetAlpha(10000, 10) + \" \" + TargetAlpha(500000, 0)); return 0; }\n";
        addScript(game, "hidetimer", toml("Hide timer", "\"ui\", \"settings\", \"read-game\""), clock);
    }
    addScript(game, "badmanifest", "name = \"Bad\"\npermissions = [\"ui\", \"write-memory\"]\n", "void Main() {}\n");
    addScript(game, "nomanifest", "", "void Main() {}\n");
    addScript(game, "include", toml("Include", ""), "#include \"other.as\"\nvoid Main() {}\n");
    addScript(game, "sig", toml("Signature", "\"read-game\""), "void OnFinish(int t) {}\nvoid Main() {}\n");
    addScript(game, "memory", toml("Memory", ""), "void Main() { array<int> a(100000000); a[0] = 1; }\n");
    addScript(game, "recurse", toml("Recursion", ""), "int f(int n) { return f(n + 1) + 1; }\nvoid Main() { f(0); }\n");
    addScript(game, "multi", toml("Two files", ""), "void Main() { print(\"helper says \" + Helper()); }\n");
    addScript(game, "multi", "", "string Helper() { return \"hi\"; }\n", "lib\\helper.as");
    addScript(game, "ifgame", toml("If game", ""), "void Main() {\n#if TMNF\n    print(\"on tmnf\");\n#endif\n#if ESWC\n    print(\"on eswc\");\n#endif\n}\n");
    addScript(game, "ctl", toml("Control", "\"read-game\", \"game-control\""), R"(
void Main() { print("can " + Game::CanControl() + " play " + Game::PlayMapFile("C:\\maps\\A01.Challenge.Gbx")); }
void OnRaceStart() { print("online play " + Game::PlayMapFile("C:\\maps\\B.Challenge.Gbx")); }
void OnFinish(int t, bool pb) { Game::PlayMapFile("C:\\maps\\notamap.txt"); }
)");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char* px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    testhost::setup(HORIZON_GAME_TMNF, g_root + game + "\\");
    stubhost::graph = fake::build(true);
    const HorizonHostApi host = testhost::api();
    Manager m;
    m.cfg.callLimitMs = 25;
    const std::wstring scriptsDir = widen(g_root + game + "\\horizon\\scripts\\");
    const std::wstring dataDir = widen(g_root + game + "\\horizon\\data\\scripts\\");
    CHECK(m.init(&host, scriptsDir, dataDir, HORIZON_GAME_TMNF));
    uint64_t t = 100000;
    {
        ImGuiFrame f; // loading may not draw, but Main() runs inside the first tick
        m.tick(t);
        CHECK(imguiBalanced());
    }
    CHECK(m.scripts().size() == 26);

    Script* s = get(m, "good");
    CHECK(s && s->state == ScriptState::Running);
    CHECK(contains(testhost::logs(), "[good] main saved 5 mode 1")); // saved label, mode clamped into its options
    CHECK(s && s->settings.size() == 3);

    s = get(m, "broken");
    CHECK(s && s->state == ScriptState::Failed && hasMessage(s, "", "main.as:2:"));
    s = get(m, "mainloop");
    CHECK(s && s->state == ScriptState::Suspended && hasMessage(s, "ran longer than 100 ms"));
    s = get(m, "exc");
    CHECK(s && s->state == ScriptState::Stopped && hasMessage(s, "Index out of bounds", "main.as:3"));
    s = get(m, "noperm");
    CHECK(s && s->state == ScriptState::Stopped && hasMessage(s, "needs the permission \"read-game\"") && hasMessage(s, "Render() is never called"));
    s = get(m, "io");
    CHECK(s && s->state == ScriptState::Stopped && hasMessage(s, "not a valid file name", "main.as:6"));
    CHECK(contains(testhost::logs(), "[io] read hi there exists true list 1"));
    CHECK(fileExists(dataDir + L"io\\notes\\a.txt") && !fileExists(dataDir + L"evil.txt") && !fileExists(dataDir + L"io\\..\\evil.txt"));
    s = get(m, "refl");
    if (s && s->state != ScriptState::Running) dumpMessages(s);
    CHECK(s && s->state == ScriptState::Running);
    const std::string& L = testhost::logs();
    CHECK(contains(L, "[refl] name A01-Race"));
    CHECK(contains(L, "[refl] class CGameCtnChallenge kind true false"));
    CHECK(contains(L, "[refl] blocks 400 at 23000"));
    CHECK(contains(L, "[refl] login oskar_tm"));
    CHECK(contains(L, "[refl] wname $f80Osk\xC3\xA4r \xF0\x9F\x8F\x81"));
    CHECK(contains(L, "[refl] speed 220.5"));
    CHECK(contains(L, "[refl] pos 512 140.75"));
    CHECK(contains(L, "[refl] p2 ghost root CTrackManiaRace"));
    CHECK(contains(L, "[refl] err1 0 CTrackMania has no member Nope"));
    CHECK(contains(L, "[refl] err2 IsTurbo is a getter"));
    CHECK(contains(L, "[refl] err3 Switcher is not an engine object"));
    CHECK(contains(L, "[refl] err4 Races is not an engine object"));
    CHECK(contains(L, "[refl] cp3 13500 count 5"));
    CHECK(fake::guardIntact());
    s = get(m, "globalloop");
    CHECK(s && s->state == ScriptState::Failed && hasMessage(s, "initialiser ran too long"));
    s = get(m, "wronggame");
    CHECK(s && s->state == ScriptState::WrongGame && !s->loaded());
    s = get(m, "badmanifest");
    CHECK(s && s->state == ScriptState::Failed && hasMessage(s, "write-memory", "info.toml"));
    s = get(m, "nomanifest");
    CHECK(s && s->state == ScriptState::Failed && hasMessage(s, "info.toml (or info.json) is missing"));
    s = get(m, "include");
    CHECK(s && s->state == ScriptState::Failed && hasMessage(s, "#include \"other.as\" is not needed"));
    s = get(m, "sig");
    CHECK(s && s->state == ScriptState::Running && hasMessage(s, "OnFinish has another signature"));
    s = get(m, "memory");
    CHECK(s && s->state == ScriptState::Stopped && hasMessage(s, "Out of memory"));
    s = get(m, "recurse");
    CHECK(s && s->state == ScriptState::Stopped && hasMessage(s, "Stack overflow"));
    CHECK(contains(L, "[multi] helper says hi") && get(m, "multi") && get(m, "multi")->files.size() == 2);
    CHECK(contains(L, "[ifgame] on tmnf") && !contains(L, "on eswc"));
    // game control: offline, no CP -> queued for the end of the frame
    CHECK(contains(L, "[ctl] can true play true"));
    testhost::runQueued();
    CHECK(testhost::playedMaps.size() == 1 && testhost::playedMaps[0] == "C:\\maps\\A01.Challenge.Gbx");

    // a frame: Render() of every running UI script; the guard closes what scripts left open
    {
        ImGuiFrame f;
        reflectionBeginFrame(m.cfg.readLimit);
        const uint64_t t0 = Budget::qpcMicros();
        m.render();
        const double ms = static_cast<double>(Budget::qpcMicros() - t0) / 1000.0;
        CHECK(imguiBalanced());
        CHECK(ms < 200.0);
    }
    s = get(m, "loop");
    CHECK(s && s->state == ScriptState::Suspended && hasMessage(s, "Render() ran longer than 25 ms", "main.as:3"));
    s = get(m, "uileft");
    CHECK(s && s->state == ScriptState::Running && s->errorCount == 0);
    s = get(m, "uiend");
    CHECK(s && s->state == ScriptState::Stopped && hasMessage(s, "UI::End without a matching UI::Begin"));
    CHECK(get(m, "good") && get(m, "good")->state == ScriptState::Running);
    {
        ImGuiFrame f; // a second frame: still balanced, suspended scripts are not called again
        m.render();
        CHECK(imguiBalanced());
        ImGuiWindow* win = ImGui::FindWindowByName("Good##hs.good");
        CHECK(win != nullptr);
        ImGuiWindow* dw = ImGui::FindWindowByName("Draw##hs.wdraw");
        CHECK(dw != nullptr && dw->DrawList->VtxBuffer.Size > 100); // the window-list shapes landed in the window
    }

    CHECK(contains(testhost::logs(), "[hidetimer] alpha 1 0.75 0 1")); // Clock: visible, fading, hidden, "0 = always visible"
    s = get(m, "hidetimer");
    CHECK(s && s->state == ScriptState::Running);
    m.peek(); // the "Show script overlays" hotkey
    m.peek();
    CHECK(contains(testhost::logs(), "[peek] peeked 2"));

    // events
    HorizonMapInfo mi = {};
    mi.size = sizeof(mi);
    mi.name = "A01-Race";
    mi.author = "Nadeo";
    mi.uid = "uid";
    mi.path = "";
    mi.environment = "Stadium";
    mi.author_time = 23140;
    mi.gold_time = 25000;
    mi.silver_time = 30000;
    mi.bronze_time = 38000;
    mi.author_score = -1;
    HorizonEvent e = {};
    e.size = sizeof(e);
    e.type = HORIZON_EVENT_MAP_LOADED;
    e.map = &mi;
    m.dispatch(e);
    e.type = HORIZON_EVENT_RACE_START;
    e.map = nullptr;
    m.dispatch(e);
    e.type = HORIZON_EVENT_CHECKPOINT;
    e.time_ms = 5120;
    e.checkpoint_index = 0;
    m.dispatch(e);
    e.type = HORIZON_EVENT_FINISH;
    e.time_ms = 23999;
    e.is_pb = 1;
    m.dispatch(e);
    e.type = HORIZON_EVENT_MENU_ENTERED;
    m.dispatch(e);
    CHECK(contains(L, "[good] map A01-Race at 23140 0:23.14"));
    CHECK(contains(L, "[good] start") && contains(L, "[good] cp 0 5120") && contains(L, "[good] finish 23999 pb") && contains(L, "[good] menu no map"));
    s = get(m, "uievent");
    CHECK(s && s->state == ScriptState::Stopped && hasMessage(s, "only works inside Render() and RenderMenu()"));
    s = get(m, "ctl"); // OnRaceStart queued a second map; OnFinish passed a non-map file and was stopped
    CHECK(contains(L, "[ctl] online play true"));
    CHECK(s && s->state == ScriptState::Stopped && hasMessage(s, "needs a .Gbx map file", "main.as:4"));
    testhost::runQueued();
    CHECK(testhost::playedMaps.size() == 2 && testhost::playedMaps[1] == "C:\\maps\\B.Challenge.Gbx");

    // online gate
    GateInput gi;
    gi.game = HORIZON_GAME_TMNF;
    gi.online = true;
    m.setGateInput(gi);
    s = get(m, "solo");
    CHECK(s && s->state == ScriptState::Paused);
    CHECK(get(m, "good")->state == ScriptState::Paused); // solo-only too
    {
        testhost::clearLogs();
        ImGuiFrame f;
        m.render();
        m.render();
        CHECK(!contains(testhost::logs(), "[solo]"));
    }
    gi.online = false;
    m.setGateInput(gi);
    CHECK(s->state == ScriptState::Running && get(m, "good")->state == ScriptState::Running);
    {
        ImGuiFrame f;
        m.render();
        m.render();
        CHECK(contains(testhost::logs(), "[solo] solo"));
    }

    // settings: the script changed count in Main(); saved on the next settings check
    m.tick(t += 2500);
    CHECK(stubhost::settings["s.good.count"] == "6");
    CHECK(stubhost::settings.count("s.good.label") && stubhost::settings["s.good.label"] == "saved");
    Script* good = get(m, "good");
    m.resetSettings(*good);
    CHECK(stubhost::settings["s.good.count"] == "5" && stubhost::settings["s.good.label"] == "hello");

    // enable / disable
    m.setEnabled(*good, false);
    CHECK(good->state == ScriptState::Off && !good->loaded() && stubhost::settings["enabled.good"] == "0");
    m.setEnabled(*good, true);
    CHECK(good->state == ScriptState::Running && good->loaded());

    // hot reload: a changed file is picked up after the debounce; new folders load, removed ones go
    const int loadsBefore = good->loads;
    testhost::clearLogs();
    addScript(game, "good", "", "void Main() { print(\"reloaded v2\"); }\n");
    addScript(game, "late", toml("Late", ""), "void Main() { print(\"late arrival\"); }\n");
    addScript(game, "loop", "", "void Render() {}\n"); // fixing a suspended script brings it back
    m.tick(t += 600);
    CHECK(good->loads == loadsBefore);         // seen, waiting for the debounce
    CHECK(get(m, "late") && get(m, "late")->state == ScriptState::Running);
    m.tick(t += 600);
    CHECK(good->loads == loadsBefore + 1 && contains(testhost::logs(), "[good] reloaded v2"));
    CHECK(get(m, "loop")->state == ScriptState::Running);
    CHECK(good->settings.empty()); // the new version declares none
    removeTree(widen(g_root + game + "\\horizon\\scripts\\late"));
    m.tick(t += 600);
    CHECK(m.find("late") == nullptr);
    // broken edit: the error shows, the next good save fixes it
    addScript(game, "good", "", "void Main() {\n  nope();\n}\n");
    m.tick(t += 600);
    m.tick(t += 600);
    CHECK(good->state == ScriptState::Failed && hasMessage(good, "", "main.as:2"));
    addScript(game, "good", "", "void Main() { print(\"fixed\"); }\n");
    m.tick(t += 600);
    m.tick(t += 600);
    CHECK(good->state == ScriptState::Running && contains(testhost::logs(), "[good] fixed"));

    m.shutdown();
    // a second engine in the same process (plugin reloaded): works the same
    Manager m2;
    CHECK(m2.init(&host, scriptsDir, dataDir, HORIZON_GAME_TMNF));
    m2.tick(t += 600);
    CHECK(get(m2, "multi") && get(m2, "multi")->state == ScriptState::Running);
    m2.shutdown();
    ImGui::DestroyContext();
}

void testCompetitionPatch() {
    printf("competition patch (TMNF) and online game control\n");
    const std::string game = "tmnf-cp";
    removeTree(widen(g_root + game));
    addScript(game, "ctl", toml("Control", "\"game-control\""), "void Main() { print(\"ctl main\"); }\n");
    addScript(game, "reader", toml("Reader", "\"read-game\"", "online-ok"),
              "void Main() { print(\"reader \" + Reflection::GetString(\"App.Challenge.ChallengeName\")); }\n"
              "void OnRaceStart() { print(\"reader online \" + Game::IsOnline()); }\n");
    addScript(game, "ctlok", toml("Control online-ok", "\"read-game\", \"game-control\"", "online-ok", "eswc"),
              "void OnRaceStart() { print(\"play \" + Game::PlayMapFile(\"x.Gbx\")); }\n");
    testhost::setup(HORIZON_GAME_TMNF, g_root + game + "\\");
    stubhost::graph = fake::build(true);
    testhost::clearLogs();
    const HorizonHostApi host = testhost::api();
    Manager m;
    CHECK(m.init(&host, widen(g_root + game + "\\horizon\\scripts\\"), widen(g_root + game + "\\horizon\\data\\scripts\\"), HORIZON_GAME_TMNF));
    GateInput gi;
    gi.game = HORIZON_GAME_TMNF;
    gi.cpLoaded = true;
    m.setGateInput(gi);
    m.tick(1000);
    Script* s = get(m, "ctl");
    CHECK(s && s->state == ScriptState::BlockedCp && !s->loaded() && !contains(testhost::logs(), "ctl main"));
    s = get(m, "reader");
    CHECK(s && s->state == ScriptState::Running && contains(testhost::logs(), "[reader] reader A01-Race"));
    // online: the online-ok reader keeps running
    gi.online = true;
    m.setGateInput(gi);
    HorizonEvent e = {};
    e.size = sizeof(e);
    e.type = HORIZON_EVENT_RACE_START;
    testhost::online = true;
    m.dispatch(e);
    CHECK(contains(testhost::logs(), "[reader] reader online true"));
    m.shutdown();

    // ESWC, online, a game-control script that is online-ok: the call is refused, nothing queued
    const std::string g2 = "eswc-online";
    removeTree(widen(g_root + g2));
    addScript(g2, "ctlok", toml("Control online-ok", "\"read-game\", \"game-control\"", "online-ok", "eswc"),
              "void OnRaceStart() { print(\"play \" + Game::PlayMapFile(\"x.Gbx\")); }\n");
    testhost::setup(HORIZON_GAME_ESWC, g_root + g2 + "\\");
    testhost::online = true;
    testhost::clearLogs();
    const HorizonHostApi host2 = testhost::api();
    Manager m2;
    CHECK(m2.init(&host2, widen(g_root + g2 + "\\horizon\\scripts\\"), widen(g_root + g2 + "\\horizon\\data\\scripts\\"), HORIZON_GAME_ESWC));
    GateInput g2i;
    g2i.game = HORIZON_GAME_ESWC;
    g2i.online = true;
    m2.setGateInput(g2i);
    m2.tick(1000);
    CHECK(get(m2, "ctlok") && get(m2, "ctlok")->state == ScriptState::Running);
    m2.dispatch(e);
    testhost::runQueued();
    CHECK(contains(testhost::logs(), "[ctlok] play false") && testhost::playedMaps.empty());
    CHECK(contains(testhost::logs(), "is not allowed now"));
    m2.shutdown();
}

} // namespace

int runScriptsTests() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_root = exe;
    g_root.resize(g_root.find_last_of('\\') + 1);
    g_root += "run\\";
    makeDirs(widen(g_root));
    testManifest();
    testGate();
    testBudget();
    testSettingMeta();
    testNames();
    testEngine();
    testCompetitionPatch();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#ifndef SCRIPTS_TEST_NO_MAIN
int main() { return runScriptsTests(); }
#endif
