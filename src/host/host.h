// Horizon Host internals shared by the host's modules.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "horizon_plugin.h"

namespace horizon {

constexpr const char* kHostVersion = "0.2.0";
constexpr unsigned kHostVersionNum = (0u << 16) | (2u << 8) | 0u; // HorizonHost_Version: (major<<16)|(minor<<8)|patch

enum class LoaderKind : uint32_t { Legacy = 1, TMLoader = 2, Test = 3 };

// ------------------------------------------------------------------ log (log.cpp)
void logOpen(const std::wstring& folder, int keep);
void logLine(int level, const char* owner, const char* fmt, ...);
void logLineV(int level, const char* owner, const char* fmt, va_list ap);
std::wstring logFilePath();
struct LogEntry {
    int level;
    std::string text;
};
// Copies the last lines into out if the log changed since 'seen' (returns the new counter).
unsigned logSnapshot(unsigned seen, std::vector<LogEntry>& out);

#define HLOG(...) ::horizon::logLine(HORIZON_LOG_INFO, "host", __VA_ARGS__)
#define HWARN(...) ::horizon::logLine(HORIZON_LOG_WARN, "host", __VA_ARGS__)
#define HERR(...) ::horizon::logLine(HORIZON_LOG_ERROR, "host", __VA_ARGS__)

// ------------------------------------------------------------------ game identity (gameid.cpp)
struct GameIdentity {
    bool supported = false;
    uint32_t game = 0;            // HORIZON_GAME_* bit
    std::string name;             // "TrackMania Nations ESWC"
    std::string version;          // "0.1.7.9 (unofficial 1.80)"
    std::string sha256;
    uint32_t timestamp = 0;
    uintptr_t base = 0;
    std::wstring exePath, exeFolder;
    std::string why;              // why unsupported
};
GameIdentity identifyGame(bool testMode);

// ------------------------------------------------------------------ state (main.cpp)
// A mod in the process that is not a Horizon plugin (menu "Other mods").
struct OtherMod {
    std::string name, version, file, kind, keys, note;
};

struct HostState {
    LoaderKind loader = LoaderKind::Legacy;
    bool safeMode = false;
    std::string safeWhy;
    bool active = false;          // host running (exe known)
    GameIdentity game;
    std::wstring baseFolder;      // folder of host.dll (profile.json, plugins\), trailing backslash
    std::wstring dataRoot;        // settings\, logs\, data\ live here, trailing backslash
    DWORD mainThread = 0;
    std::string profileName;
    std::vector<std::string> notes; // shown in the menu's About page
    bool competitionPatch = false;  // TMCompPatch.dll loaded (Forever)
    bool tmvsLoaded = false;
    bool chainPresent = false;      // dinput8.chain.dll next to the Legacy Loader
    bool chainIsAutosave = false;   // ... and it is the eswc-autosave loader
    std::vector<OtherMod> otherMods;
    HorizonGameInfo info = {};
    // backing storage of info's strings
    std::string infoGameFolder, infoUserFolder, infoLanguage;
};
HostState& state();
void setHostModule(HMODULE m);

// ------------------------------------------------------------------ game adapters (game/*.cpp)
class GameAdapter {
public:
    virtual ~GameAdapter() = default;
    virtual const char* name() const = 0;
    virtual void install() {}                 // host init, game thread
    virtual void frame() {}                   // once per frame on the game thread, before plugins draw
    virtual HorizonGameState gameState() { return HORIZON_STATE_UNKNOWN; }
    virtual int isOnline() { return 0; }
    virtual bool onlineKnown() const { return true; } // false: solo-only plugins stay gated (Forever for now)
    virtual bool raceRunning() { return false; }
    virtual void* engine(uint32_t /*which*/) { return nullptr; }
    virtual HorizonResult playMapFile(const char* /*path*/, uint32_t /*flags*/) { return HORIZON_ERR_NOT_SUPPORTED; }
    virtual HorizonResult returnToMenu() { return HORIZON_ERR_NOT_SUPPORTED; }
    // save_replay: the run's replay without dialogs. stem "" = the game's default name. pathOut: the file
    // written (UTF-8), why: a short reason when not OK. Game thread.
    virtual HorizonResult saveReplay(const std::wstring& /*stem*/, std::string* /*pathOut*/, std::string* why) {
        if (why) *why = "not supported for this game";
        return HORIZON_ERR_NOT_SUPPORTED;
    }
    virtual bool usesDirectInputBlock() const { return true; } // false on Forever (Competition Patch rule)
    virtual std::string status() const { return std::string(); } // one line for the About page
};
GameAdapter* makeEswcAdapter();
GameAdapter* makeSunriseAdapter();
GameAdapter* makeOriginalAdapter();
GameAdapter* makeForeverAdapter();
GameAdapter* makeNullAdapter();
GameAdapter& game();
void setGame(GameAdapter* adapter);
const char* gameStateText(int s);

// ------------------------------------------------------------------ events (events.cpp)
void eventsInit();
// Fills is_online and map, then delivers e to every subscribed, running, not gated plugin.
void dispatchEvent(HorizonEvent& e);
// Current map (MAP_LOADED fills it), strings owned here.
void setCurrentMap(const HorizonMapInfo* map);
const HorizonMapInfo* currentMap();
// Called once a frame by the overlay: online/state changes become events.
void eventsFrame();
bool isOnlineNow();
bool runGate(); // Competition Patch present and a run going

// ------------------------------------------------------------------ plugins (plugins.cpp)
void pluginsReadProfile(); // profile.json (before the overlay context: menu key, bar)
void pluginsLoadAll();
void pluginsShutdownAll();
void pluginsDrawOverlays();
void pluginsDeviceCreated(void* device, void* params);
void pluginsAdjustParams(void* params);
void pluginsBeforeReset(void* device);
void pluginsAfterReset(void* device, long hr);
void pluginsPrePresent(void* device);
void pluginsPostPresent(void* device, long hr);
void pluginsDeliver(const HorizonEvent& e);
void pluginsRunQueued();              // RunOnGameThread queue (end of frame)
void pluginsSaveSettings(bool force); // dirty settings to disk
size_t pluginCount();

// ------------------------------------------------------------------ host settings + hotkeys (hostcfg.cpp)
void hostConfigLoad();
std::string hostSetting(const std::string& key, const std::string& fallback);
int hostSettingInt(const std::string& key, int fallback);
void hostSetSetting(const std::string& key, const std::string& value);
void hostSetSettingInt(const std::string& key, int value);
void hostConfigSave(bool force);
// Hotkeys: owner "host" or a plugin id.
int hotkeyAdd(const std::string& owner, const std::string& ownerName, const std::string& action, const std::string& name,
              const char* defaultKey, void (*callback)(void*), void* user);
std::string hotkeyText(int handle);
bool hotkeyRebind(int handle, const std::string& text);
void hotkeysPoll(bool foreground, bool typing, bool menuOpen); // fires callbacks
void hotkeysRemoveOwner(const std::string& owner);
struct HotkeyRow {
    int handle;
    std::string owner, ownerName, name, key, conflict;
};
std::vector<HotkeyRow> hotkeyRows();
void hotkeysDetectExternal();
// Window key messages: a navigation key from the number pad (NumLock off: Num0 = Insert) is not the
// navigation key itself, so it never fires a hotkey bound to Insert/Home/... (TM's cameras use the pad).
void hotkeysKeyMessage(UINT msg, WPARAM vk, LPARAM lparam);
// Key capture for rebinding in the menu.
void keyCaptureBegin(int handle);
bool keyCapturePoll(int handle, std::string& out);
bool keyCaptureActive();

// ------------------------------------------------------------------ D3D9 (d3dhook.cpp) and overlay (overlay.cpp)
void d3dInstall();
void overlayCreateContext();
void texturesLoad(); // logo, plugin and other-mod icons (textures.cpp), after the device exists
void texturesReloadPluginIcons(); // after the icon style changed (host.json "ui.icons"), between frames
void* gameDevice();
void overlayDeviceCreated(void* device, HWND window, void* params);
void overlayBeforeReset();
void overlayAfterReset(bool ok);
void overlayFrame(void* device); // in Present, before the call on
bool menuOpen();
void setMenuOpen(bool open, const char* why);
// ABI minor 3 open_page: menu on the plugin's page (and panel tab), or closed again when it already shows it.
HorizonResult menuOpenPluginPage(const std::string& pluginId, const std::string& panelKey);
void* imguiContext();
void imguiAllocators(void** allocFn, void** freeFn, void** user);
const HorizonStyle* hostStyle();
const HorizonUiMetrics* hostUiMetrics(); // ABI minor 4
// Layout mode ("Edit layout"): every widget gets a frame to move, resize, minimise and hide.
bool layoutModeOn();
// why: a short reason when it may not start now (TMNF/TMUF during a race).
bool layoutAllowed(std::string* why);
void setLayoutMode(bool on, const char* why);
uint64_t createTextureRgba(uint32_t w, uint32_t h, const void* rgba); // game thread
void releaseTexture(uint64_t id);

// ------------------------------------------------------------------ input (dinput.cpp)
void dinputInstall();
// While true, the game's DirectInput keyboard/mouse reads return nothing.
void dinputSetBlocked(bool blocked);
void dinputHoldUntilReleased(int vk); // keeps one key away from the game until it is released

// ------------------------------------------------------------------ helpers (inlinehook.cpp)
struct InlineHook {
    uintptr_t link;
    unsigned char prologue[12];
    size_t length; // whole instructions, >= 5, no relative operands
    const char* name;
    void* trampoline;
};
uintptr_t rebase(uintptr_t link);
bool installInlineHook(InlineHook& h, void* detour);
bool writePointer(void** slot, void* value);
bool readPointer(const void* address, void** out);
void describeAddress(const void* address, char* out, size_t size);
bool patchImport(HMODULE module, const char* dll, const char* function, void* replacement, void** originalOut);

// ------------------------------------------------------------------ online fix, ESWC and Original (onlinefix.cpp)
// iatSlot: link address of the exe's HttpOpenRequestA import slot.
void onlineFixInstall(uintptr_t exeBase, uintptr_t iatSlot);

} // namespace horizon
