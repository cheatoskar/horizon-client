// profile.json (written by TM Horizon before each start) and plugin.json (one per plugin folder).
// Format: docs/profile-json.md. Pure parsing, no Windows calls: unit-tested in tests/unit_tests.cpp.
#pragma once

#include <string>
#include <vector>

namespace horizon {

struct ProfilePlugin {
    std::string id;
    std::string version; // "" = any
    bool enabled = true;
};

struct Profile {
    bool present = false;   // the file exists
    bool valid = false;     // parsed and format understood
    std::string error;      // why not valid
    int format = 0;
    std::string name;
    std::string game;
    bool safeMode = false;
    std::string menuKey;    // "" = host default
    bool bar = true;
    std::string statsDir;   // "stats_dir": TM Horizon's folder for play events (host/stats.h), "" = none
    std::vector<ProfilePlugin> plugins;
    std::vector<std::string> warnings; // entries that were skipped and why
    std::string writtenBy, writtenAt;

    const ProfilePlugin* find(const std::string& id) const;
};

// Parses profile.json text. Never throws; sets valid/error/warnings.
Profile parseProfile(const std::string& text);

struct PluginManifest {
    bool valid = false;
    std::string error;
    std::string id, name, version, dll, description, author;
    std::string icon;        // "icon": PNG file in the plugin's folder (shown in the menu and bar), "" = initials
    unsigned games = 0;      // HORIZON_GAME_* mask (0 = not stated)
    bool soloOnly = true;    // "online": "solo-only" (default) / "online-ok"
    bool enabledByDefault = true; // "enabled_by_default": false = off until switched on (no profile.json)
    int abi = 0;
};

// Parses plugin.json. folderId: the folder name, which must equal "id".
PluginManifest parsePluginManifest(const std::string& text, const std::string& folderId);

// "eswc" -> HORIZON_GAME_ESWC etc., 0 for unknown names.
unsigned gameBitFromName(const std::string& name);
const char* gameKey(unsigned gameBit); // "eswc", "sunrise", ...

} // namespace horizon
