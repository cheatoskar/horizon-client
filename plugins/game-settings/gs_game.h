// Game settings: music and sound volume, window mode (TrackMania Nations ESWC 1.80, TrackMania Sunrise and
// TrackMania Original, same engine). Shared by the Horizon module (gs_plugin.cpp) and Legacy Essentials
// (vendored there by its tools/sync-horizon.ps1).
//
// What the game does (static analysis of the three exes, addresses below):
//   The settings live in the CSystemConfig the game loads from Default.SystemConfig.Gbx at start (written by the
//   launcher's "Configure" dialog): app+0x18. Its AudioSoundVolume / AudioMusicVolume (floats 0..1) and
//   Display (CSystemConfigDisplay, +0x20) with FullScreen (+0x34).
//   During start the game makes the input port first (that is where DirectInput8Create runs and the Horizon
//   loader / Legacy Essentials start), then the viewport from the config (Direct3D device, window mode), then
//   calls AudioPort->ApplySystemConfig(config) (vtable +0x8c), which copies the two volumes, clamped to 0..1,
//   into the audio port (CAudioPort SoundVolume +0x14, MusicVolume +0x18).
//   Every frame the port's update computes each playing buffer's DirectSound volume from those two fields
//   (music or sound), so a new value is heard at once.
//   Window mode: the viewport (app+0x3c ESWC, +0x44 Sunrise/Original) has StartFullscreen (vtable +0x9c) and
//   StopFullscreen (+0xa0); viewport+0x260 is "fullscreen now". The game's own start calls StopFullscreen then
//   StartFullscreen when the config says fullscreen (retrying with other modes if it fails); the window
//   procedure's developer command "ToggleFullscreen" calls one or the other while the game runs (that command's
//   id is never set in the shipped games). StartFullscreen saves the window's style and rectangle, builds the
//   fullscreen present parameters from the configured fullscreen mode and resets the device through the game's
//   own reset path (the one it uses after a lost device: resources released and made again); on failure it logs
//   "-> Fail" and goes back to windowed. StopFullscreen resets to windowed and restores style and rectangle.
// So:
//   at start   applyStart(): the chosen values go into the config the game is about to use (its own init applies
//              them to the audio port and the Direct3D device);
//   live       setMusic()/setSounds(): config field + port field (what ApplySystemConfig does), heard at once;
//              switchWindowMode(): the game's own StartFullscreen / StopFullscreen, like its developer command,
//              called on the game thread between two frames, and the config field so the next start matches.
// The game never writes these back to Default.SystemConfig.Gbx while it runs, so the caller keeps the chosen
// values (Horizon: module settings; Legacy Essentials: its ini) and they are applied again at every start.
// The launcher's own dialog keeps showing its value.
//
// Checks: per game, the code that reads the globals (code + pointer signatures) and the game's own reflection
// entries for every field used (class member id + offset). A mismatch turns off only that part (audio or
// window mode), logged once. Every read and write is guarded; a value that is not what the field should hold
// (a float outside 0..1, a bool other than 0/1) means "not that object": nothing is written.
#pragma once

#include <windows.h>

#include <stdint.h>

namespace gs {

struct CodeSig { // bytes at a link address
    uintptr_t addr;
    uint32_t len;
    const uint8_t* bytes;
};
struct PtrSig { // a dword at a link address holds a link address (a global the code uses)
    uintptr_t addr;
    uintptr_t value;
};
struct ParamSig { // the game's reflection entry for a class member: {type, member id, 0, offset, ...}
    uintptr_t addr;
    uint32_t type;
    uint32_t id;
    uint32_t offset;
};

struct Part { // what one feature relies on
    const CodeSig* code;
    int codeCount;
    const PtrSig* ptrs;
    int ptrCount;
    const ParamSig* params;
    int paramCount;
};

struct GameTable {
    const char* name;
    uint32_t timestamp;   // PE TimeDateStamp
    uintptr_t engines;    // global: engine manager; [+0x28] engine array, [+0x40] audio engine, its +0x2c audio port
    uintptr_t app;        // global: the application; +0x18 the CSystemConfig
    uint32_t cfgSound;    // CSystemConfig AudioSoundVolume
    uint32_t cfgMusic;    // CSystemConfig AudioMusicVolume
    Part audio;
    Part display;
    uint32_t vpOffset;    // app -> viewport (CHmsViewportDx9)
    uintptr_t vpVtable;   // its vtable; +0x9c StartFullscreen, +0xa0 StopFullscreen
    Part toggle;          // the live switch: both functions' code and the two vtable slots
};

const GameTable& eswcTable();
const GameTable& sunriseTable();
const GameTable& originalTable();
const GameTable* tableForTimestamp(uint32_t timestamp);

// The menu shows only what the game's own settings have: the games' options have Volume and Music, no separate
// sound volume, and a Sounds slider (AudioSoundVolume + the port's SoundVolume) was not heard on ESWC (my test
// 2026-11-13). false for all three games: no Sounds slider, and a saved Sounds value is not put into the game.
bool soundsSliderSupported(const GameTable* table);

// Chosen values; -1 = the game's own (nothing is applied).
struct Prefs {
    int music = -1;  // percent 0..100
    int sounds = -1; // percent 0..100
    int window = -1; // 0 windowed, 1 fullscreen
};

using LogFn = void (*)(void* user, int level, const char* text); // level 0 info, 1 warn, 2 error

class Game {
public:
    Game(const GameTable* table, uintptr_t exeBase, LogFn log, void* logUser);
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // Signatures, checked once each (logged). false: that part stays off.
    bool audioOk();
    bool displayOk();

    // Inside the game's DirectInput8Create at start: writes the chosen values into the config the game is about
    // to use. Logs what it did. Returns false if a chosen value could not be applied.
    bool applyStart(const Prefs& p);

    // Game thread. Volumes 0..1. read*: from the audio port once it exists, else from the config.
    bool readMusic(float* v);
    bool readSounds(float* v);
    bool setMusic(float v);
    bool setSounds(float v);
    bool readFullscreen(bool* fs);   // the config's value (what the next start uses)
    bool setFullscreen(bool fs);     // config only

    // Live window mode. toggleOk: signatures of the game's switch. fullscreenNow: the viewport's own flag.
    bool toggleOk();
    bool fullscreenNow(bool* fs);
    // Game thread, between two frames (not inside BeginScene/EndScene, not inside a Reset): switches with the
    // game's own StartFullscreen / StopFullscreen and writes the config to match. Returns true if the game is in
    // the wanted mode afterwards (false: the game refused or went back; logged).
    bool switchWindowMode(bool fullscreen);

    const GameTable* table() const { return table_; }

private:
    bool verify(const Part& part, const char* what, bool* done, bool* ok);
    uintptr_t rb(uintptr_t linkAddr) const { return linkAddr - 0x00400000 + base_; }
    uint8_t* config();
    uint8_t* display();
    uint8_t* port();
    uint8_t* viewport();
    bool readVolume(bool music, float* v);
    bool setVolume(bool music, float v);
    void logf(int level, const char* fmt, ...);

    const GameTable* table_;
    uintptr_t base_;
    LogFn log_;
    void* logUser_;
    bool audioDone_ = false, audioOk_ = false;
    bool displayDone_ = false, displayOk_ = false;
    bool toggleDone_ = false, toggleOk_ = false;
    bool loggedPort_ = false;
};

// Guarded memory access (false: the address cannot be read / written).
bool safeRead(const void* p, void* out, size_t n);
bool safeWrite(void* p, const void* in, size_t n);

// percent <-> 0..1 (rounded), clamped
int toPercent(float v);
float fromPercent(int pct);

// A slider being dragged. A new volume makes the game set the DirectSound volume of every playing sound again
// (and stop or restart sounds near 0 %), so a drag is applied at most every `ms` and the last value when it is
// let go, not on every frame.
class Throttle {
public:
    explicit Throttle(DWORD ms = 100) : ms_(ms) {}
    // the drag starts: `value` is what the game has now
    void begin(int value) {
        applied_ = value;
        any_ = false;
    }
    // this frame's slider value; `released`: the drag ended this frame. true: apply `value` now.
    bool due(int value, bool released, DWORD now) {
        if (value == applied_) return false;
        if (!released && any_ && now - last_ < ms_) return false;
        last_ = now;
        applied_ = value;
        any_ = true;
        return true;
    }

private:
    DWORD ms_;
    DWORD last_ = 0;
    int applied_ = -1;
    bool any_ = false;
};

// Runs fn(user) on the game's main thread the next time it takes a message from its queue, i.e. between two
// frames, where the game's own window procedure (and its developer "ToggleFullscreen" command) runs. Call it on
// the game thread (the first call installs a message hook for that thread). One call at a time: false while one
// is still waiting (for at most 2 s).
bool runBetweenFrames(void (*fn)(void* user), void* user);
// Before the DLL unloads (game thread): removes the hook, drops a call still waiting.
void stopBetweenFrames();

} // namespace gs
