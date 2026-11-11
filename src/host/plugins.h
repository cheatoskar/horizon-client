// The host's view of a plugin (plugins.cpp). Used by the overlay to build the menu model.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/profile.h"
#include "common/settings.h"
#include "host/host.h"
#include "host/ui/ui.h"

namespace horizon {

struct Plugin {
    std::string id;
    std::wstring folder;
    PluginManifest manifest;
    std::string name, version, author, description;
    HMODULE module = nullptr;
    const HorizonPluginInfo* info = nullptr;
    HorizonPlugin_ShutdownFn shutdown = nullptr;
    HorizonPlugin_DrawMenuFn drawMenu = nullptr;
    HorizonPlugin_DrawOverlayFn drawOverlay = nullptr;
    HorizonPlugin_OnEventFn onEvent = nullptr;
    HorizonPlugin_SetEnabledFn setEnabled = nullptr;
    HorizonHostApi api = {};
    bool running = false;   // Init returned 0 and it has not crashed since
    bool enabled = false;   // gets calls (live switch position for live plugins)
    int wanted = -1;        // switch position chosen this start when it needs a restart (-1 none)
    bool live = false;
    bool soloOnly = true;
    ui::PluginStatus status = ui::PluginStatus::Off;
    std::string statusText;
    bool restartNote = false;
    std::string restartWhy;
    uint32_t eventMask = 0;
    SettingsFile settings;
    std::string dataFolder;
    std::wstring dataFolderW;
    HorizonDeviceCallbacks dev = {};
    bool hasDev = false;
    std::string hotkeyTextBuf;
    uint32_t hostFlags = 0; // set_host_flag (ABI minor 2)
    bool loadQueued = false; // switched on while the game runs: loaded at the start of the next frame
    bool loadedLate = false; // Init ran while the game was already running
};

std::vector<std::unique_ptr<Plugin>>& plugins();
const Profile& profile();
Plugin* findPlugin(const std::string& id);
bool pluginGated(const Plugin* p);
void pluginSetEnabled(const std::string& id, bool on);
bool pluginSwitchPosition(const Plugin* p);
// True when moving the switch now would only take effect after a restart (running without live toggle,
// or not loaded and "startup_only" in plugin.json).
bool pluginSwitchNeedsRestart(const Plugin* p);
// Loads the plugins switched on since the last frame (LoadLibrary, Init, device handed over). Game thread,
// outside the ImGui frame.
void pluginsLoadPending();
void pluginDrawMenu(const std::string& id);
void pluginCrashed(Plugin* p, const char* where); // stops calling it, shows "Failed"

} // namespace horizon
