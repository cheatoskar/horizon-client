// Clock: the real time and how long the game has been running, in a corner of the screen.
// Runs unchanged on ESWC, Sunrise, TMNF and TMUF: it only uses UI, Time and settings.
// It shows itself for "Hide after" seconds when the script starts or a map loads, fades out, and comes
// back while the Horizon menu is open and for the same time after the "Show script overlays" hotkey
// (Hotkeys page, default Ctrl+F4; the host calls OnPeek()).

[Setting name="Show clock"]
bool show = true;

[Setting name="Position" options="Top left|Top right|Bottom left|Bottom right"]
int position = 1;

[Setting name="Format" options="24 hours|12 hours"]
int hours = 0;

[Setting name="Seconds"]
bool seconds = true;

[Setting name="Session time" description="How long the game has been running"]
bool showSession = true;

[Setting name="Hide after" description="Seconds the clock stays visible after a map starts or the hotkey is pressed. 0 = always visible" min=0 max=120]
int hideAfter = 10;

[Setting name="Large text"]
bool large = true;

[Setting name="Background" min=0 max=1]
float background = 0.55f;

[Setting name="Distance from the edge" min=0 max=200]
int margin = 16;

const float FADE_MS = 400.0f;

uint64 shownAt = 0;     // Time::Now() of the last start, map or hotkey
float alpha = 1.0f;     // what is drawn right now (follows the target, so it fades)

// Target opacity from the time since the last wake-up: 1 while visible, ramps down over FADE_MS, then 0.
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

void Main() {
    Wake();
    print("Clock ready on " + Game::Title());
}

void OnMapLoaded() { Wake(); }
void OnPeek() { Wake(); }

string ClockText() {
    string f = hours == 0 ? "%H:%M" : "%I:%M";
    if (seconds) f += ":%S";
    if (hours == 1) f += " %p";
    return Time::Format(f);
}

// Window position and pivot for the chosen corner.
void PlaceWindow() {
    vec2 screen = UI::GetScreenSize();
    float m = margin * UI::GetScale();
    bool right = position == 1 || position == 3;
    bool bottom = position == 2 || position == 3;
    vec2 pos(right ? screen.x - m : m, bottom ? screen.y - m : m);
    vec2 pivot(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f);
    UI::SetNextWindowPos(pos, UI::Cond::Always, pivot);
    UI::SetNextWindowBgAlpha(background);
}

void Render() {
    if (!show) return;
    float target = UI::IsMenuOpen() ? 1.0f : TargetAlpha(int64(Time::Now() - shownAt), hideAfter);
    float step = Time::Delta() / FADE_MS;
    if (alpha < target) alpha = alpha + step > target ? target : alpha + step;
    else alpha = alpha - step < target ? target : alpha - step;
    if (alpha <= 0.001f) return;
    PlaceWindow();
    UI::PushStyleVar(UI::StyleVar::Alpha, alpha);
    int flags = UI::WindowFlags::NoDecoration | UI::WindowFlags::AlwaysAutoResize | UI::WindowFlags::NoInputs
              | UI::WindowFlags::NoSavedSettings | UI::WindowFlags::NoFocusOnAppearing | UI::WindowFlags::NoNav;
    if (UI::Begin("Clock", flags)) {
        if (large) UI::PushFont(UI::Font::Title);
        UI::Text(ClockText());
        if (large) UI::PopFont();
        if (showSession) UI::TextColored(UI::MutedColor(), "Session " + Time::FormatDuration(Time::Now()));
    }
    UI::End();
    UI::PopStyleVar();
}
