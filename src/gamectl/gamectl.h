// Game control for old TrackMania games, host-agnostic (no hooks of its own, no UI): start a map file in solo,
// leave a solo race, read the game state and the current map. Used by the Horizon host (vendored, like
// reflect) and by plugins through the host API. Everything that changes game state must run on the game's
// main thread (host: RunOnGameThread) and only while the game is in the state the call asks for.
#pragma once

#include <stdint.h>

namespace gamectl {

enum class Game : uint32_t { Unknown = 0, Eswc = 1, Sunrise = 2, Original = 3, Tmnf = 4, Tmuf = 5 };

enum class State : uint32_t {
    Unknown = 0,
    Loading,     // starting up, no app/menus yet
    MainMenu,    // menu loop running, PlayMapFile allowed
    SoloRace,    // a local solo race is loaded (racing, end screen or in-race menu)
    NetRace,     // LAN/online: PlayMapFile and ReturnToMenu refuse
    Editor,
};

enum class Result : int32_t {
    Ok = 0,
    NotSupported = -1,   // unknown exe (TM Original: only the Online Patch v2 exe is known)
    WrongState = -2,     // e.g. PlayMapFile outside the main menu, or the menu is handling a button press
    FileError = -3,      // map file missing or copy into the game's Tracks folder failed
    NotFound = -4,       // after the rescan no challenge info (or more than one) has the copy's file name
    GameError = -5,      // an exception inside a game call (logged), nothing else changed if possible
};

struct MapInfo {
    char uid[64];
    char name[128];       // with TM format codes ($o, $fff...)
    char author[64];
    int32_t authorTime;   // ms, -1 if unknown
    int32_t goldTime, silverTime, bronzeTime;
    wchar_t path[260];    // file the game loaded, if known
};

using LogFn = void (*)(const char* line);

// Detects the game from the running exe (PE timestamp), sets up reflect. allowExperimentalOriginal runs a
// discovery log for unknown exes (nothing is controlled there). Call once on the game's main thread after the exe started (e.g. from the loader).
Game init(LogFn log, bool allowExperimentalOriginal);
Game game();
const char* gameName();

State state();
bool isOnline();

// The game's app object (CTrackMania*) read from the exe's global, nullptr while it does not exist yet or for an
// unknown exe. Read only.
void* appObject();

// Copies the map into GameData\Tracks\Challenges\Downloaded\_Horizon\ under a unique name, makes the game rescan
// its track folders once, finds the challenge info whose file is the copy (gamectl_pick.h) and does what the
// game's track browser does when a map is activated: SelectedChallengeInfo = info, ChallengeType 3 (solo), menu
// result 1 (see the comment above kSpecs in gamectl.cpp). Any menu page; not while a race, the editor or a network
// game is loaded. The race starts within the next frames; watch state() / the host's MapLoaded event.
Result playMapFile(const wchar_t* path);

// Presses the solo race's own quit button (CTrackManiaRace::ButtonQuitOnClick). SoloRace only.
Result returnToMenu();

// Map of the current solo race (or the last one). False if none.
bool currentMap(MapInfo* out);

// Removes old copies made by playMapFile (keeps the newest `keep`), call at startup or between maps.
void cleanupCopies(int keep);

} // namespace gamectl
