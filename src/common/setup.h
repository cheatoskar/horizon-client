// The shared setup (one Horizon setup for every game install of a Windows user) and widget anchors.
// Pure logic, no Windows calls: which host.json keys stay per game, the export file, anchored positions.
// See docs/shared-setup.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace horizon {

class SettingsFile;

namespace setup {

// "plugin.<id>.x", "panel.<id>.x", "hotkey.<id>.x" -> "<id>"; any other key -> "".
std::string pluginOfKey(const std::string& key);

// True for a module made for some games only (plugin.json "games" not all five): its switch, windows, keys
// and settings stay with the game install. 0 = every game.
bool pluginGameSpecific(uint32_t manifestGames);

// Asks whether a module id is game specific (nullptr: none is).
using GameSpecificFn = bool (*)(const std::string& pluginId, void* user);

// True: the key stays in this game's own host.json even with the shared setup on. Per game are the TM Horizon
// pairing (bridge.*), the setup switch itself (setup.*), the login fix (OnlineFix), the free module windows
// in pixels (pwin.*) and everything of a game-specific module.
bool isLocalHostKey(const std::string& key, GameSpecificFn gameSpecific, void* user);

// True: the key goes into an export file. Never the pairing key or the setup switch.
bool isExportableHostKey(const std::string& key);

// setup.shared for an install without the key: a new install (no host.json yet) uses the shared setup, an
// existing one keeps its own until the player switches.
int sharedDefault(bool hostJsonExisted);

// Copies the keys pick() accepts from src to dst. replace: dst's own picked keys that src lacks are removed.
using PickFn = bool (*)(const std::string& key, void* user);
void copyKeys(const SettingsFile& src, SettingsFile& dst, PickFn pick, void* user, bool replace);

// ---- export file ----------------------------------------------------------------------------------------
// {"format": "horizon-setup", "version": 1, "host_version": "...", "game": "...",
//  "host": { flat host.json keys }, "plugins": { "<id>": { flat settings }, ... }}
struct PluginSettings {
    std::string id;
    std::string json; // a flat JSON object (SettingsFile::toText)
};
std::string exportText(const std::string& hostJson, const std::vector<PluginSettings>& plugins, const std::string& game,
                       const std::string& hostVersion);
struct Imported {
    std::string hostJson; // flat object text ("{}" when the file has no host part)
    std::vector<PluginSettings> plugins;
    std::string game, hostVersion;
};
// False with err for a file that is not a setup (or from a newer format). Plugin ids are checked.
bool importText(const std::string& text, Imported& out, std::string& err);

// ---- anchored widget positions -----------------------------------------------------------------------
// A widget's place relative to the nearest screen edge (or the centre) per axis, as a share of the screen, so
// it stays at the same edge at any window size. ax/ay: 0 left/top, 0.5 centre, 1 right/bottom.
// ox/oy: left/top: gap to that edge; right/bottom: gap to that edge; centre: offset of the widget's centre
// from the screen's centre. All divided by the screen width/height.
struct Anchor {
    float ax = 0.0f, ay = 0.0f, ox = 0.0f, oy = 0.0f;
    bool valid = false;
};
Anchor anchorOf(float x, float y, float w, float h, float screenW, float screenH);
// The top-left corner for a widget of size w x h on this screen, kept on the screen.
void placeAt(const Anchor& a, float w, float h, float screenW, float screenH, float& x, float& y);

} // namespace setup
} // namespace horizon
