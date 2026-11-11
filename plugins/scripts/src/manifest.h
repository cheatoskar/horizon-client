// A script's manifest: info.toml (or info.json) in its folder. Pure parsing and validation, no files.
//
//   name = "Clock"
//   author = "cheatoskar"
//   version = "1.0.0"
//   description = "Real time and session time."
//   games = ["eswc", "sunrise", "tmnf", "tmuf"]     # or "all"; missing = all
//   online = "online-ok"                             # or "solo-only" (default)
//   permissions = ["ui", "settings"]                 # ui, settings, read-game, game-control
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace hs {

enum Perm : uint32_t {
    PermUi = 0x01,          // UI namespace, Render() and RenderMenu()
    PermSettings = 0x02,    // [Setting] globals are shown and saved
    PermReadGame = 0x04,    // Game reads, Reflection, game event callbacks
    PermGameControl = 0x08, // Game::PlayMapFile / Game::ReturnToMenu (offline only)
};

enum class OnlineMode { SoloOnly, OnlineOk };

struct Manifest {
    std::string name, author, version, description;
    uint32_t games = 0;        // HORIZON_GAME_* bits
    OnlineMode online = OnlineMode::SoloOnly;
    uint32_t perms = 0;        // Perm bits
    std::vector<std::string> warnings; // unknown keys and the like: shown, not fatal
};

// Both return false with *error = "line 3: ..." (TOML) or a short text (JSON) on a broken manifest.
bool parseManifestToml(const std::string& text, Manifest* out, std::string* error);
bool parseManifestJson(const std::string& text, Manifest* out, std::string* error);

// "eswc" -> HORIZON_GAME_ESWC, "all" -> every supported game, 0 unknown.
uint32_t gameBits(const std::string& name);
// "ui" -> PermUi, 0 unknown.
uint32_t permBit(const std::string& name);
const char* permName(uint32_t bit);   // "ui", "settings", "read-game", "game-control"
const char* permLabel(uint32_t bit);  // "UI", "Settings", "Read game", "Game control"
std::string gamesText(uint32_t games); // "ESWC, Sunrise, TMNF, TMUF"

} // namespace hs
