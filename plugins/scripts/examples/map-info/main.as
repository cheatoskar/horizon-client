// Map Info: name, author and medal times of the running map, from OnMapLoaded.
// Runs unchanged on ESWC, Sunrise, TMNF and TMUF. Some games leave the author empty for maps started
// from their own menus; then the line is simply left out.

[Setting name="Show"]
bool show = true;

[Setting name="Position" options="Top left|Top right|Bottom left|Bottom right"]
int position = 0;

[Setting name="Medal times"]
bool showMedals = true;

[Setting name="Hide while driving" description="Only show it in menus, on the end screen and in replays"]
bool hideWhileDriving = false;

[Setting name="Background" min=0 max=1]
float background = 0.7f;

[Setting name="Distance from the edge" min=0 max=200]
int margin = 16;

// Medal colours
const vec4 AUTHOR = vec4(0.18f, 0.80f, 0.44f, 1.0f);
const vec4 GOLD = vec4(1.00f, 0.80f, 0.20f, 1.0f);
const vec4 SILVER = vec4(0.78f, 0.81f, 0.86f, 1.0f);
const vec4 BRONZE = vec4(0.82f, 0.52f, 0.28f, 1.0f);

bool haveMap = false;
string mapName, mapAuthor, environment;
int authorTime = -1, goldTime = -1, silverTime = -1, bronzeTime = -1;
int authorScore = -1;
bool stunts = false;

void Main() {
    // loaded (or reloaded) while a map is already running: pick it up
    Game::Map@ map = Game::CurrentMap();
    if (map !is null) OnMapLoaded(map);
}

void OnMapLoaded(Game::Map@ map) {
    haveMap = map !is null;
    if (!haveMap) return;
    mapName = map.name;
    mapAuthor = map.author;
    environment = map.environment;
    authorTime = map.authorTime;
    goldTime = map.goldTime;
    silverTime = map.silverTime;
    bronzeTime = map.bronzeTime;
    authorScore = map.authorScore;
    stunts = map.isStunts;
    print("map: " + mapName + (mapAuthor.isEmpty() ? "" : " by " + mapAuthor));
}

void OnMenu() {
    haveMap = false;
}

void MedalRow(const string &in name, int ms, const vec4 &in color) {
    if (ms < 0) return;
    UI::TableNextRow();
    UI::TableNextColumn();
    UI::TextColored(color, name);
    UI::TableNextColumn();
    UI::Text(Time::FormatRace(ms));
}

void Render() {
    if (!show || !haveMap) return;
    if (hideWhileDriving && Game::InRace()) return;

    vec2 screen = UI::GetScreenSize();
    float m = margin * UI::GetScale();
    bool right = position == 1 || position == 3;
    bool bottom = position == 2 || position == 3;
    UI::SetNextWindowPos(vec2(right ? screen.x - m : m, bottom ? screen.y - m : m), UI::Cond::Always,
                         vec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    UI::SetNextWindowBgAlpha(background);
    int flags = UI::WindowFlags::NoDecoration | UI::WindowFlags::AlwaysAutoResize | UI::WindowFlags::NoInputs
              | UI::WindowFlags::NoSavedSettings | UI::WindowFlags::NoFocusOnAppearing | UI::WindowFlags::NoNav;
    if (UI::Begin("Map Info", flags)) {
        UI::PushFont(UI::Font::Title);
        UI::Text(mapName.isEmpty() ? "Unnamed map" : mapName);
        UI::PopFont();
        string sub = mapAuthor.isEmpty() ? "" : "by " + mapAuthor;
        if (!environment.isEmpty()) sub += (sub.isEmpty() ? "" : "  ·  ") + environment;
        if (!sub.isEmpty()) UI::TextColored(UI::MutedColor(), sub);

        if (showMedals) {
            if (stunts) {
                if (authorScore >= 0) UI::TextColored(AUTHOR, "Author score " + authorScore);
            } else if (UI::BeginTable("medals", 2, UI::TableFlags::SizingFixedFit)) {
                MedalRow("Author", authorTime, AUTHOR);
                MedalRow("Gold", goldTime, GOLD);
                MedalRow("Silver", silverTime, SILVER);
                MedalRow("Bronze", bronzeTime, BRONZE);
                UI::EndTable();
            }
        }
    }
    UI::End();
}
