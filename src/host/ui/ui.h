// The host's menu shell: the big centred panel (tile grid of plugins, plugin pages, hotkeys, log,
// about) and the slim Horizon bar at the top edge. Pure Dear ImGui: no Direct3D, no game, no
// plugin calls of its own. The host fills a Model every frame and passes Actions to call back.
#pragma once

#include <functional>
#include <string>
#include <vector>

struct ImFont;

// rpcndr.h (pulled in by shellapi.h / OLE headers) defines "small" as char.
#ifdef small
#undef small
#endif

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
// Builds the fonts again at a new scale between two frames. The ImFont objects stay the same (plugins keep
// the pointers from HorizonStyle); the caller drops the backend's font texture so it is made again.
void reloadFonts(float scale);
// Where to read the icon font from when host.dll has no resource (the UI preview). Call before loadFonts.
void setIconFontFile(const std::string& path);
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

// A panel of a plugin (ABI minor 2): a tab on its page, maybe popped out into its own window.
struct PanelView {
    std::string key;
    std::string label;        // tab text: icon (when the font has it) + title
    std::string title;
    unsigned icon = 0;        // codepoint, 0 = none
    bool detached = false;    // in its own window
};

struct PluginView {
    std::string id, name, version, author, description;
    PluginStatus status = PluginStatus::Off;
    std::string statusText;   // short, e.g. "Init returned 3", "solo only: off while online"
    bool enabled = false;     // the switch position
    bool hasMenu = false;     // has a settings page (DrawMenu)
    bool liveToggle = false;  // switching applies now
    bool restartToSwitch = false; // moving the switch only takes effect after a restart
    std::string restartText;  // tooltip of the switch then, e.g. "Needs a restart: this module hooks the game at start."
    bool soloOnly = false;
    std::vector<PanelView> panels; // tabs after "Settings"
    float winX = -1, winY = -1, winW = 0, winH = 0; // remembered rect of its free window (px), winX < 0 = none
    float minW = 480, minH = 320, prefW = 820, prefH = 560; // plugin.json "ui": its own window, logical px at scale 1
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

// A mod in the game that is not a Horizon plugin (TMVS, TMInterface, CenterSpeed, Twinkie, another
// TMLoader mod, the chained dinput8.dll): shown under "Other mods" for orientation, no control.
struct OtherModView {
    std::string name;     // "TM Vibrant Shaders"
    std::string version;  // "1.5.0" or ""
    std::string file;     // "d3d9.dll", "CenterSpeed.dll"
    std::string kind;     // "d3d9 proxy", "TMLoader mod", "dinput8 chain", "loaded DLL"
    std::string keys;     // its own hotkeys if known: "F5-F8, F12 (menu F8)"; "" unknown
    std::string note;     // one short line, e.g. "Keeps its own Direct3D hook."
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
    std::vector<OtherModView> otherMods;       // "Other mods" section
    bool barEnabled = true;                    // General: the top-edge bar on/off
    int menuHotkey = 0;                        // handle of the menu key in hotkeys (for the key picker)
    std::vector<std::string> menuKeySuggestions; // quick picks for the menu key: "Insert", "F2", "#", "+", "Ctrl+Tab"
    int onlineFix = -1;                        // General: ESWC online login fix, -1 = not this game, 0 off, 1 on (next start)
    // Notifications (toasts, ABI minor 3) and the TM Horizon bridge
    bool dnd = false;                          // do not disturb: toasts only go to the list below
    bool holdToasts = true;                    // keep toasts back while a run is going
    bool bridgeEnabled = true;                 // the connection to TM Horizon on this PC
    std::string bridgeText;                    // "Connected", "Waiting for Allow in TM Horizon", ...
    bool bridgePaired = false;                 // a key is stored (Forget is offered)
    struct Recent {
        std::string owner, title, text;
        unsigned kind = 0;
        bool missed = false;
    };
    std::vector<Recent> recentToasts;          // newest first
    int bridgeState = 0;                       // HORIZON_BRIDGE_*: the "Launcher connected" chip
    // Display (General): menu size in % of the screen and the content scale in %; *Setting 0 = Auto
    int menuSizePct = 85, menuSizeSetting = 0, menuSizeAuto = 85;
    int contentPct = 100, contentSetting = 0, contentAuto = 100;
    // Layout mode ("Edit layout")
    std::string layoutKey;                     // "Ctrl+Insert", "" none
    bool layoutAllowed = true;                 // false on TMNF/TMUF during a race
    std::string layoutWhy;                     // why not, short
    bool forever = false;                      // TMNF/TMUF: say that layout mode is only for the menus
    int widgetOpacity = 65;                    // widget background in % (host.json widgets.opacity)
    int iconStyle = 0;                         // module icons: 0 Distinct (icon.png), 1 Saturn (icon-alt.png)
    // Shared setup (General): one setup for every game install of this Windows user
    bool setupAvailable = false;               // false in the test program (no shared folder)
    bool setupShared = false;                  // the switch (applies after a restart)
    bool setupRestart = false;                 // switched, copied, reset or imported: restart to apply
    std::vector<std::string> setupFiles;       // exported setups (setups\*.json), newest first
};

// One toast on screen (ABI minor 3), see drawToasts.
struct ToastView {
    std::string owner, title, text;
    unsigned kind = 0;   // HORIZON_TOAST_*
    unsigned icon = 0;   // 0 = the kind's icon
    float alpha = 1.0f;  // fade
};
// The toast stack, top right (foreground draw list, no input).
void drawToasts(const std::vector<ToastView>& toasts);

struct Actions {
    std::function<void(const std::string& id, bool enabled)> setPluginEnabled;
    std::function<void(const std::string& id)> drawPluginPage;   // calls the plugin's DrawMenu (inside a child)
    std::function<void(int handle)> beginKeyCapture;              // next key combo pressed is for this hotkey
    std::function<bool(int handle, std::string& keyOut)> pollKeyCapture; // true once a key was taken ("" = cancelled)
    std::function<bool(int handle, const std::string& key)> rebindHotkey; // false: not a valid key / taken
    std::function<void()> openLogFolder;
    std::function<void()> openLauncher;                           // bring TM Horizon to the front
    std::function<void()> closeMenu;
    std::function<void(bool on)> setBarEnabled;
    std::function<void(bool on)> setOnlineFix;
    std::function<void(const std::string& pluginId, const std::string& key)> drawPanelTab; // body or window options
    std::function<void(bool on)> setDnd;
    std::function<void(bool on)> setHoldToasts;
    std::function<void(bool on)> setBridgeEnabled;
    std::function<void()> forgetBridge;
    std::function<void(const std::string& pluginId, float x, float y, float w, float h)> saveWindow; // free window moved/resized
    std::function<void(int pct)> setMenuSize;     // 0 = Auto, else 60/70/80/90
    std::function<void(int pct)> setContentScale; // 0 = Auto, else 80..125
    std::function<void()> toggleLayout;           // "Edit layout" (closes the menu)
    std::function<void(int pct)> setWidgetOpacity; // widget background 20..100 %
    std::function<void(int style)> setIconStyle;  // 0 Distinct (icon.png), 1 Saturn (icon-alt.png)
    std::function<void(bool on)> setSharedSetup;  // General > Shared setup
    std::function<void()> copySetupFromGame;      // this game's own setup becomes the shared one
    std::function<void()> resetSharedSetup;       // the shared setup back to the defaults
    std::function<void()> exportSetup;            // the setup in use into setups\setup-<time>.json
    std::function<void(const std::string& file)> importSetup; // a file from setups\ (name only)
    std::function<void()> openSetupFolder;        // Explorer on ...\Horizon Client\setups
};

// Where the last menu frame was: the window and the room inside a plugin page's card (for ui_metrics).
// pageW/pageH: the room of the container a plugin is being drawn in right now (PageRoom), else the menu page's.
struct MenuMetrics {
    float menuW = 0, menuH = 0, pageW = 0, pageH = 0;
    float headerBottom = 0; // screen y below the menu's header row in the last menu frame (0 = menu not drawn yet)
    int headerFrame = -10;  // ImGui frame of that menu frame (the header only counts while the menu is open)
};
const MenuMetrics& menuMetrics();
// ui_metrics' page_w/page_h for one plugin draw call (menu page, pop-out, free window): set from the
// container's content region, restored afterwards. 0 x 0 = a widget that sizes itself (no fixed room).
class PageRoom {
public:
    PageRoom(float w, float h);
    ~PageRoom();
    PageRoom(const PageRoom&) = delete;
    PageRoom& operator=(const PageRoom&) = delete;
private:
    float w0_, h0_;
};

// A module's own window (pop-out, free window): where it opens and how small it may get.
struct WinRect {
    float x, y, w, h;
    float minW, minH; // for SetNextWindowSizeConstraints
};
// The first row such a window opens at: below the top bar and the menu's header, so both stay reachable.
float windowTop(float displayH);
// x/y < 0: not placed yet (centred, a bit above the middle, shifted by index). w/h: the wanted size (saved or
// preferred), minW/minH: the module's minimum, all in pixels. Clamped to the screen below windowTop().
WinRect fitWindow(float x, float y, float w, float h, float minW, float minH, float displayW, float displayH, int index);

// Fitting the menu into small windows (TMUF at 640x480 and the like).
constexpr float kMinUiScale = 0.8f;     // smallest UI scale the fit goes to (body text about 13 px); below it the menu scrolls
constexpr float kFitWidth = 880.0f;     // the menu's room at scale 1 (header, list and page side by side, footer)
constexpr float kFitHeight = 560.0f;
constexpr float kMinPageWidth = 440.0f; // page beside the list at scale 1; narrower: list or page, with a Back button
// The share of the window the menu frame takes: the player's menu size, nearly all of a small window.
float menuShare(int menuSizePct, float displayW, float displayH);
// The UI scale for this window: the player's scale (screen height x text size), made smaller so the menu fits,
// but not below kMinUiScale (or the player's own scale if that is lower). Steps of 0.05.
float fitScale(float userScale, float displayW, float displayH, int menuSizePct);

// Pictures the host made (ImTextureID values): plugin icons by plugin id, other mods by "other:<name>",
// the TM Horizon wordmark (with its width/height) and the round mark. 0 = none (initials / text).
void setIcon(const std::string& key, unsigned long long texture);
unsigned long long iconFor(const std::string& key);
void setLogo(unsigned long long wordmark, float wordmarkAspect, unsigned long long mark);
unsigned long long logoWordmark(float* aspect);
unsigned long long logoMark();

// The big menu. Call every frame while it is open; sets *open = false when the user closes it.
void drawMenu(const Model& model, const Actions& actions, bool* open);
// Opens the menu on a plugin's page next time (bar: right-click / "Settings").
void showPluginPage(const std::string& id);
// True while the menu's current page is this plugin's page.
bool pluginPageShown(const std::string& id);
// The next draw of the plugin's page selects this panel tab once ("" = Settings), so a hotkey can
// land on a tab although the tab bar remembers the one the user left open.
void selectPanelTab(const std::string& pluginId, const std::string& key);
// Pages: "general", "hotkeys", "log", "about" (the Horizon section), "plugin:<id>", "other:<file>".
// "mods" opens the first plugin (or general when there is none).
void showPage(const char* page);

// Free plugin windows: a plugin's page (Settings + panels) in its own movable window, no menu frame.
// Opened from the bar (left click). Several can be open; they sit above the dim layer and the bar.
void togglePluginWindow(const std::string& pluginId);
bool pluginWindowOpen(const std::string& pluginId);
bool anyPluginWindowOpen();
// Draws the open windows (call every frame, after drawMenu). Returns true if one was drawn.
bool drawPluginWindows(const Model& model, const Actions& actions);

// The Horizon bar. mouseAtTop: the pointer touches the top edge of the game window. Call every
// frame (also while the menu is closed); it animates in/out on its own. Returns true while any part
// of the bar is visible. *openMenu is set when the user asks for the full menu.
bool drawBar(const Model& model, const Actions& actions, bool mouseAtTop, bool menuOpen, bool* openMenu);
// True while the bar is shown and the pointer is over it (the host keeps the mouse from the game).
bool barHovered();

} // namespace ui
} // namespace horizon
