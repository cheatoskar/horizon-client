// Forever (TMNF/TMUF) event polling, pure logic: no game, no Windows. Memory comes in through the Mem interface, so
// the unit tests run it against fake memory, and the adapter (forever.cpp) gives it guarded reads of the real game.
//
// Reads only. Nothing here writes game memory or calls game code (Competition Patch 2.0 may treat that as tampering).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "horizon_plugin.h"

namespace horizon {
namespace fpoll {

// What an engine object is, decided by the adapter from its class name (reflection) or by the test's fake.
// EditorPuzzle: CTrackManiaEditorPuzzle, which TMNF/TMUF keep in the Editor slot under every solo and network race.
enum class Kind { Unknown, App, Challenge, RaceSolo, RaceNet, RaceOther, PlayerInfo, EditorReplay, EditorOther, EditorPuzzle };

struct Mem {
    virtual ~Mem() {}
    virtual bool read32(uintptr_t addr, uint32_t* out) = 0;                     // false: not readable
    virtual Kind kindOf(uintptr_t obj) = 0;                                      // Unknown: not an engine object
    virtual bool readWide(uintptr_t addr, uint32_t len, wchar_t* out) = 0;       // len wchar_t, false: not readable
};

// Member offsets. The defaults are the TMNF/TMUF values (identical reflection tables, tm-modding-knowledge
// games/tmnf/game-state-offsets.md + data/reflection/tmnf_classes.json); the adapter replaces each one the live
// reflection finds by name. -1: not available, the dependent event stays off.
struct Layout {
    int appChallenge = 0x198;   // CGameCtnApp.Challenge
    int appEditor = 0x414;      // CTrackMania.Editor
    int appRace = 0x454;        // the app's current race (no reflection member; gamectl "SetRace field")
    int raceTarget = 0x98;      // CTrackManiaRace.TMTargetPlayerInfo
    int raceCount = 0x24;       // fallback path: player count, array pointer, player -> info
    int racePlayers = 0x28;
    int playerInfo = 0x1c;
    int chName = 0x108;         // CGameCtnChallenge.ChallengeName (CFastStringInt {len, wchar_t*})
    int chParams = 0xb4;        // CGameCtnChallenge.ChallengeParameters
    int chPlayMode = 0xf8;      // CGameCtnChallenge.PlayMode
    int pAuthor = 0x20, pGold = 0x1c, pSilver = 0x18, pBronze = 0x14, pAuthorScore = 0x28; // ChallengeParameters
    int piState = 0x314, piTime = 0x2b0, piBest = 0x2b4; // CTrackManiaPlayerInfo RaceState, RaceTime, RaceBestTime
};

constexpr uint32_t kNoTime = 0xffffffffu;

struct Obs {
    bool app = false;                 // the app object exists and is a CTrackMania
    uintptr_t challenge = 0, race = 0, editor = 0, info = 0;
    uint32_t nameLen = 0;             // length of the challenge's name, 0 while not set yet
    Kind raceKind = Kind::Unknown;    // Unknown: no race
    Kind editorKind = Kind::Unknown;  // Unknown: no editor
    int infoPath = 0;                 // 1 = race.TMTargetPlayerInfo, 2 = race players[0].info
    bool haveInfo = false;
    uint32_t raceState = 0;           // 0 before the start, 1 running, 2 finished
    int32_t raceTime = 0;
    uint32_t best = kNoTime;
};

inline bool plausible(uint32_t p) { return p > 0x10000u; }

inline bool readPtr(Mem& m, uintptr_t base, int off, uintptr_t* out) {
    uint32_t v = 0;
    if (off < 0 || !m.read32(base + static_cast<uintptr_t>(off), &v) || !plausible(v)) return false;
    *out = v;
    return true;
}

// One poll of the game objects. False if the app object is not there (startup).
inline bool observe(Mem& m, const Layout& L, uintptr_t app, Obs* o) {
    *o = Obs();
    if (!plausible(static_cast<uint32_t>(app)) || m.kindOf(app) != Kind::App) return false;
    o->app = true;
    uintptr_t p = 0;
    if (readPtr(m, app, L.appChallenge, &p) && m.kindOf(p) == Kind::Challenge) {
        o->challenge = p;
        uint32_t len = 0;
        if (L.chName >= 0 && m.read32(p + static_cast<uintptr_t>(L.chName), &len) && len > 0 && len <= 512) o->nameLen = len;
    }
    if (readPtr(m, app, L.appEditor, &p)) {
        const Kind k = m.kindOf(p);
        if (k == Kind::EditorReplay || k == Kind::EditorOther || k == Kind::EditorPuzzle) {
            o->editor = p;
            o->editorKind = k;
        }
    }
    if (!readPtr(m, app, L.appRace, &p)) return true;
    const Kind rk = m.kindOf(p);
    if (rk != Kind::RaceSolo && rk != Kind::RaceNet && rk != Kind::RaceOther) return true;
    o->race = p;
    o->raceKind = rk;
    if (rk == Kind::RaceOther) return true;
    // the local player's info: the race's target player first, then the first entry of its player array
    uintptr_t info = 0;
    if (readPtr(m, o->race, L.raceTarget, &p) && m.kindOf(p) == Kind::PlayerInfo) {
        info = p;
        o->infoPath = 1;
    } else {
        uint32_t count = 0;
        uintptr_t arr = 0, player = 0;
        if (L.raceCount >= 0 && m.read32(o->race + static_cast<uintptr_t>(L.raceCount), &count) && count >= 1 && count <= 256 &&
            readPtr(m, o->race, L.racePlayers, &arr) && readPtr(m, arr, 0, &player) && readPtr(m, player, L.playerInfo, &p) &&
            m.kindOf(p) == Kind::PlayerInfo) {
            info = p;
            o->infoPath = 2;
        }
    }
    if (!info || L.piState < 0 || L.piTime < 0) return true;
    uint32_t st = 0, tm = 0, best = kNoTime;
    if (!m.read32(info + static_cast<uintptr_t>(L.piState), &st) || st > 2 || !m.read32(info + static_cast<uintptr_t>(L.piTime), &tm))
        return true;
    if (L.piBest >= 0) m.read32(info + static_cast<uintptr_t>(L.piBest), &best);
    o->info = info;
    o->haveInfo = true;
    o->raceState = st;
    o->raceTime = static_cast<int32_t>(tm);
    o->best = best;
    return true;
}

// The editor slot decides only when it is a real editor: a network race is never an editor test drive, and the
// puzzle editor the game keeps under a solo race is that race (as legacy.cpp's ESWC rule). Without a race, every
// editor counts (the puzzle editor's own screen too).
inline bool editorDecides(const Obs& o) {
    if (!o.editor || o.raceKind == Kind::RaceNet) return false;
    return !(o.editorKind == Kind::EditorPuzzle && o.raceKind == Kind::RaceSolo);
}

inline HorizonGameState stateOf(const Obs& o) {
    if (!o.app) return HORIZON_STATE_LOADING;
    if (editorDecides(o)) return o.editorKind == Kind::EditorReplay ? HORIZON_STATE_REPLAY : HORIZON_STATE_EDITOR;
    if (o.raceKind == Kind::RaceNet) return HORIZON_STATE_NET_RACE;
    if (o.raceKind == Kind::RaceSolo) return o.haveInfo && o.raceState == 2 ? HORIZON_STATE_END_RACE : HORIZON_STATE_SOLO_RACE;
    if (o.raceKind == Kind::RaceOther) return HORIZON_STATE_REPLAY;
    if (o.challenge) return HORIZON_STATE_LOADING; // a map is set, its race is not there yet
    return HORIZON_STATE_MAIN_MENU;
}

inline HorizonPlayMode playModeOf(const Obs& o) {
    if (editorDecides(o)) return o.editorKind == Kind::EditorReplay ? HORIZON_MODE_REPLAY : HORIZON_MODE_EDITOR_TEST;
    if (o.raceKind == Kind::RaceNet) return HORIZON_MODE_NET;
    if (o.raceKind == Kind::RaceSolo) return HORIZON_MODE_SOLO;
    return HORIZON_MODE_UNKNOWN;
}

// A time of RaceBestTime that means "a real best time".
inline bool validTime(uint32_t t) { return t > 0 && t < 0x80000000u; }

struct Step {
    bool mapGone = false;
    bool mapLoaded = false;
    bool raceStart = false;
    bool finish = false;
    int32_t finishTime = 0;
    bool pb = false;
};

// Turns successive observations into events. Race states: 0 before the start (countdown), 1 running, 2 finished.
class Tracker {
public:
    static constexpr int kMapWaitFrames = 90; // emit MAP_LOADED anyway when the name stays empty this long

    Step step(const Obs& o) {
        Step s;
        if (!o.app) return s;
        // map
        if (o.challenge != challenge_) {
            if (!o.challenge && challenge_) s.mapGone = true;
            challenge_ = o.challenge;
            pendingMap_ = o.challenge != 0;
            waited_ = 0;
            sessionBest_ = kNoTime;
        }
        if (pendingMap_ && (o.nameLen > 0 || ++waited_ >= kMapWaitFrames)) {
            pendingMap_ = false;
            s.mapLoaded = true;
        }
        // the local player's run (solo and network races; replays and editors give no events)
        if (!o.haveInfo || (o.raceKind != Kind::RaceSolo && o.raceKind != Kind::RaceNet)) {
            haveInfo_ = false;
            return s;
        }
        const bool same = haveInfo_ && race_ == o.race && info_ == o.info;
        // first sight of a run: a finished state is stale (e.g. the host started on the end screen), anything else counts from 0
        const uint32_t before = same ? state_ : (o.raceState == 2 ? 2u : 0u);
        if (before != 1 && o.raceState == 1) s.raceStart = true;
        else if (same && before == 1 && o.raceState == 1 && o.raceTime < time_) s.raceStart = true; // restart without a countdown
        if (before == 1 && o.raceState == 2) {
            s.finish = true;
            s.finishTime = o.raceTime;
            const uint32_t t = o.raceTime > 0 ? static_cast<uint32_t>(o.raceTime) : 0u;
            const uint32_t bestBefore = same ? best_ : kNoTime;
            s.pb = t > 0 && t < sessionBest_ && !(validTime(bestBefore) && bestBefore <= t);
            if (t > 0 && t < sessionBest_) sessionBest_ = t;
        }
        haveInfo_ = true;
        race_ = o.race;
        info_ = o.info;
        state_ = o.raceState;
        time_ = o.raceTime;
        best_ = o.best;
        return s;
    }

private:
    uintptr_t challenge_ = 0, race_ = 0, info_ = 0;
    bool pendingMap_ = false, haveInfo_ = false;
    int waited_ = 0;
    uint32_t state_ = 0, best_ = kNoTime, sessionBest_ = kNoTime;
    int32_t time_ = 0;
};

// Nadeo text formatting removed: "$rgb" colours, "$o $i $w $n $s $z $g $t $l $h ..." switches, "$$" -> "$".
inline std::wstring stripCodes(const wchar_t* buf, size_t len) {
    std::wstring clean;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != L'$') {
            clean += buf[i];
            continue;
        }
        if (i + 1 >= len) break;
        const wchar_t c = buf[i + 1];
        if (c == L'$') {
            clean += L'$';
            i++;
        } else if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')) {
            i += (i + 3 < len) ? 3 : len - i; // $rgb colour
        } else {
            i++;
        }
    }
    return clean;
}

// A time field of ChallengeParameters: 0 (map never validated) or garbage -> -1.
inline int32_t mapTime(uint32_t v) { return v > 0 && v < 0x7fffffffu ? static_cast<int32_t>(v) : -1; }

} // namespace fpoll
} // namespace horizon
