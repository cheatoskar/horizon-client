// The host's menu shell: the big centred panel (tile grid of plugins, plugin pages, hotkeys, log,
// about) and the slim Horizon bar at the top edge. Pure Dear ImGui: no Direct3D, no game, no
// plugin calls of its own. The host fills a Model every frame and passes Actions to call back.
#pragma once

#include <functional>
#include <string>
#include <vector>

struct ImFont;

namespace horizon {
namespace ui {

// TM Horizon design tokens (tm-hub-mono packages/shared/src/tokens.ts), RGBA 0..1.
struct Rgba {
    float r, g, b, a;
};
struct Tokens {
    Rgba accent{1.0f, 0.42f, 0.10f, 1.0f};    // #ff6b1a
    Rgba accent2{1.0f, 0.54f, 0.27f, 1.0f};   // #ff8a45
    Rgba accent3{1.0f, 0.69f, 0.37f, 1.0f};   // #ffb15e
    Rgba accentInk{0.10f, 0.04f, 0.01f, 1.0f}; // #1a0b02
    Rgba ok{0.24f, 0.86f, 0.52f, 1.0f};       // #3ddc84
    Rgba warn{0.95f, 0.76f, 0.31f, 1.0f};     // #f2c14e
    Rgba bad{1.0f, 0.36f, 0.36f, 1.0f};       // #ff5d5d
    Rgba bg{0.039f, 0.047f, 0.067f, 0.97f};   // #0a0c11
    Rgba bg2{0.063f, 0.075f, 0.102f, 1.0f};   // #10131a
    Rgba surface{0.106f, 0.118f, 0.141f, 1.0f}; // #1b1e24
    Rgba text{0.933f, 0.941f, 0.953f, 1.0f};  // #eef0f3
    Rgba text2{0.765f, 0.788f, 0.820f, 1.0f}; // #c3c9d1
    Rgba muted{0.557f, 0.592f, 0.639f, 1.0f}; // #8e97a3
    float radiusSm = 10.0f, radius = 14.0f, radiusLg = 20.0f; // unscaled px
};
const Tokens& tokens();

struct Fonts {
    ImFont* body = nullptr;    // Segoe UI ~16 px
    ImFont* bodyBold = nullptr; // Segoe UI Semibold ~16 px
    ImFont* title = nullptr;   // display font (Bahnschrift, the closest Windows font to Barlow Condensed) ~28 px
    ImFont* small = nullptr;   // Segoe UI ~13 px
    ImFont* mono = nullptr;    // Consolas / Cascadia Mono ~14 px
};
// Adds the fonts to the current context's atlas (call once, before the backend builds the atlas).
// scale: 1.0 at 1080p window height (host passes clamp(height/1080, 1, 2)).
void loadFonts(float scale);
const Fonts& fonts();
float scale();
// Colours, rounding, spacing of the current context's ImGuiStyle from tokens(), scaled.
void applyStyle(float scale);

enum class PluginStatus {
    On,            // loaded and running
    Off,           // switched off by the user (or not enabled in the profile)
    Failed,        // could not be loaded / Init failed (statusText says why)
    Blocked,       // gated: solo-only plugin while online, or Competition Patch run
    NeedsRestart,  // switched, takes effect after a restart (no live toggle)
    WrongGame      // not made for this game
};

struct PluginView {
    std::string id, name, version, author, description;
    PluginStatus status = PluginStatus::Off;
    std::string statusText;   // short, e.g. "Init returned 3", "solo only: off while online"
    bool enabled = false;     // the switch position
    bool hasMenu = false;     // has a settings page (DrawMenu)
    bool liveToggle = false;  // switching applies now
    bool soloOnly = false;
};

struct HotkeyView {
    int handle = 0;           // > 0
    std::string ownerId;      // "host" or a plugin id
    std::string ownerName;    // "Horizon", "ESWC Autosave"
    std::string name;         // "Open menu", "Save replay now"
    std::string key;          // "Insert", "" = none
    std::string conflict;     // "" or e.g. "also TM Vibrant Shaders (F5-F8, F12)"
};

struct LogLine {
    int level = 1;            // 0 debug, 1 info, 2 warn, 3 error
    std::string text;         // "21:04:11.120  [eswc-autosave] SAVED ..."
};

struct Model {
    std::string hostVersion;  // "0.1.0"
    std::string gameName;     // "TrackMania Nations ESWC"
    std::string exeVersion;   // "0.1.7.9 (unofficial 1.80)"
    std::string profileName;  // from profile.json, "" none
    std::string menuKey;      // "Insert"
    std::string logPath;      // full path of this start's log
    bool safeMode = false;
    bool online = false;
    bool runGate = false;     // Competition Patch run in progress
    std::string gameState;    // "Main menu", "Solo race", ...
    std::vector<PluginView> plugins;
    std::vector<HotkeyView> hotkeys;
    const std::vector<LogLine>* log = nullptr; // newest last, at most ~500
    std::vector<std::string> notes;            // e.g. "Changes apply after a restart: Clips"
};

struct Actions {
    std::function<void(const std::string& id, bool enabled)> setPluginEnabled;
    std::function<void(const std::string& id)> drawPluginPage;   // calls the plugin's DrawMenu (inside a child)
    std::function<void(int handle)> beginKeyCapture;              // next key combo pressed is for this hotkey
    std::function<bool(int handle, std::string& keyOut)> pollKeyCapture; // true once a key was taken ("" = cancelled)
    std::function<bool(int handle, const std::string& key)> rebindHotkey; // false: not a valid key / taken
    std::function<void()> openLogFolder;
    std::function<void()> openLauncher;                           // bring TM Horizon to the front
    std::function<void()> closeMenu;
};

// The big menu. Call every frame while it is open; sets *open = false when the user closes it.
void drawMenu(const Model& model, const Actions& actions, bool* open);
// Opens the menu on a plugin's page next time (bar: right-click / "Settings").
void showPluginPage(const std::string& id);
void showPage(const char* page); // "mods", "hotkeys", "log", "about"

// The Horizon bar. mouseAtTop: the pointer touches the top edge of the game window. Call every
// frame (also while the menu is closed); it animates in/out on its own. Returns true while any part
// of the bar is visible. *openMenu is set when the user asks for the full menu.
bool drawBar(const Model& model, const Actions& actions, bool mouseAtTop, bool menuOpen, bool* openMenu);
// True while the bar is shown and the pointer is over it (the host keeps the mouse from the game).
bool barHovered();

} // namespace ui
} // namespace horizon
