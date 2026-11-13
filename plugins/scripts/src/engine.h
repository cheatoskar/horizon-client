// Horizon Scripts engine: one AngelScript engine, one module per script folder.
//
// <game>\horizon\scripts\<id>\info.toml + *.as  ->  load (manifest, gate, compile, settings, Main())
// -> callbacks (Render, RenderMenu, game events) under a time budget -> hot reload when a file changes
// (debounced) -> unload. Everything runs on the game thread. The API the scripts see is registered in
// api_*.cpp; it reads the running script through currentScript() for its permissions and folders.
#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "budget.h"
#include "gate.h"
#include "horizon_plugin.h"
#include "manifest.h"
#include "settings_meta.h"

class asIScriptEngine;
class asIScriptModule;
class asIScriptFunction;
class asIScriptContext;
struct asSMessageInfo;

namespace hs {

enum class ScriptState {
    Off,        // switched off by the player
    Running,
    Paused,     // online gate (solo-only script while online)
    WrongGame,  // not made for this game
    BlockedCp,  // game-control script while Competition Patch is loaded
    Failed,     // manifest or compile error: not loaded
    Stopped,    // a script exception (or a crash in a call): loaded, no longer called
    Suspended,  // a callback ran over the time budget: loaded, no longer called
};
const char* stateLabel(ScriptState s); // "Running", "Error", ...

struct Message {
    enum Kind { Error, Warning, Info, Log };
    Kind kind = Info;
    std::string where;  // "main.as:12:5" or "" (manifest, runtime without a line)
    std::string text;
};

struct ScriptSetting {
    std::string var;    // global variable name
    int typeId = 0;
    void* addr = nullptr;
    SettingMeta meta;
    std::string defaultText;  // value after the script's own initialiser
    std::string savedText;    // last value written to the host settings
};

// The running map as scripts see it (Game::Map). One instance, updated in place.
struct MapData {
    std::string name, author, uid, path, environment;
    int authorTime = -1, goldTime = -1, silverTime = -1, bronzeTime = -1, authorScore = -1;
    bool isStunts = false;
};

enum Callback {
    CbMain, CbRender, CbRenderMenu, CbOnMapLoaded, CbOnMapLoadedNoArg, CbOnRaceStart, CbOnCheckpoint, CbOnRespawn, CbOnFinish,
    CbOnRaceEnd, CbOnMenu, CbOnGameState, CbOnPeek, CbOnUnload, CbRenderWidget, CbCount
};

// A widget a script added with UI::AddWidget: a host panel (HORIZON_PANEL_HUD | START_HIDDEN) that the layout
// mode moves, resizes and minimises like a module's widget. The body is drawn by RenderWidget(id).
struct WidgetDef {
    std::string id;      // the script's own id ("clock")
    std::string title;   // shown in layout mode and on the tab
    float x = 1.0f, y = 0.0f; // default place, 0..1 per axis (0 left/top, 0.5 centre, 1 right/bottom)
};

struct Script {
    std::string id;               // folder name
    std::wstring folder;          // ...\horizon\scripts\<id>\  (trailing backslash)
    std::wstring dataFolder;      // ...\horizon\data\scripts\<id>\ (created on first IO write)
    Manifest manifest;
    bool hasManifest = false;
    bool enabled = true;
    ScriptState state = ScriptState::Off;
    std::string stateText;        // why (shown under the name)
    std::vector<Message> messages; // compile errors, warnings, runtime errors, log lines (newest last)
    int errorCount = 0, warningCount = 0;
    std::vector<std::string> files; // .as files compiled (relative)

    asIScriptModule* module = nullptr;
    asIScriptFunction* cb[CbCount] = {};
    std::vector<ScriptSetting> settings;
    std::vector<WidgetDef> widgets; // UI::AddWidget (cleared on unload)

    uint64_t loadedSig = 0;       // file signature when loaded
    uint64_t pendingSig = 0;      // a changed signature waiting for the debounce
    uint64_t pendingSince = 0;
    uint64_t loadedAtMs = 0;
    int loads = 0;                // how often it was (re)loaded in this session
    double lastMs = 0, maxMs = 0; // time of the last / slowest callback
    uint64_t logWindowStart = 0;
    int logInWindow = 0, logDropped = 0;
    bool gameControlNow = false;
    bool mainDone = false;        // Main() ran (it waits while the script starts paused online)

    bool loaded() const { return module != nullptr; }
    bool active() const { return state == ScriptState::Running; }
};

struct Config {
    double callLimitMs = 25.0;    // per callback; Main() and global initialisers get 4x
    int readLimit = 4000;         // guarded reflection reads per frame, all scripts together
    bool hideInCaptures = true;   // skip Render() while a photo/video is taken (host flag CAPTURE)
};

class Manager {
public:
    // scriptsDir: <game>\horizon\scripts\  dataDir: the plugin's data folder (horizon\data\scripts\)
    bool init(const HorizonHostApi* host, const std::wstring& scriptsDir, const std::wstring& dataDir, uint32_t game);
    void shutdown();
    bool ready() const { return engine_ != nullptr; }

    // Once per frame (and from tests with their own clock): folder poll every 500 ms, debounced
    // reloads, gate changes, settings saved when a script changed them.
    void tick(uint64_t nowMs);
    void setGateInput(const GateInput& in);
    const GateInput& gateInput() const { return gate_; }

    void render();                       // Render() of every running script with "ui" (inside an ImGui frame)
    void renderMenu(Script& s);          // RenderMenu() of one script (inside the Scripts tab)
    void renderWidget(Script& s, const std::string& id); // RenderWidget(id) inside the host's widget window
    void dispatch(const HorizonEvent& e);
    void peek();                         // the "show overlays" hotkey: OnPeek() of every running script

    void reload(Script& s);
    void reloadAll();
    void setEnabled(Script& s, bool on);
    void rescan();                       // folder poll now
    void settingChanged(Script& s, ScriptSetting& st); // from the settings page: save now
    void resetSettings(Script& s);

    std::vector<std::unique_ptr<Script>>& scripts() { return scripts_; }
    Script* find(const std::string& id);
    const std::wstring& scriptsDir() const { return scriptsDir_; }
    asIScriptEngine* engine() const { return engine_; }
    const HorizonHostApi* host() const { return host_; }

    Config cfg;
    MapData map;
    bool hasMap = false;
    uint32_t game = 0;
    uint64_t nowMs = 0, startMs = 0;
    float frameMs = 0;
    int64_t raceStartMs = -1;            // Horizon clock at the last RACE_START, -1 outside a race
    int gameState = 0;                   // last HorizonGameState from events / polling
    bool online = false;

    // API helpers (api_*.cpp)
    void addMessage(Script& s, Message::Kind k, const std::string& where, const std::string& text);
    void scriptLog(Script& s, int level, const std::string& text); // rate-limited into the host log too
    std::string settingKey(const Script& s, const std::string& var) const;
    int stringTypeId() const { return stringTypeId_; }

private:
    void loadScript(Script& s);
    void unloadScript(Script& s, bool callUnload);
    bool compile(Script& s);
    void bindCallbacks(Script& s);
    void bindSettings(Script& s);
    void applyGate(Script& s);
    void saveChangedSettings(Script& s);
    // Runs one function under the budget. False when it failed (the script is then Stopped/Suspended).
    bool call(Script& s, asIScriptFunction* f, const char* what, double limitMs, bool ui, void (*args)(asIScriptContext*, const void*),
              const void* argData);
    void onMessage(const asSMessageInfo* msg);
    void lineCallback(asIScriptContext* ctx);

    const HorizonHostApi* host_ = nullptr;
    asIScriptEngine* engine_ = nullptr;
    std::wstring scriptsDir_, dataDir_;
    std::vector<std::unique_ptr<Script>> scripts_;
    Script* compiling_ = nullptr;
    Budget budget_;
    GateInput gate_;
    uint64_t nextPollMs_ = 0, nextSettingsMs_ = 0;
    int stringTypeId_ = 0;
    friend struct ManagerAccess;
};

// The script whose callback is running (nullptr outside script code) and the manager.
Script* currentScript();
Manager* currentManager();
void setCurrent(Manager* m, Script* s);
// Sets a script exception on the active context ("needs permission ui in info.toml").
void scriptError(const std::string& text);
// True when the running script has the permission; otherwise sets an exception naming it.
bool requirePerm(uint32_t perm, const char* what);

// Settings values as text (host settings store them as strings).
std::string settingToText(int typeId, const void* addr, int stringTypeId);
bool settingFromText(int typeId, void* addr, const std::string& text, int stringTypeId);

// API registration (api_*.cpp). Each returns false if a registration failed (a bug: logged).
bool registerCoreApi(asIScriptEngine* e);
bool registerUiApi(asIScriptEngine* e);
bool registerGameApi(asIScriptEngine* e);
bool registerIoApi(asIScriptEngine* e);
bool registerReflectionApi(asIScriptEngine* e);

// UI guard (api_ui.cpp): every Begin/Push a script makes is recorded; uiEndCall() closes whatever the
// script left open (also after an exception or a budget abort), so the host's ImGui stack stays sound.
void uiBeginCall();
void uiEndCall();
bool uiAllowed();
// True while RenderWidget() runs (UI::WidgetAlpha works only there).
bool uiInWidget();
void uiSetInWidget(bool on);
// Reflection (api_reflection.cpp): per-frame read budget and the class catalog.
void reflectionBeginFrame(int readLimit);
void reflectionShutdown();

} // namespace hs
