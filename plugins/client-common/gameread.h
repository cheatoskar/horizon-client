// Client mods (WR Hunter, Input Display, TMX Browser): read-only access to the local player's run and car.
//
// Everything here only reads game memory, every read guarded; nothing is written, no game code is called,
// nothing is hooked (Competition Patch 2.0 rule on Forever). Offsets come from the engine reflection by
// class and member NAME (host->reflect_offset); where a value has no reflection member the TMNF/TMUF or
// ESWC/Sunrise default is used and the value is checked for plausibility before it is shown.
//
// The logic works on a Mem interface, so the unit tests run it against fake memory (tests/fake_world.*),
// and the plugins give it guarded reads of the live game (LiveMem).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "horizon_plugin.h"

namespace ck {

struct Mem {
    virtual ~Mem() {}
    virtual bool read(uintptr_t addr, void* out, size_t n) = 0;  // false: not readable
    // True when obj is an engine object of class cls (or derives from it). Unknown class tables: true when
    // the object at least looks like one (vtable readable).
    virtual bool isKind(uintptr_t obj, const char* cls) = 0;
    template <typename T> bool get(uintptr_t addr, T* out) { return addr > 0x10000 && read(addr, out, sizeof(T)); }
    bool ptr(uintptr_t base, int off, uintptr_t* out) {
        uint32_t v = 0;
        if (off < 0 || !get(base + static_cast<uintptr_t>(off), &v) || v <= 0x10000u) return false;
        *out = v;
        return true;
    }
};

enum class Engine { Legacy, Forever };

// Member offsets, by reflection name where there is one (resolve()), else the defaults below.
struct Layout {
    Engine engine = Engine::Forever;
    // race (CTrackManiaRace / CGameRace / CGamePlayground)
    int racePlayers = 0x24;       // CGamePlayground.Players {count, items} (ESWC: CConstructionRace.Players 0x20)
    int raceTarget = 0x98;        // CTrackManiaRace.TMTargetPlayerInfo (ESWC 0x10c)
    int raceSpeed = 0x26c;        // CTrackManiaRace.CarSpeedDisplay (km/h; Forever only)
    int raceClock = 0x274;        // no member: the race's clock (Forever, TMVS RE)
    int raceGhosts = 0x520;       // CTrackManiaRace.RaceGhosts {count, items} (Forever)
    // player (CGamePlayer)
    int playerInfo = 0x1c;        // CGamePlayer.PlayerInfo (ESWC: CConstructionPlayer.PlayerInfo 0x34)
    int playerMobil = 0x28;       // CGamePlayer.Mobil (ESWC 0x24)
    int mobilScene = 0x14;        // CGameMobil.SceneMobil (ESWC: CGameMobil.Mobil)
    // player info (CTrackManiaPlayerInfo)
    int piState = 0x314;          // RaceState (0 before the start, 1 running, 2 finished)
    int piTime = 0x2b0;           // RaceTime (the final time once finished)
    int piStart = 0x2ac;          // RaceStartTime
    int piNow = 0x308;            // CurrentTime (the clock the start time is on)
    int piCurCp = 0x2e4;          // CurCheckpoint
    int piCpList = 0x2f8;         // no member: {count, {time, score}*} race time at each checkpoint (Forever, TMVS RE)
    // ghost (CGameCtnGhost)
    int ghostTime = 0xe8;         // RaceTime
    int ghostCps = 0x94;          // no member: {count, {time, score}*} (TMVS RE)
    // vehicle
    const char* vehicleClass = "CSceneVehicleCar";
    int vSteer = 0x58, vGas = 0x50, vBrake = 0x54; // Forever defaults: TMInterface's SceneVehicleCar layout
    int vGear = 0x5c8, vRpm = 0x5b8;               // Engine block (TMInterface): gear (int), actual rpm (float); -1 none
    bool vNamed = false;          // input offsets came from reflection names
};

Layout defaultLayout(Engine e);
// Fills a layout from the host's reflection (by name). Missing members keep the default; *log lists them.
using OffsetFn = int (*)(const char* cls, const char* member); // -1 missing
Layout resolveLayout(Engine e, OffsetFn offsetOf, std::string* log);

constexpr int kMaxCps = 256;

struct Run {
    bool race = false;            // a race object with the local player
    int state = -1;               // 0 before the start, 1 running, 2 finished, -1 unknown
    int timeMs = -1;              // race time now (running), final (finished), -1 unknown
    std::vector<int> cps;         // race time at each checkpoint passed in this run (the finish last once finished)
    bool cpsFromList = false;     // read from the game's checkpoint list (exact); false: sampled when the counter moved
    uintptr_t raceObj = 0, infoObj = 0, playerObj = 0;
};

struct Car {
    bool ok = false;              // steer/gas/brake read and plausible
    float steer = 0, gas = 0, brake = 0;  // -1..1, 0..1, 0..1
    bool haveSpeed = false;
    float speedKmh = 0;
    bool haveGear = false;
    int gear = 0;
    bool haveRpm = false;
    float rpm = 0;
};

struct GhostTimes {
    int raceTime = -1;
    std::vector<int> cps;         // the finish last
};

class Reader {
public:
    explicit Reader(const Layout& L) : L_(L) {}
    const Layout& layout() const { return L_; }
    // race: the host's HORIZON_ENGINE_RACE object (0 none). Reads the local run and the car.
    void poll(Mem& m, uintptr_t race);
    const Run& run() const { return run_; }
    const Car& car() const { return car_; }
    // The best opponent ghost of the race (Forever: a medal, your best, a loaded replay), false none.
    bool bestGhost(Mem& m, GhostTimes* out) const;
    // Checkpoint list of a buffer {count, {time, score}*}: false when it does not look like one.
    static bool readTimes(Mem& m, uintptr_t buf, std::vector<int>* out);

private:
    uintptr_t localPlayer(Mem& m, uintptr_t race, uintptr_t* info);
    Layout L_;
    Run run_;
    Car car_;
    int lastCurCp_ = 0;
    int forgetTimer_ = 0;
};

// Plausible steer/gas/brake (rejects garbage when an offset is wrong).
inline bool plausibleInput(float steer, float gas, float brake) {
    return steer >= -1.01f && steer <= 1.01f && gas >= -0.01f && gas <= 1.01f && brake >= -0.01f && brake <= 1.01f &&
           steer == steer && gas == gas && brake == brake;
}

// Live game memory (SEH-guarded reads, classes from the reflection tables via the vendored reflect module).
class LiveMem : public Mem {
public:
    bool read(uintptr_t addr, void* out, size_t n) override;
    bool isKind(uintptr_t obj, const char* cls) override;
};

// Sets up the vendored reflect module for this exe (call once in Init). False: reflection not available.
bool liveReflectInit(const HorizonHostApi* host);
int liveOffset(const char* cls, const char* member);

} // namespace ck
