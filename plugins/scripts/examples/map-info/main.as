// Map Info: name, author and medal times of the running map, from OnMapLoaded.
// Small box in the HUD font: name, "by <author>", then one row per medal (a drawn medal icon and the time).
// It fades out "Hide after" seconds after the map loads and comes back while the Horizon menu is open and
// after the "Show script overlays" hotkey (default Ctrl+F4, the host calls OnPeek()).
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

[Setting name="Hide after" description="Seconds the box stays visible after a map starts or the hotkey is pressed. 0 = always visible" min=0 max=120]
int hideAfter = 10;

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

const float FADE_MS = 400.0f;
uint64 shownAt = 0;
float alpha = 1.0f;

float TargetAlpha(int64 sinceMs, int hideAfterSeconds) {
    if (hideAfterSeconds <= 0) return 1.0f;
    float left = float(hideAfterSeconds) * 1000.0f - float(sinceMs);
    if (left >= FADE_MS) return 1.0f;
    if (left <= 0.0f) return 0.0f;
    return left / FADE_MS;
}

void Wake() {
    shownAt = Time::Now();
}

void OnPeek() { Wake(); }

void Main() {
    // loaded (or reloaded) while a map is already running: pick it up
    Game::Map@ map = Game::CurrentMap();
    if (map !is null) OnMapLoaded(map);
}

void OnMapLoaded(Game::Map@ map) {
    haveMap = map !is null;
    if (!haveMap) return;
    Wake();
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

// Colour with the fade applied: shapes in the draw list do not follow the window alpha by themselves.
vec4 Tint(const vec4 &in c, float shade = 1.0f, float a = 1.0f) {
    return vec4(c.x * shade, c.y * shade, c.z * shade, c.w * a * alpha);
}

// A medal about one text line high: two-tailed ribbon on top, a disc with a darker rim and a light highlight.
void DrawMedal(const vec2 &in pos, float size, const vec4 &in color) {
    float r = size * 0.38f;
    float cx = pos.x + size * 0.5f;
    float cy = pos.y + size - r - size * 0.04f;
    float top = pos.y + size * 0.04f;
    float w = r * 0.8f;
    vec4 ribbon = Tint(color, 0.62f);
    UI::WindowDrawTriangleFilled(vec2(cx - r * 0.95f, top), vec2(cx - r * 0.15f, top), vec2(cx + w * 0.55f, cy), ribbon);
    UI::WindowDrawTriangleFilled(vec2(cx + r * 0.95f, top), vec2(cx + r * 0.15f, top), vec2(cx - w * 0.55f, cy), Tint(color, 0.80f));
    UI::WindowDrawCircleFilled(vec2(cx, cy), r, Tint(color, 0.50f));
    UI::WindowDrawCircleFilled(vec2(cx, cy), r - size * 0.07f, Tint(color));
    UI::WindowDrawCircleFilled(vec2(cx - r * 0.30f, cy - r * 0.32f), r * 0.28f, Tint(vec4(1, 1, 1, 1), 1.0f, 0.55f));
}

void MedalRow(const vec4 &in color, const string &in text) {
    float h = UI::CalcTextSize("0").y;
    vec2 p = UI::GetCursorScreenPos();
    UI::Dummy(vec2(h, h));
    DrawMedal(p, h, color);
    UI::SameLine();
    UI::Text(text);
}

void MedalTime(const vec4 &in color, int ms) {
    if (ms >= 0) MedalRow(color, Time::FormatRace(ms));
}

void Render() {
    if (!show || !haveMap) return;
    if (hideWhileDriving && Game::InRace()) return;
    float target = UI::IsMenuOpen() ? 1.0f : TargetAlpha(int64(Time::Now() - shownAt), hideAfter);
    float step = Time::Delta() / FADE_MS;
    if (alpha < target) alpha = alpha + step > target ? target : alpha + step;
    else alpha = alpha - step < target ? target : alpha - step;
    if (alpha <= 0.001f) return;
    float sc = UI::GetScale();
    UI::PushStyleVar(UI::StyleVar::Alpha, alpha);
    UI::PushStyleVar(UI::StyleVar::WindowPadding, vec2(8.0f * sc, 5.0f * sc));
    UI::PushStyleVar(UI::StyleVar::ItemSpacing, vec2(5.0f * sc, 1.0f * sc));

    vec2 screen = UI::GetScreenSize();
    float m = margin * sc;
    bool right = position == 1 || position == 3;
    bool bottom = position == 2 || position == 3;
    UI::SetNextWindowPos(vec2(right ? screen.x - m : m, bottom ? screen.y - m : m), UI::Cond::Always,
                         vec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    UI::SetNextWindowBgAlpha(background);
    int flags = UI::WindowFlags::NoDecoration | UI::WindowFlags::AlwaysAutoResize | UI::WindowFlags::NoInputs
              | UI::WindowFlags::NoSavedSettings | UI::WindowFlags::NoFocusOnAppearing | UI::WindowFlags::NoNav;
    if (UI::Begin("Map Info", flags)) {
        UI::Text(mapName.isEmpty() ? "Unnamed map" : mapName);
        string sub = mapAuthor.isEmpty() ? "" : "by " + mapAuthor;
        if (!environment.isEmpty()) sub += (sub.isEmpty() ? "" : " - ") + environment;
        if (!sub.isEmpty()) UI::TextColored(UI::MutedColor(), sub);

        if (showMedals) {
            if (stunts) {
                if (authorScore >= 0) MedalRow(AUTHOR, "" + authorScore);
            } else {
                UI::Dummy(vec2(0.0f, 2.0f * sc));
                MedalTime(AUTHOR, authorTime);
                MedalTime(GOLD, goldTime);
                MedalTime(SILVER, silverTime);
                MedalTime(BRONZE, bronzeTime);
            }
        }
    }
    UI::End();
    UI::PopStyleVar(3);
}
