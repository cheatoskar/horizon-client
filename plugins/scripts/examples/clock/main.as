// Clock: the real time and how long the game has been running, in a corner of the screen.
// Runs unchanged on ESWC, Sunrise, TMNF and TMUF: it only uses UI, Time and settings.

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

[Setting name="Large text"]
bool large = true;

[Setting name="Background" min=0 max=1]
float background = 0.55f;

[Setting name="Distance from the edge" min=0 max=200]
int margin = 16;

void Main() {
    print("Clock ready on " + Game::Title());
}

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
    PlaceWindow();
    int flags = UI::WindowFlags::NoDecoration | UI::WindowFlags::AlwaysAutoResize | UI::WindowFlags::NoInputs
              | UI::WindowFlags::NoSavedSettings | UI::WindowFlags::NoFocusOnAppearing | UI::WindowFlags::NoNav;
    if (UI::Begin("Clock", flags)) {
        if (large) UI::PushFont(UI::Font::Title);
        UI::Text(ClockText());
        if (large) UI::PopFont();
        if (showSession) UI::TextColored(UI::MutedColor(), "Session " + Time::FormatDuration(Time::Now()));
    }
    UI::End();
}
