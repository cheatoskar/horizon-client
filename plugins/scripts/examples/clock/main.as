// Clock: the real time and how long the game has been running, as a widget.
// Runs unchanged on ESWC, Sunrise, Original, TMNF and TMUF: it only uses UI, Time and settings.
// The widget is off until you show it (Ctrl+Insert, "+ Clock" in the bar at the top, or "Show" on its tab on the
// Horizon Scripts page). Drag it in layout mode; Horizon keeps the place. It stays on screen in menus, races and
// across map loads. With "Hide after" above 0 it fades out that many seconds after a map loads and comes back
// while the menu is open and after the "Show script widgets" hotkey (OnPeek()).

[Setting name="Format" options="24 hours|12 hours"]
int hours = 0;

[Setting name="Seconds"]
bool seconds = true;

[Setting name="Session time"]
bool showSession = true;

[Setting name="Hide after" description="Seconds after a map start. 0 = always shown." min=0 max=120]
int hideAfter = 0;

[Setting name="Large text"]
bool large = true;

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
    UI::AddWidget("clock", "Clock", vec2(1, 0)); // top right
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

void RenderWidget(const string &in id) {
    float target = UI::IsMenuOpen() || UI::IsLayoutMode() ? 1.0f : TargetAlpha(int64(Time::Now() - shownAt), hideAfter);
    float step = Time::Delta() / FADE_MS;
    if (alpha < target) alpha = alpha + step > target ? target : alpha + step;
    else alpha = alpha - step < target ? target : alpha - step;
    UI::WidgetAlpha(alpha);
    if (large) UI::PushFont(UI::Font::Title);
    UI::Text(ClockText());
    if (large) UI::PopFont();
    if (showSession) UI::TextColored(UI::MutedColor(), "Session " + Time::FormatDuration(Time::Now()));
}
