// Map Info: name, author and medal times of the running map, from OnMapLoaded, as a widget.
// Name, "by <author>", then one row per medal (a drawn medal icon and the time).
// The widget is off until you show it (Ctrl+Insert, "+ Map Info" in the bar at the top, or "Show" on its tab on
// the Horizon Scripts page); layout mode moves and resizes it and Horizon keeps the place. It fades out
// "Hide after" seconds after the map loads and comes back while the menu is open and after the
// "Show script widgets" hotkey (default Ctrl+F4, the host calls OnPeek()).
// Runs unchanged on ESWC, Sunrise, Original, TMNF and TMUF. Some games leave the author empty for maps
// started from their own menus; then the line is simply left out.

[Setting name="Medal times"]
bool showMedals = true;

[Setting name="Hide while driving"]
bool hideWhileDriving = false;

[Setting name="Hide after" description="Seconds after a map start. 0 = always." min=0 max=120]
int hideAfter = 10;

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
    UI::AddWidget("map-info", "Map Info", vec2(0, 0)); // top left
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

void RenderWidget(const string &in id) {
    bool preview = UI::IsMenuOpen() || UI::IsLayoutMode();
    float target = preview ? 1.0f : TargetAlpha(int64(Time::Now() - shownAt), hideAfter);
    if (!preview && (!haveMap || (hideWhileDriving && Game::InRace()))) target = 0.0f;
    float step = Time::Delta() / FADE_MS;
    if (alpha < target) alpha = alpha + step > target ? target : alpha + step;
    else alpha = alpha - step < target ? target : alpha - step;
    UI::WidgetAlpha(alpha);
    float sc = UI::GetScale();
    UI::PushStyleVar(UI::StyleVar::ItemSpacing, vec2(5.0f * sc, 1.0f * sc));
    if (!haveMap) {
        UI::TextColored(UI::MutedColor(), "No map");
    } else {
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
    UI::PopStyleVar();
}
