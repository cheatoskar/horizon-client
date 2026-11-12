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
//   The game has no own path to switch the window mode while it runs (the only one is a developer menu command
//   whose id is never set), so the window mode is written to the config before the viewport reads it, i.e. it
//   applies at the next start.
// So:
//   at start   applyStart(): the chosen values go into the config the game is about to use (its own init applies
//              them to the audio port and the Direct3D device);
//   live       setMusic()/setSounds(): config field + port field (what ApplySystemConfig does), heard at once;
//              setFullscreen(): config field only, "Restart game".
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
};

const GameTable& eswcTable();
const GameTable& sunriseTable();
const GameTable& originalTable();
const GameTable* tableForTimestamp(uint32_t timestamp);

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
    bool readFullscreen(bool* fs);
    bool setFullscreen(bool fs); // config only: applies at the next start

    const GameTable* table() const { return table_; }

private:
    bool verify(const Part& part, const char* what, bool* done, bool* ok);
    uintptr_t rb(uintptr_t linkAddr) const { return linkAddr - 0x00400000 + base_; }
    uint8_t* config();
    uint8_t* display();
    uint8_t* port();
    bool readVolume(bool music, float* v);
    bool setVolume(bool music, float v);
    void logf(int level, const char* fmt, ...);

    const GameTable* table_;
    uintptr_t base_;
    LogFn log_;
    void* logUser_;
    bool audioDone_ = false, audioOk_ = false;
    bool displayDone_ = false, displayOk_ = false;
    bool loggedPort_ = false;
};

// Guarded memory access (false: the address cannot be read / written).
bool safeRead(const void* p, void* out, size_t n);
bool safeWrite(void* p, const void* in, size_t n);

// percent <-> 0..1 (rounded), clamped
int toPercent(float v);
float fromPercent(int pct);

} // namespace gs
