// Game adapter for TrackMania Nations ESWC (unofficial 1.80) and TrackMania Sunrise (Win10 repack).
//
// What the game runs is read once a frame (game thread, end of frame) from the game's objects, by
// reflection member NAMES (CConstructionGame.CurrentRace, .Challenge, CTrackMania.Editor, ...; ids and
// offsets differ per engine version): map loaded, game state, online (network race), and for ESWC the
// local player's start, respawn and checkpoint counters (offsets verified in TM Vibrant Shaders' ESWC
// port, wt-150 src/engine.cpp). Nothing is written, no game function is called by the poll.
//
// ESWC only, inline hooks with the prologue checked byte for byte (the same three hooks eswc-autosave
// has run in game since 1.0): CTrackManiaRace::OnFinishLine (FINISH), RaceNet::StopReplayRecordAndKeepCopy
// and the solo race end (RACE_END with the run's replay). Sunrise's matching functions are not verified
// yet: Sunrise gets the polled events only.
#include "host/host.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/util.h"
#include "host/game/glue.h"
#include "reflect/reflect.h"

namespace horizon {
namespace {

struct Exe {
    uint32_t game;
    uintptr_t appGlobal;  // CTrackMania* (stored by CConstructionGame's constructor)
};
// app globals: ESWC 0x00caee50 (TMVS engine.cpp, verified in game), Sunrise 0x00bcce50 (legacy-randomizer spike, static).
const Exe kEswc = {HORIZON_GAME_ESWC, 0x00caee50};
const Exe kSunrise = {HORIZON_GAME_SUNRISE, 0x00bcce50};

constexpr uint32_t kClassTrackMania = 0x24001000;

// ESWC 1.80 race/player members not exposed by reflection (TMVS engine.cpp, verified):
constexpr size_t kEswcRaceStarts = 0x4dc, kEswcRaceRespawns = 0x4e0, kEswcRaceApp = 0x40;
constexpr size_t kEswcNetworkInfos = 0x1a4, kEswcInfoPlayerType = 0xd8;
constexpr size_t kEswcInfoState = 0x25c, kEswcInfoTime = 0x1f4, kEswcInfoBest = 0x1f8;
constexpr size_t kEswcInfoCheckpoints = 0x244, kEswcInfoPassed = 0x284;
constexpr size_t kEswcPlayerInfo = 0x34;
constexpr size_t kEswcAppPrevReplay = 0x25c, kEswcAppCurReplay = 0x144;
constexpr uintptr_t kEswcLocalPlayerInfo = 0x0071c730; // CTrackManiaRace::GetLocalPlayerInfo()

enum RaceKind { kNone = 0, kSolo = 1, kNet = 2, kOther = 3 };

struct Offsets {
    int race = -1, challenge = -1, network = -1, editor = -1;
    int chName = -1, chParams = -1, chPlayMode = -1;
    int pAuthor = -1, pGold = -1, pSilver = -1, pBronze = -1, pAuthorScore = -1;
};

struct Snapshot {
    void* app = nullptr;
    void* race = nullptr;
    void* challenge = nullptr;
    void* editor = nullptr;
    int raceKind = kNone;
    bool editorIsReplay = false;
    void* info = nullptr; // ESWC: local player info
    uint32_t starts = 0, respawns = 0, passed = 0;
    int infoState = 0;
};

template <typename T> bool rd(const void* base, size_t off, T* out) {
    __try {
        *out = *reinterpret_cast<const T*>(static_cast<const char*>(base) + off);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// GetMwClassId is slot 2 of every engine class's vtable: `mov eax, <id>; ret`.
bool classIdOf(const void* nod, uint32_t* id) {
    __try {
        if (!nod) return false;
        const uintptr_t* vt = *static_cast<const uintptr_t* const*>(nod);
        const unsigned char* fn = reinterpret_cast<const unsigned char*>(vt[2]);
        if (fn[0] != 0xb8 || fn[5] != 0xc3) return false;
        *id = *reinterpret_cast<const uint32_t*>(fn + 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool copyChars(wchar_t* dst, const wchar_t* src, uint32_t len) {
    __try {
        memcpy(dst, src, len * sizeof(wchar_t));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string stripCodes(const wchar_t* buf, size_t len);

// Copies a CFastStringInt {len, wchar_t*} into UTF-8 with Nadeo $-codes removed.
std::string fastString(const void* obj, size_t off) {
    uint32_t len = 0;
    const wchar_t* ptr = nullptr;
    if (!rd(obj, off, &len) || !rd(obj, off + 4, &ptr) || !ptr || len == 0 || len > 512) return std::string();
    wchar_t buf[520];
    if (!copyChars(buf, ptr, len)) return std::string();
    buf[len] = 0;
    return stripCodes(buf, len);
}

std::string stripCodes(const wchar_t* buf, size_t len) {
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
            i++; // $o $i $w $n $s $z $g $t $l $h ...
        }
    }
    return utf8(clean);
}

struct Deferred {
    void* race;
    void* replay;
    bool solo;
};

class LegacyAdapter;
LegacyAdapter* g_self = nullptr;

class LegacyAdapter final : public GameAdapter {
public:
    explicit LegacyAdapter(const Exe& exe) : exe_(exe) { g_self = this; }
    const char* name() const override { return exe_.game == HORIZON_GAME_ESWC ? "ESWC" : "Sunrise"; }
    bool eswc() const { return exe_.game == HORIZON_GAME_ESWC; }

    void install() override {
        const bool r = reflect::ready(); // set up by gamectl::init (main.cpp)
        HLOG("game: reflection %s", r ? "ready (members by name)" : "NOT available for this exe: no game state");
        if (r) {
            o_.race = reflect::offsetOf("CConstructionGame", "CurrentRace");
            o_.challenge = reflect::offsetOf("CConstructionGame", "Challenge");
            o_.network = reflect::offsetOf("CConstructionGame", "Network");
            o_.editor = reflect::offsetOf("CTrackMania", "Editor");
            o_.chName = reflect::offsetOf("CConstructionChallenge", "ChallengeName");
            o_.chParams = reflect::offsetOf("CConstructionChallenge", "ChallengeParameters");
            o_.chPlayMode = reflect::offsetOf("CConstructionChallenge", "PlayMode");
            o_.pAuthor = reflect::offsetOf("CConstructionChallengeParameters", "AuthorTime");
            o_.pGold = reflect::offsetOf("CConstructionChallengeParameters", "GoldTime");
            o_.pSilver = reflect::offsetOf("CConstructionChallengeParameters", "SilverTime");
            o_.pBronze = reflect::offsetOf("CConstructionChallengeParameters", "BronzeTime");
            o_.pAuthorScore = reflect::offsetOf("CConstructionChallengeParameters", "AuthorScore");
            HLOG("game: CurrentRace +0x%x, Challenge +0x%x, Network +0x%x, Editor +0x%x, ChallengeName +0x%x, ChallengeParameters +0x%x",
                 o_.race, o_.challenge, o_.network, o_.editor, o_.chName, o_.chParams);
        }
        if (eswc()) installEswcHooks();
        else HLOG("game: Sunrise: finish/checkpoint/respawn events not available yet (functions not verified); map, state and online are");
    }

    void frame() override;
    HorizonGameState gameState() override { return state_; }
    int isOnline() override { return snap_.raceKind == kNet ? 1 : 0; }
    bool raceRunning() override { return snap_.race != nullptr; }
    void* engine(uint32_t which) override {
        Snapshot s;
        read(s);
        switch (which) {
        case HORIZON_ENGINE_APP: return s.app;
        case HORIZON_ENGINE_RACE: return s.race;
        case HORIZON_ENGINE_CHALLENGE: return s.challenge;
        default: return nullptr;
        }
    }
    // Map starts and the menu exit: legacy-randomizer's gamectl module (src/gamectl, vendored).
    HorizonResult playMapFile(const char* path, uint32_t) override { return fromGamectl(gamectl::playMapFile(wide(path).c_str())); }
    HorizonResult returnToMenu() override { return fromGamectl(gamectl::returnToMenu()); }
    std::string status() const override { return status_; }

    // ---- ESWC hooks (public for the detours)
    void onFinish(void* race, void* info, int stateBefore, uint32_t bestBefore);
    void onRaceEnd(void* race, void* replay, bool solo);
    void flushDeferred() {
        std::vector<Deferred> d;
        d.swap(deferred_);
        for (const auto& x : d) onRaceEnd(x.race, x.replay, x.solo);
    }
    int finishDepth = 0;

private:
    bool read(Snapshot& s);
    int raceKind(void* race);
    void loadNames();
    void emitMapLoaded(void* challenge);
    HorizonPlayMode playMode() const {
        if (snap_.editor && !snap_.editorIsReplay) return HORIZON_MODE_EDITOR_TEST;
        if (snap_.raceKind == kNet) return HORIZON_MODE_NET;
        if (snap_.raceKind == kSolo) return HORIZON_MODE_SOLO;
        if (snap_.editorIsReplay) return HORIZON_MODE_REPLAY;
        return HORIZON_MODE_UNKNOWN;
    }
    void installEswcHooks();

    Exe exe_;
    Offsets o_;
    Snapshot snap_;
    HorizonGameState state_ = HORIZON_STATE_UNKNOWN;
    std::unordered_map<uintptr_t, int> kinds_; // race vtable -> kind
    std::unordered_map<uint32_t, std::string> classNames_;
    bool haveSnap_ = false;
    std::string status_;
    std::vector<Deferred> deferred_;
    uint32_t sessionBest_ = 0xffffffffu;
};

void LegacyAdapter::loadNames() {
    if (!classNames_.empty() || !reflect::ready()) return;
    reflect::forEachClass(
        [](const reflect::ClassInfo* c, void* user) {
            auto* names = static_cast<std::unordered_map<uint32_t, std::string>*>(user);
            const char* n = reflect::className(c);
            if (n) (*names)[reflect::classId(c)] = n;
            return true;
        },
        &classNames_);
    HLOG("game: %zu engine classes found by reflection", classNames_.size());
}

int LegacyAdapter::raceKind(void* race) {
    if (!race) return kNone;
    uintptr_t vt = 0;
    if (!rd(race, 0, &vt)) return kNone;
    auto it = kinds_.find(vt);
    if (it != kinds_.end()) return it->second;
    int kind = kOther;
    uint32_t id = 0;
    if (classIdOf(race, &id)) {
        loadNames();
        auto n = classNames_.find(id);
        const std::string cls = n != classNames_.end() ? n->second : std::string();
        if (cls.rfind("CTrackManiaRace1P", 0) == 0 || cls == "CTrackManiaRace2PTurnBased") kind = kSolo;
        else if (cls.rfind("CTrackManiaRaceNet", 0) == 0) kind = kNet;
        HLOG("game: race class %s (id %08x) = %s", cls.empty() ? "?" : cls.c_str(), id,
             kind == kSolo ? "solo" : kind == kNet ? "network" : "other");
    }
    kinds_[vt] = kind;
    return kind;
}

bool LegacyAdapter::read(Snapshot& s) {
    s = Snapshot{};
    void* app = nullptr;
    if (!rd(reinterpret_cast<const void*>(rebase(exe_.appGlobal)), 0, &app) || !app) return false;
    uint32_t id = 0;
    if (!classIdOf(app, &id) || id != kClassTrackMania) return false;
    s.app = app;
    if (o_.challenge >= 0) rd(app, static_cast<size_t>(o_.challenge), &s.challenge);
    if (o_.editor >= 0) rd(app, static_cast<size_t>(o_.editor), &s.editor);
    if (s.editor) {
        uint32_t eid = 0;
        if (classIdOf(s.editor, &eid)) {
            loadNames();
            auto n = classNames_.find(eid);
            s.editorIsReplay = n != classNames_.end() && n->second.find("Replay") != std::string::npos;
        }
    }
    if (o_.race >= 0) rd(app, static_cast<size_t>(o_.race), &s.race);
    s.raceKind = raceKind(s.race);
    if (eswc() && s.race && s.raceKind != kOther) {
        rd(s.race, kEswcRaceStarts, &s.starts);
        rd(s.race, kEswcRaceRespawns, &s.respawns);
        void* network = nullptr;
        void** infos = nullptr;
        void* info = nullptr;
        int type = -1;
        if (o_.network >= 0 && rd(app, static_cast<size_t>(o_.network), &network) && network && rd(network, kEswcNetworkInfos, &infos) &&
            infos && rd(infos, 0, &info) && info && rd(info, kEswcInfoPlayerType, &type) && type == 0) {
            s.info = info;
            rd(info, kEswcInfoPassed, &s.passed);
            rd(info, kEswcInfoState, &s.infoState);
        }
    }
    return true;
}

void LegacyAdapter::emitMapLoaded(void* challenge) {
    HorizonMapInfo m = {};
    m.size = sizeof(m);
    std::string name = o_.chName >= 0 ? fastString(challenge, static_cast<size_t>(o_.chName)) : std::string();
    m.path = "";
    m.uid = "";
    m.name = name.c_str();
    m.author = "";
    m.environment = "";
    m.author_time = m.gold_time = m.silver_time = m.bronze_time = -1;
    m.author_score = -1;
    void* params = nullptr;
    if (o_.chParams >= 0 && rd(challenge, static_cast<size_t>(o_.chParams), &params) && params) {
        if (o_.pAuthor >= 0) rd(params, static_cast<size_t>(o_.pAuthor), &m.author_time);
        if (o_.pGold >= 0) rd(params, static_cast<size_t>(o_.pGold), &m.gold_time);
        if (o_.pSilver >= 0) rd(params, static_cast<size_t>(o_.pSilver), &m.silver_time);
        if (o_.pBronze >= 0) rd(params, static_cast<size_t>(o_.pBronze), &m.bronze_time);
        if (o_.pAuthorScore >= 0) rd(params, static_cast<size_t>(o_.pAuthorScore), &m.author_score);
    }
    int mode = 0;
    if (o_.chPlayMode >= 0 && rd(challenge, static_cast<size_t>(o_.chPlayMode), &mode)) m.is_stunts = mode == 5 ? 1u : 0u;
    m.play_mode = static_cast<uint32_t>(playMode());
    // uid, author, path: from the map file when gamectl started it (PlayMapFile) and the names match
    gamectl::MapInfo gi = {};
    std::string uid, author, path;
    if (gamectl::currentMap(&gi)) {
        const std::wstring wn = wide(gi.name);
        if (stripCodes(wn.c_str(), wn.size()) == name) {
            uid = gi.uid;
            author = gi.author;
            path = utf8(gi.path);
            m.uid = uid.c_str();
            m.author = author.c_str();
            m.path = path.c_str();
            if (m.author_time < 0) m.author_time = gi.authorTime;
            if (m.gold_time < 0) m.gold_time = gi.goldTime;
            if (m.silver_time < 0) m.silver_time = gi.silverTime;
            if (m.bronze_time < 0) m.bronze_time = gi.bronzeTime;
        }
    }
    setCurrentMap(&m);
    sessionBest_ = 0xffffffffu;
    HorizonEvent e = {};
    e.type = HORIZON_EVENT_MAP_LOADED;
    e.play_mode = m.play_mode;
    e.engine_challenge = challenge;
    dispatchEvent(e);
}

void LegacyAdapter::frame() {
    Snapshot s;
    const bool ok = read(s);
    const Snapshot prev = snap_;
    const bool hadSnap = haveSnap_;
    snap_ = s;
    haveSnap_ = ok;
    // state
    HorizonGameState st = HORIZON_STATE_UNKNOWN;
    if (!ok) st = HORIZON_STATE_LOADING;
    else if (s.editor && !s.editorIsReplay) st = HORIZON_STATE_EDITOR;
    else if (s.editor && s.editorIsReplay) st = HORIZON_STATE_REPLAY;
    else if (s.race && s.raceKind == kNet) st = HORIZON_STATE_NET_RACE;
    else if (s.race && s.raceKind == kSolo) st = s.infoState == 2 ? HORIZON_STATE_END_RACE : HORIZON_STATE_SOLO_RACE;
    else if (s.race) st = HORIZON_STATE_REPLAY; // a race of another class: replay viewer, video
    else if (!s.challenge) st = HORIZON_STATE_MAIN_MENU;
    else st = HORIZON_STATE_LOADING;
    state_ = st;
    char line[160];
    snprintf(line, sizeof(line), "%s; reflection %s", gameStateText(st), reflect::ready() ? "ready" : "off");
    status_ = line;
    if (!ok) return;

    // map
    if (s.challenge && s.challenge != prev.challenge) emitMapLoaded(s.challenge);
    if (!s.challenge && prev.challenge) setCurrentMap(nullptr);

    // local player's run (ESWC)
    if (!eswc() || !s.race || !s.info) return;
    const bool sameRace = hadSnap && prev.race == s.race && prev.info == s.info;
    const uint32_t prevStarts = sameRace ? prev.starts : 0;
    const uint32_t prevRespawns = sameRace ? prev.respawns : 0;
    const uint32_t prevPassed = sameRace ? prev.passed : 0;
    if (s.starts > prevStarts) {
        HorizonEvent e = {};
        e.type = HORIZON_EVENT_RACE_START;
        e.play_mode = static_cast<uint32_t>(playMode());
        e.engine_race = s.race;
        dispatchEvent(e);
    }
    if (s.respawns > prevRespawns && s.infoState == 1) {
        HorizonEvent e = {};
        e.type = HORIZON_EVENT_RESPAWN;
        e.play_mode = static_cast<uint32_t>(playMode());
        e.respawns = static_cast<int32_t>(s.respawns);
        e.engine_race = s.race;
        dispatchEvent(e);
    }
    if (s.passed > prevPassed && s.infoState == 1 && s.starts == prevStarts) {
        // the race time at each checkpoint: CFastBuffer {count, {time, score}*} at info+0x244
        uint32_t count = 0;
        const int32_t* times = nullptr;
        rd(s.info, kEswcInfoCheckpoints, &count);
        rd(s.info, kEswcInfoCheckpoints + 4, &times);
        for (uint32_t i = prevPassed; i < s.passed; i++) {
            int32_t t = -1;
            if (times && i < count && i < 512) rd(times, i * 8, &t);
            HorizonEvent e = {};
            e.type = HORIZON_EVENT_CHECKPOINT;
            e.play_mode = static_cast<uint32_t>(playMode());
            e.time_ms = t;
            e.checkpoint_index = static_cast<int32_t>(i);
            e.respawns = static_cast<int32_t>(s.respawns);
            e.engine_race = s.race;
            dispatchEvent(e);
        }
    }
}

void LegacyAdapter::onFinish(void* race, void* info, int stateBefore, uint32_t bestBefore) {
    int after = 0;
    if (!info || stateBefore != 1 || !rd(info, kEswcInfoState, &after) || after != 2) return;
    using FnLocal = void*(__fastcall*)(void*, void*);
    void* local = nullptr;
    __try {
        local = reinterpret_cast<FnLocal>(rebase(kEswcLocalPlayerInfo))(race, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        local = nullptr;
    }
    if (local != info) return;
    uint32_t time = 0, best = 0xffffffffu, respawns = 0;
    rd(info, kEswcInfoTime, &time);
    rd(info, kEswcInfoBest, &best);
    rd(race, kEswcRaceRespawns, &respawns);
    const bool pb = best != bestBefore || time < bestBefore;
    if (time < sessionBest_) sessionBest_ = time;
    HorizonEvent e = {};
    e.type = HORIZON_EVENT_FINISH;
    const int kind = raceKind(race);
    e.play_mode = kind == kNet ? HORIZON_MODE_NET : kind == kSolo ? HORIZON_MODE_SOLO : static_cast<uint32_t>(playMode());
    e.time_ms = static_cast<int32_t>(time);
    e.is_local_player = 1;
    e.is_pb = pb ? 1u : 0u;
    e.respawns = static_cast<int32_t>(respawns);
    e.engine_race = race;
    dispatchEvent(e);
}

void LegacyAdapter::onRaceEnd(void* race, void* replay, bool solo) {
    if (finishDepth > 0) {
        deferred_.push_back(Deferred{race, replay, solo});
        return;
    }
    HorizonEvent e = {};
    e.type = HORIZON_EVENT_RACE_END;
    e.play_mode = solo ? HORIZON_MODE_SOLO : HORIZON_MODE_NET;
    e.end_screen = solo ? 1u : 0u;
    e.engine_race = race;
    e.engine_replay = replay;
    dispatchEvent(e);
}

// ---------------------------------------------------------------- ESWC inline hooks
InlineHook g_finishHook = {0x006d9f50, {0x8b, 0x44, 0x24, 0x04, 0x83, 0xec, 0x1c}, 7, "CTrackManiaRace::OnFinishLine", nullptr};
InlineHook g_keepCopyHook = {0x0074ab60, {0x51, 0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x40}, 7, "RaceNet::StopReplayRecordAndKeepCopy", nullptr};
InlineHook g_soloEndHook = {0x006c5060, {0x83, 0xec, 0x08, 0x53, 0x55, 0x56, 0x57}, 7, "CTrackManiaRace1P race end", nullptr};

using FnFinish = void(__fastcall*)(void* race, void* edx, void* player);
using FnRaceVoid = void(__fastcall*)(void* race, void* edx);

void __fastcall finishDetour(void* race, void* edx, void* player) {
    void* info = nullptr;
    int stateBefore = 0;
    uint32_t bestBefore = 0xffffffffu;
    if (player && rd(player, kEswcPlayerInfo, &info) && info) {
        rd(info, kEswcInfoState, &stateBefore);
        rd(info, kEswcInfoBest, &bestBefore);
    }
    g_self->finishDepth++;
    reinterpret_cast<FnFinish>(g_finishHook.trampoline)(race, edx, player);
    g_self->finishDepth--;
    g_self->onFinish(race, info, stateBefore, bestBefore);
    if (g_self->finishDepth == 0) g_self->flushDeferred(); // RACE_END that happened inside OnFinishLine: after FINISH
}

void* appOf(void* race) {
    void* app = nullptr;
    return race && rd(race, kEswcRaceApp, &app) ? app : nullptr;
}

void __fastcall keepCopyDetour(void* race, void* edx) {
    void* app = appOf(race);
    void* before = nullptr;
    if (app) rd(app, kEswcAppPrevReplay, &before);
    reinterpret_cast<FnRaceVoid>(g_keepCopyHook.trampoline)(race, edx);
    void* after = nullptr;
    if (!app || !rd(app, kEswcAppPrevReplay, &after) || !after || after == before) return;
    g_self->onRaceEnd(race, after, false);
}

void __fastcall soloEndDetour(void* race, void* edx) {
    reinterpret_cast<FnRaceVoid>(g_soloEndHook.trampoline)(race, edx);
    void* app = appOf(race);
    void* replay = nullptr;
    if (!app || !rd(app, kEswcAppCurReplay, &replay)) return;
    g_self->onRaceEnd(race, replay, true);
}

void LegacyAdapter::installEswcHooks() {
    const bool a = installInlineHook(g_finishHook, reinterpret_cast<void*>(&finishDetour));
    const bool b = installInlineHook(g_keepCopyHook, reinterpret_cast<void*>(&keepCopyDetour));
    const bool c = installInlineHook(g_soloEndHook, reinterpret_cast<void*>(&soloEndDetour));
    HLOG("game: ESWC events: finish %s, network race end %s, solo race end %s; start/checkpoint/respawn/map/state by polling", a ? "on" : "OFF",
         b ? "on" : "OFF", c ? "on" : "OFF");
}

} // namespace

GameAdapter* makeEswcAdapter() { return new LegacyAdapter(kEswc); }
GameAdapter* makeSunriseAdapter() { return new LegacyAdapter(kSunrise); }

} // namespace horizon
