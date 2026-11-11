// Game adapter for TrackMania Nations ESWC (unofficial 1.80), TrackMania Sunrise (Win10 repack) and TrackMania
// Original (Online Patch v2 community exe; same engine as Sunrise, same reflection member offsets for the classes
// read here, race virtuals at the same vtable slots: trackmania-internals games/tm-original).
//
// What the game runs is read once a frame (game thread, end of frame) from the game's objects, by
// reflection member NAMES (CConstructionGame.CurrentRace, .Challenge, CTrackMania.Editor, ...; ids and
// offsets differ per engine version): map loaded, game state, online (network race), and for ESWC the
// local player's start, respawn and checkpoint counters (offsets verified in TM Vibrant Shaders' ESWC
// port, wt-150 src/engine.cpp). Nothing is written, no game function is called by the poll.
//
// ESWC only, inline hooks with the prologue checked byte for byte (the same three hooks eswc-autosave
// has run in game since 1.0): CTrackManiaRace::OnFinishLine (FINISH), RaceNet::StopReplayRecordAndKeepCopy
// and the solo race end (RACE_END with the run's replay). Sunrise and Original: OnStartLine, OnFinishLine,
// OnCheckpoint and the solo race end, static addresses, each hooked only when its prologue matches byte for byte.
#include "host/host.h"
#include "host/game/eswc_save.h"

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
    uintptr_t localPlayerInfo; // CTrackManiaRace::GetLocalPlayerInfo()
    // Sunrise/Original race hooks (0 = none): CTrackManiaRace::OnStartLine, OnFinishLine, OnCheckpoint (vtable slots
    // 86, 87, 89) and CTrackManiaRace1P's solo race end (slot 88)
    uintptr_t onStart, onFinish, onCheckpoint, soloEnd;
};
// app globals: ESWC 0x00caee50 (TMVS engine.cpp, verified in game), Sunrise 0x00bcce50 (legacy-randomizer spike, static),
// Original 0x00bc8084 (written by CConstructionGame's constructor 0x813b00, matched to Sunrise's 0x815640; static).
const Exe kEswc = {HORIZON_GAME_ESWC, 0x00caee50, 0x0071c730, 0, 0, 0, 0};
// Sunrise (repack, PE 5cc8f0cf): tm-modding-knowledge games/tm-sunrise/race-events.md (static, matched from
// the ESWC race vtables; hooked only behind the byte-exact prologue check). Player info members by reflection.
const Exe kSunrise = {HORIZON_GAME_SUNRISE, 0x00bcce50, 0x0082aeb0, 0x007ec550, 0x007ec7a0, 0x007ec5b0, 0x007dd7b0};
// Original (Online Patch v2, PE 5d337477): race vtable 0xa484f8, Race1P vtable 0xa48138, same slots and prologues as
// Sunrise (trackmania-internals games/tm-original/addresses.md; static, not run yet).
const Exe kOriginal = {HORIZON_GAME_ORIGINAL, 0x00bc8084, 0x00829200, 0x007eab20, 0x007ead30, 0x007eab80, 0x007dc250};

constexpr uint32_t kClassTrackMania = 0x24001000;

// ESWC 1.80 race/player members not exposed by reflection (TMVS engine.cpp, verified):
constexpr size_t kEswcRaceStarts = 0x4dc, kEswcRaceRespawns = 0x4e0, kEswcRaceApp = 0x40;
constexpr size_t kEswcNetworkInfos = 0x1a4, kEswcInfoPlayerType = 0xd8;
constexpr size_t kEswcInfoState = 0x25c, kEswcInfoTime = 0x1f4, kEswcInfoBest = 0x1f8;
constexpr size_t kEswcInfoCheckpoints = 0x244, kEswcInfoPassed = 0x284;
constexpr size_t kEswcPlayerInfo = 0x34;
constexpr size_t kEswcAppPrevReplay = 0x25c, kEswcAppCurReplay = 0x144;
// A replay watched from the menu (or shot as a video) runs in the app's clip viewer, not in CurrentRace and not in
// the Editor slot: CConstructionGame +0x128, class CCtnMediaClipViewer 0x2408a000 (no reflection member; made at
// 0x006ea968 / 0x006e74fc, cleared when the replay ends; TMVS wt-150 engine.cpp kEswcGameClipViewer, in game since
// TMVS 1.5). Sunrise's slot is not known: Sunrise reports a watched replay as "Loading" (the benchmark then starts
// by its key).
constexpr size_t kEswcAppClipViewer = 0x128;
constexpr uint32_t kEswcClassClipViewer = 0x2408a000;

enum RaceKind { kNone = 0, kSolo = 1, kNet = 2, kOther = 3 };

struct Offsets {
    int race = -1, challenge = -1, network = -1, editor = -1;
    int chName = -1, chParams = -1, chPlayMode = -1;
    int pAuthor = -1, pGold = -1, pSilver = -1, pBronze = -1, pAuthorScore = -1;
    // CTrackManiaPlayerInfo (ESWC: 0x25c/0x1f4/0x1f8/0x1f0/0x258, Sunrise: 0x228/0x1c0/0x1c4/0x1bc/0x224)
    int piState = -1, piTime = -1, piBest = -1, piStartTime = -1, piCurTime = -1;
};

// Sunrise: the local player's info and race, as the race hooks saw them; checkpoint counter of the run.
void* g_sunRace = nullptr;
void* g_sunInfo = nullptr;
int g_sunCheckpoints = 0;

struct Snapshot {
    void* app = nullptr;
    void* race = nullptr;
    void* challenge = nullptr;
    void* editor = nullptr;
    int raceKind = kNone;
    bool editorIsReplay = false;
    bool clipViewer = false; // ESWC: a replay plays in the app's clip viewer
    void* info = nullptr; // local player info (ESWC: polled; Sunrise: from the race hooks)
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
    const char* name() const override {
        return exe_.game == HORIZON_GAME_ESWC ? "ESWC" : exe_.game == HORIZON_GAME_ORIGINAL ? "Original" : "Sunrise";
    }
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
            o_.piState = reflect::offsetOf("CTrackManiaPlayerInfo", "RaceState");
            o_.piTime = reflect::offsetOf("CTrackManiaPlayerInfo", "RaceTime");
            o_.piBest = reflect::offsetOf("CTrackManiaPlayerInfo", "RaceBestTime");
            o_.piStartTime = reflect::offsetOf("CTrackManiaPlayerInfo", "RaceStartTime");
            o_.piCurTime = reflect::offsetOf("CTrackManiaPlayerInfo", "CurrentTime");
            HLOG("game: CTrackManiaPlayerInfo RaceState +0x%x, RaceTime +0x%x, RaceBestTime +0x%x, RaceStartTime +0x%x, CurrentTime +0x%x",
                 o_.piState, o_.piTime, o_.piBest, o_.piStartTime, o_.piCurTime);
            HLOG("game: CurrentRace +0x%x, Challenge +0x%x, Network +0x%x, Editor +0x%x, ChallengeName +0x%x, ChallengeParameters +0x%x",
                 o_.race, o_.challenge, o_.network, o_.editor, o_.chName, o_.chParams);
        }
        // offsets the finish code reads: ESWC's verified constants, Sunrise by reflection
        stateOff = eswc() ? static_cast<int>(kEswcInfoState) : o_.piState;
        timeOff = eswc() ? static_cast<int>(kEswcInfoTime) : o_.piTime;
        bestOff = eswc() ? static_cast<int>(kEswcInfoBest) : o_.piBest;
        if (eswc()) installEswcHooks();
        else installSunriseHooks();
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
    HorizonResult saveReplay(const std::wstring& stem, std::string* pathOut, std::string* why) override;
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
    int stateOff = -1, timeOff = -1, bestOff = -1;
    void* localInfo(void* race);
    void onSunStart(void* race, void* player);
    void onSunCheckpoint(void* race, void* player);

private:
    bool read(Snapshot& s);
    int raceKind(void* race);
    void loadNames();
    void emitMapLoaded(void* challenge);
    HorizonPlayMode playMode() const {
        if (snap_.editor && !snap_.editorIsReplay) return HORIZON_MODE_EDITOR_TEST;
        if (snap_.raceKind == kNet) return HORIZON_MODE_NET;
        if (snap_.raceKind == kSolo) return HORIZON_MODE_SOLO;
        if (snap_.editorIsReplay || (snap_.clipViewer && !snap_.race)) return HORIZON_MODE_REPLAY;
        return HORIZON_MODE_UNKNOWN;
    }
    void installEswcHooks();
    void installSunriseHooks();

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
    if (eswc()) {
        void* viewer = nullptr;
        uint32_t vid = 0;
        s.clipViewer = rd(app, kEswcAppClipViewer, &viewer) && viewer && classIdOf(viewer, &vid) && vid == kEswcClassClipViewer;
    }
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
    if (!eswc() && s.race && s.race == g_sunRace && g_sunInfo) {
        s.info = g_sunInfo;
        if (o_.piState >= 0) rd(g_sunInfo, static_cast<size_t>(o_.piState), &s.infoState);
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
    if (!eswc() && s.race != g_sunRace) {
        g_sunRace = nullptr;
        g_sunInfo = nullptr;
    }
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
    else if (s.clipViewer) st = HORIZON_STATE_REPLAY; // ESWC: a replay watched from the menu
    else st = HORIZON_STATE_LOADING;
    state_ = st;
    if (ok && s.clipViewer != prev.clipViewer && hadSnap)
        HLOG("game: %s", s.clipViewer ? "a replay plays (the app's clip viewer)" : "the replay's clip viewer is gone");
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

void* LegacyAdapter::localInfo(void* race) {
    using FnLocal = void*(__fastcall*)(void*, void*);
    void* local = nullptr;
    __try {
        local = reinterpret_cast<FnLocal>(rebase(exe_.localPlayerInfo))(race, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        local = nullptr;
    }
    return local;
}

void LegacyAdapter::onFinish(void* race, void* info, int stateBefore, uint32_t bestBefore) {
    int after = 0;
    if (!info || stateOff < 0 || stateBefore != 1 || !rd(info, static_cast<size_t>(stateOff), &after) || after != 2) return;
    if (localInfo(race) != info) return;
    if (!eswc()) {
        g_sunRace = race;
        g_sunInfo = info;
    }
    uint32_t time = 0, best = 0xffffffffu;
    int32_t respawns = -1; // Sunrise: not known yet
    if (timeOff >= 0) rd(info, static_cast<size_t>(timeOff), &time);
    if (bestOff >= 0) rd(info, static_cast<size_t>(bestOff), &best);
    if (eswc()) rd(race, kEswcRaceRespawns, &respawns);
    const bool pb = best != bestBefore || time < bestBefore;
    if (time < sessionBest_) sessionBest_ = time;
    HorizonEvent e = {};
    e.type = HORIZON_EVENT_FINISH;
    const int kind = raceKind(race);
    e.play_mode = kind == kNet ? HORIZON_MODE_NET : kind == kSolo ? HORIZON_MODE_SOLO : static_cast<uint32_t>(playMode());
    e.time_ms = static_cast<int32_t>(time);
    e.is_local_player = 1;
    e.is_pb = pb ? 1u : 0u;
    e.respawns = respawns;
    e.engine_race = race;
    dispatchEvent(e);
}

// Sunrise: OnStartLine / OnCheckpoint of the local player.
void LegacyAdapter::onSunStart(void* race, void* player) {
    void* info = nullptr;
    if (!player || !rd(player, kEswcPlayerInfo, &info) || !info || localInfo(race) != info) return;
    g_sunRace = race;
    g_sunInfo = info;
    g_sunCheckpoints = 0;
    HorizonEvent e = {};
    e.type = HORIZON_EVENT_RACE_START;
    const int kind = raceKind(race);
    e.play_mode = kind == kNet ? HORIZON_MODE_NET : kind == kSolo ? HORIZON_MODE_SOLO : static_cast<uint32_t>(playMode());
    e.engine_race = race;
    dispatchEvent(e);
}

void LegacyAdapter::onSunCheckpoint(void* race, void* player) {
    void* info = nullptr;
    if (!player || !rd(player, kEswcPlayerInfo, &info) || !info || localInfo(race) != info) return;
    g_sunRace = race;
    g_sunInfo = info;
    // race time at the checkpoint: CurrentTime - RaceStartTime (not verified in game yet; -1 if implausible)
    int32_t now = 0, start = 0, t = -1;
    if (o_.piCurTime >= 0 && o_.piStartTime >= 0 && rd(info, static_cast<size_t>(o_.piCurTime), &now) &&
        rd(info, static_cast<size_t>(o_.piStartTime), &start) && now >= start && now - start < 86400000)
        t = now - start;
    HorizonEvent e = {};
    e.type = HORIZON_EVENT_CHECKPOINT;
    const int kind = raceKind(race);
    e.play_mode = kind == kNet ? HORIZON_MODE_NET : kind == kSolo ? HORIZON_MODE_SOLO : static_cast<uint32_t>(playMode());
    e.time_ms = t;
    e.checkpoint_index = g_sunCheckpoints++;
    e.respawns = -1;
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
        if (g_self->stateOff >= 0) rd(info, static_cast<size_t>(g_self->stateOff), &stateBefore);
        if (g_self->bestOff >= 0) rd(info, static_cast<size_t>(g_self->bestOff), &bestBefore);
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

// ---------------------------------------------------------------- Sunrise / Original inline hooks
// Static addresses (link addresses set per exe from Exe before install); each one only when its prologue matches
// byte for byte. The prologues are the same in Sunrise and Original.
InlineHook g_sunStartHook = {0x007ec550, {0x8b, 0x44, 0x24, 0x04, 0x56}, 5, "Sunrise CTrackManiaRace::OnStartLine", nullptr};
InlineHook g_sunFinishHook = {0x007ec7a0, {0x8b, 0x44, 0x24, 0x04, 0x53, 0x55, 0x56, 0x33, 0xdb, 0x3b, 0xc3}, 11,
                              "Sunrise CTrackManiaRace::OnFinishLine", nullptr};
InlineHook g_sunCheckpointHook = {0x007ec5b0, {0x83, 0xec, 0x38, 0x53, 0x55, 0x8b, 0x6c, 0x24, 0x44, 0x56, 0x8b, 0xf1}, 12,
                                  "Sunrise CTrackManiaRace::OnCheckpoint", nullptr};
InlineHook g_sunSoloEndHook = {0x007dd7b0, {0x83, 0xec, 0x08, 0x53, 0x55, 0x56, 0x57, 0x8b, 0xf1}, 9, "Sunrise CTrackManiaRace1P race end", nullptr};

void __fastcall sunStartDetour(void* race, void* edx, void* player) {
    reinterpret_cast<FnFinish>(g_sunStartHook.trampoline)(race, edx, player);
    g_self->onSunStart(race, player);
}

void __fastcall sunCheckpointDetour(void* race, void* edx, void* player) {
    reinterpret_cast<FnFinish>(g_sunCheckpointHook.trampoline)(race, edx, player);
    g_self->onSunCheckpoint(race, player);
}

void __fastcall sunFinishDetour(void* race, void* edx, void* player) {
    void* info = nullptr;
    int stateBefore = 0;
    uint32_t bestBefore = 0xffffffffu;
    if (player && rd(player, kEswcPlayerInfo, &info) && info) {
        if (g_self->stateOff >= 0) rd(info, static_cast<size_t>(g_self->stateOff), &stateBefore);
        if (g_self->bestOff >= 0) rd(info, static_cast<size_t>(g_self->bestOff), &bestBefore);
    }
    g_self->finishDepth++;
    reinterpret_cast<FnFinish>(g_sunFinishHook.trampoline)(race, edx, player);
    g_self->finishDepth--;
    g_self->onFinish(race, info, stateBefore, bestBefore);
    if (g_self->finishDepth == 0) g_self->flushDeferred();
}

void __fastcall sunSoloEndDetour(void* race, void* edx) {
    reinterpret_cast<FnRaceVoid>(g_sunSoloEndHook.trampoline)(race, edx);
    g_self->onRaceEnd(race, nullptr, true); // the replay's place in the app is not known on Sunrise yet
}

void LegacyAdapter::installSunriseHooks() {
    g_sunStartHook.link = exe_.onStart;
    g_sunFinishHook.link = exe_.onFinish;
    g_sunCheckpointHook.link = exe_.onCheckpoint;
    g_sunSoloEndHook.link = exe_.soloEnd;
    g_sunStartHook.name = exe_.game == HORIZON_GAME_ORIGINAL ? "Original CTrackManiaRace::OnStartLine" : g_sunStartHook.name;
    g_sunFinishHook.name = exe_.game == HORIZON_GAME_ORIGINAL ? "Original CTrackManiaRace::OnFinishLine" : g_sunFinishHook.name;
    g_sunCheckpointHook.name = exe_.game == HORIZON_GAME_ORIGINAL ? "Original CTrackManiaRace::OnCheckpoint" : g_sunCheckpointHook.name;
    g_sunSoloEndHook.name = exe_.game == HORIZON_GAME_ORIGINAL ? "Original CTrackManiaRace1P race end" : g_sunSoloEndHook.name;
    const bool haveInfo = stateOff >= 0 && timeOff >= 0;
    const bool a = installInlineHook(g_sunStartHook, reinterpret_cast<void*>(&sunStartDetour));
    const bool b = haveInfo && installInlineHook(g_sunFinishHook, reinterpret_cast<void*>(&sunFinishDetour));
    const bool c = installInlineHook(g_sunCheckpointHook, reinterpret_cast<void*>(&sunCheckpointDetour));
    const bool d = installInlineHook(g_sunSoloEndHook, reinterpret_cast<void*>(&sunSoloEndDetour));
    HLOG("game: %s events (static addresses, first test): start %s, finish %s, checkpoint %s, solo race end %s; "
         "respawn not available, network race end not available",
         name(), a ? "on" : "OFF", b ? "on" : (haveInfo ? "OFF" : "OFF (player info members not found)"), c ? "on" : "OFF", d ? "on" : "OFF");
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
GameAdapter* makeOriginalAdapter() { return new LegacyAdapter(kOriginal); }

// save_replay (ESWC only; Sunrise and Original: not supported): the solo end-race screen saves the current run (as "Save replay" there);
// elsewhere (menus, LAN/online after a race) the previous race's copy (as "Save previous replay").
// A solo race still running has no finished replay yet.
HorizonResult LegacyAdapter::saveReplay(const std::wstring& stem, std::string* pathOut, std::string* why) {
    if (!eswc()) {
        *why = "not supported on Sunrise and Original yet";
        return HORIZON_ERR_NOT_SUPPORTED;
    }
    Snapshot s;
    read(s);
    if (!s.app) {
        *why = "game not ready";
        return HORIZON_ERR_WRONG_STATE;
    }
    if (state_ == HORIZON_STATE_SOLO_RACE || state_ == HORIZON_STATE_LOADING || state_ == HORIZON_STATE_EDITOR) {
        *why = "no finished run (race running, loading or editor)";
        return HORIZON_ERR_WRONG_STATE;
    }
    const bool solo = state_ == HORIZON_STATE_END_RACE;
    EswcSaveResult r = {};
    eswcSaveReplay(s.app, solo, stem.empty() ? nullptr : stem.c_str(), &r);
    if (r.ok == 1) {
        *pathOut = utf8(std::wstring(r.folder) + r.name);
        return HORIZON_OK;
    }
    *why = r.why ? r.why : "failed";
    return r.ok == 0 ? HORIZON_ERR_FAILED : HORIZON_ERR_WRONG_STATE;
}

} // namespace horizon
