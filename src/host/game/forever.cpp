// Game adapter for TrackMania Nations/United Forever (loaded as the TMLoader product "Horizon Host"),
// and the null adapter (test program).
//
// Competition Patch 2.0 validates runs on its server and may treat changed code bytes as tampering, so this
// adapter never hooks the game: no inline hook, no vtable or import patch in the game's code. The one exception is
// the keyboard while Horizon has it (host 0.3.2, dinput.cpp): without Competition Patch the method table of the
// system dinput8.dll devices gets the host's keyboard hold-back, so Enter/Backspace typed into the menu do not
// respawn the car; with Competition Patch loaded (it replaces the game's input) DirectInput is not touched at all.
// The game's events come from polling the game's objects once a frame
// (frame(), game thread): reads only, every read SEH-guarded, no write into game memory, no call into game code.
// What is read: CTrackMania (the app global of gamectl) -> Challenge, Editor, current race -> the local player's
// CTrackManiaPlayerInfo (RaceState, RaceTime, RaceBestTime), by reflection member names, TMNF/TMUF defaults when a
// member is missing. The logic is in forever_poll.h (unit tested with fake memory).
//
// Game control (play_map_file, return_to_menu) is gamectl's, as on the other games; that is the only code here
// that writes (the menu fields the game's own "play track" path sets) or calls game functions.
#include "host/host.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "common/util.h"
#include "host/game/forever_poll.h"
#include "host/game/glue.h"
#include "reflect/reflect.h"

namespace horizon {
namespace {

template <typename T> bool rd(const void* base, size_t off, T* out) {
    __try {
        *out = *reinterpret_cast<const T*>(static_cast<const char*>(base) + off);
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

fpoll::Kind classify(const reflect::ClassInfo* c) {
    using fpoll::Kind;
    if (!c) return Kind::Unknown;
    const char* n = reflect::className(c);
    if (reflect::isKindOf(c, "CTrackMania")) return Kind::App;
    if (reflect::isKindOf(c, "CGameCtnChallenge")) return Kind::Challenge;
    if (reflect::isKindOf(c, "CTrackManiaPlayerInfo")) return Kind::PlayerInfo;
    if (reflect::isKindOf(c, "CTrackManiaRace")) {
        // CTrackManiaRace1PGhosts is a Race1P (solo with medal ghosts)
        if (reflect::isKindOf(c, "CTrackManiaRace1P") || (n && strncmp(n, "CTrackManiaRace1P", 17) == 0)) return Kind::RaceSolo;
        if (n && strstr(n, "Net")) return Kind::RaceNet;
        return Kind::RaceOther;
    }
    if (n && (strstr(n, "Replay"))) return Kind::EditorReplay;
    // TMNF/TMUF keep one in the Editor slot through every solo and network race (logs 2026-11-11..13), as ESWC does
    if (reflect::isKindOf(c, "CTrackManiaEditorPuzzle") || (n && strcmp(n, "CTrackManiaEditorPuzzle") == 0)) return Kind::EditorPuzzle;
    if (reflect::isKindOf(c, "CGameCtnEditor") || (n && (strstr(n, "Editor") || strstr(n, "MediaTracker")))) return Kind::EditorOther;
    return Kind::Unknown;
}

const char* kindText(fpoll::Kind k) {
    switch (k) {
    case fpoll::Kind::App: return "app";
    case fpoll::Kind::Challenge: return "challenge";
    case fpoll::Kind::RaceSolo: return "solo race";
    case fpoll::Kind::RaceNet: return "network race";
    case fpoll::Kind::RaceOther: return "other race (replay/video)";
    case fpoll::Kind::PlayerInfo: return "player info";
    case fpoll::Kind::EditorReplay: return "replay editor";
    case fpoll::Kind::EditorOther: return "editor";
    case fpoll::Kind::EditorPuzzle: return "puzzle editor";
    default: return "not classified";
    }
}

// Guarded reads of the live game; the class of an object comes from the reflection tables (vtable slot read, nothing
// called), cached by vtable address.
class GameMem final : public fpoll::Mem {
public:
    bool read32(uintptr_t addr, uint32_t* out) override { return addr > 0x10000 && rd(reinterpret_cast<const void*>(addr), 0, out); }
    bool readWide(uintptr_t addr, uint32_t len, wchar_t* out) override {
        return addr > 0x10000 && len <= 512 && copyChars(out, reinterpret_cast<const wchar_t*>(addr), len);
    }
    fpoll::Kind kindOf(uintptr_t obj) override {
        uintptr_t vt = 0;
        if (obj <= 0x10000 || !rd(reinterpret_cast<const void*>(obj), 0, &vt) || vt <= 0x10000) return fpoll::Kind::Unknown;
        auto it = kinds_.find(vt);
        if (it != kinds_.end()) return it->second;
        const reflect::ClassInfo* c = reflect::classOfObject(reinterpret_cast<const void*>(obj));
        const fpoll::Kind k = classify(c);
        if (c) {
            const char* n = reflect::className(c);
            HLOG("game: object class %s (id %08x) = %s", n ? n : "?", reflect::classId(c), kindText(k));
        }
        kinds_[vt] = k;
        return k;
    }

private:
    std::unordered_map<uintptr_t, fpoll::Kind> kinds_;
};

class ForeverAdapter final : public GameAdapter {
public:
    const char* name() const override { return "Forever"; }

    void install() override {
        known_ = (gamectl::game() == gamectl::Game::Tmnf || gamectl::game() == gamectl::Game::Tmuf) && reflect::ready();
        if (!known_) {
            HLOG("game: Forever adapter (%s): %s: solo-only plugins stay paused, no game events", gamectl::gameName(),
                 reflect::ready() ? "exe not known to gamectl" : "reflection not available");
            return;
        }
        HLOG("game: Forever adapter (%s): polling only (guarded reads once a frame), no game hooks, no memory writes", gamectl::gameName());
        loadLayout();
    }

    void frame() override;
    HorizonGameState gameState() override { return known_ ? state_ : HORIZON_STATE_UNKNOWN; }
    int isOnline() override { return known_ && obs_.raceKind == fpoll::Kind::RaceNet ? 1 : 0; }
    bool onlineKnown() const override { return known_; }
    bool raceRunning() override {
        if (!known_) return true; // unknown: treat as running (Competition Patch gate stays closed)
        return obs_.raceKind == fpoll::Kind::RaceSolo || obs_.raceKind == fpoll::Kind::RaceNet;
    }
    // Forever (host 0.3.0): the file is handed to the game the way a second TmForever.exe started with
    // "/singleinst /file=<path>" does it (a double-clicked map): WM_COPYDATA with dwData 'ELIF' and the wide path
    // to the game window, sent from a short thread so the game handles it in its own message loop. No rescan of
    // the map list (gamectl's way froze TMUF for seconds with ~7200 maps), no memory write, no game call.
    // Only from the main menu and offline; the game opens the map a moment later (OK = handed over).
    HorizonResult playMapFile(const char* path, uint32_t) override {
        if (!known_) return HORIZON_ERR_NOT_SUPPORTED;
        if (isOnline()) return HORIZON_ERR_BLOCKED_ONLINE;
        if (gameState() != HORIZON_STATE_MAIN_MENU) return HORIZON_ERR_WRONG_STATE;
        const std::wstring w = wide(path ? path : "");
        wchar_t full[MAX_PATH] = {};
        const DWORD n = GetFullPathNameW(w.c_str(), MAX_PATH, full, nullptr);
        const DWORD attr = n && n < MAX_PATH ? GetFileAttributesW(full) : INVALID_FILE_ATTRIBUTES;
        const size_t len = wcslen(full);
        const wchar_t* ext = L".challenge.gbx";
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) || len < 15 || _wcsicmp(full + len - 14, ext) != 0)
            return HORIZON_ERR_INVALID_ARG;
        HWND win = overlayWindow();
        if (!win) return HORIZON_ERR_FAILED;
        std::wstring* arg = new std::wstring(full);
        HANDLE t = CreateThread(
            nullptr, 0,
            [](void* p) -> DWORD {
                std::wstring* s = static_cast<std::wstring*>(p);
                HWND target = overlayWindow();
                COPYDATASTRUCT cd = {};
                cd.dwData = 0x46494c45; // 'ELIF'
                cd.cbData = static_cast<DWORD>((s->size() + 1) * sizeof(wchar_t));
                cd.lpData = const_cast<wchar_t*>(s->c_str());
                DWORD_PTR result = 0;
                const LRESULT ok = target ? SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 15000, &result) : 0;
                HLOG("game: map handed to the game (single-instance file message): %s, answer %d", utf8(*s).c_str(), ok ? static_cast<int>(result) : -1);
                delete s;
                return 0;
            },
            arg, 0, nullptr);
        if (!t) {
            delete arg;
            return HORIZON_ERR_FAILED;
        }
        CloseHandle(t);
        return HORIZON_OK;
    }
    HorizonResult returnToMenu() override {
        if (!known_) return HORIZON_ERR_NOT_SUPPORTED;
        return fromGamectl(gamectl::returnToMenu());
    }
    // keyboard only (the mouse stays the game's as before), nothing with Competition Patch (hostcfg.cpp finds it
    // when the device is made, before dinputInstall runs)
    InputBlock directInputBlock() const override { return state().competitionPatch ? InputBlock::None : InputBlock::KeyboardOnly; }
    // The objects this frame's poll found (read only; plugins read them by reflection names).
    void* engine(uint32_t which) override {
        if (!known_) return nullptr;
        switch (which) {
        case HORIZON_ENGINE_APP: return gamectl::appObject();
        case HORIZON_ENGINE_RACE: return reinterpret_cast<void*>(obs_.race);
        case HORIZON_ENGINE_CHALLENGE: return reinterpret_cast<void*>(obs_.challenge);
        default: return nullptr;
        }
    }
    std::string status() const override { return status_; }

private:
    void loadLayout();
    void emitMapLoaded(const fpoll::Obs& o);
    void dispatch(HorizonEventType type, const fpoll::Obs& o, int32_t timeMs, bool pb);

    bool known_ = false;
    fpoll::Layout layout_;
    GameMem mem_;
    fpoll::Tracker tracker_;
    fpoll::Obs obs_;
    HorizonGameState state_ = HORIZON_STATE_LOADING;
    std::string status_ = "state unknown";
    int pathLogged_ = 0;
    int noInfoFrames_ = 0;
    bool noInfoLogged_ = false;
};

void setOff(int* slot, const char* cls, const char* member, std::string* log, bool* anyMissing) {
    const int v = reflect::offsetOf(cls, member);
    char buf[96];
    if (v >= 0) {
        *slot = v;
        snprintf(buf, sizeof(buf), "%s +0x%x, ", member, v);
    } else {
        snprintf(buf, sizeof(buf), "%s MISSING (default +0x%x), ", member, *slot);
        *anyMissing = true;
    }
    *log += buf;
}

void ForeverAdapter::loadLayout() {
    std::string log;
    bool missing = false;
    fpoll::Layout& L = layout_;
    setOff(&L.appChallenge, "CGameCtnApp", "Challenge", &log, &missing);
    setOff(&L.appEditor, "CTrackMania", "Editor", &log, &missing);
    setOff(&L.raceTarget, "CTrackManiaRace", "TMTargetPlayerInfo", &log, &missing);
    setOff(&L.chName, "CGameCtnChallenge", "ChallengeName", &log, &missing);
    setOff(&L.chParams, "CGameCtnChallenge", "ChallengeParameters", &log, &missing);
    setOff(&L.chPlayMode, "CGameCtnChallenge", "PlayMode", &log, &missing);
    setOff(&L.pAuthor, "CGameCtnChallengeParameters", "AuthorTime", &log, &missing);
    setOff(&L.pGold, "CGameCtnChallengeParameters", "GoldTime", &log, &missing);
    setOff(&L.pSilver, "CGameCtnChallengeParameters", "SilverTime", &log, &missing);
    setOff(&L.pBronze, "CGameCtnChallengeParameters", "BronzeTime", &log, &missing);
    setOff(&L.pAuthorScore, "CGameCtnChallengeParameters", "AuthorScore", &log, &missing);
    setOff(&L.piState, "CTrackManiaPlayerInfo", "RaceState", &log, &missing);
    setOff(&L.piTime, "CTrackManiaPlayerInfo", "RaceTime", &log, &missing);
    setOff(&L.piBest, "CTrackManiaPlayerInfo", "RaceBestTime", &log, &missing);
    HLOG("game: reflection offsets: %s", log.c_str());
    HLOG("game: fixed (no reflection member): app race +0x%x, race players +0x%x/+0x%x, player info +0x%x%s", L.appRace, L.raceCount,
         L.racePlayers, L.playerInfo,
         missing ? "; some members were not found, the TMNF/TMUF default offsets (identical in both exes) are used" : "");
}

void ForeverAdapter::emitMapLoaded(const fpoll::Obs& o) {
    HorizonMapInfo m = {};
    m.size = sizeof(m);
    std::string name;
    uint32_t ptr = 0;
    if (o.nameLen && mem_.read32(o.challenge + static_cast<uintptr_t>(layout_.chName) + 4, &ptr)) {
        wchar_t buf[520];
        if (mem_.readWide(ptr, o.nameLen, buf)) name = utf8(fpoll::stripCodes(buf, o.nameLen));
    }
    m.path = "";
    m.uid = "";
    m.name = name.c_str();
    m.author = "";
    m.environment = "";
    m.author_time = m.gold_time = m.silver_time = m.bronze_time = -1;
    m.author_score = -1;
    uintptr_t params = 0;
    if (fpoll::readPtr(mem_, o.challenge, layout_.chParams, &params)) {
        uint32_t v = 0;
        auto t = [&](int off) { return off >= 0 && mem_.read32(params + static_cast<uintptr_t>(off), &v) ? fpoll::mapTime(v) : -1; };
        m.author_time = t(layout_.pAuthor);
        m.gold_time = t(layout_.pGold);
        m.silver_time = t(layout_.pSilver);
        m.bronze_time = t(layout_.pBronze);
        m.author_score = t(layout_.pAuthorScore);
    }
    uint32_t mode = 0;
    if (layout_.chPlayMode >= 0 && mem_.read32(o.challenge + static_cast<uintptr_t>(layout_.chPlayMode), &mode)) m.is_stunts = mode == 5 ? 1u : 0u;
    m.play_mode = static_cast<uint32_t>(fpoll::playModeOf(o));
    // uid, author, path: from the map file when gamectl started it (PlayMapFile) and the names match. A map picked in
    // the game's own menu keeps them "" (the uid is an id index that only a game call resolves; not done here).
    gamectl::MapInfo gi = {};
    std::string uid, author, path;
    if (gamectl::currentMap(&gi)) {
        const std::wstring wn = wide(gi.name);
        if (utf8(fpoll::stripCodes(wn.c_str(), wn.size())) == name) {
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
    HorizonEvent e = {};
    e.type = HORIZON_EVENT_MAP_LOADED;
    e.play_mode = m.play_mode;
    e.engine_challenge = reinterpret_cast<void*>(o.challenge);
    dispatchEvent(e);
}

void ForeverAdapter::dispatch(HorizonEventType type, const fpoll::Obs& o, int32_t timeMs, bool pb) {
    HorizonEvent e = {};
    e.type = type;
    e.play_mode = static_cast<uint32_t>(fpoll::playModeOf(o));
    e.time_ms = timeMs;
    e.checkpoint_index = -1;
    e.respawns = -1;
    if (type == HORIZON_EVENT_FINISH) {
        e.is_local_player = 1;
        e.is_pb = pb ? 1u : 0u;
    }
    e.engine_race = reinterpret_cast<void*>(o.race);
    e.engine_challenge = reinterpret_cast<void*>(o.challenge);
    dispatchEvent(e);
}

void ForeverAdapter::frame() {
    if (!known_) return;
    fpoll::Obs o;
    void* app = gamectl::appObject();
    if (!app || !fpoll::observe(mem_, layout_, reinterpret_cast<uintptr_t>(app), &o)) o = fpoll::Obs();
    obs_ = o;
    state_ = fpoll::stateOf(o);
    char line[160];
    snprintf(line, sizeof(line), "%s; polling only", gameStateText(state_));
    status_ = line;
    if (o.infoPath && o.infoPath != pathLogged_) {
        pathLogged_ = o.infoPath;
        HLOG("game: the local player's info is found through %s",
             o.infoPath == 1 ? "race.TMTargetPlayerInfo" : "race players[0].info (fallback path)");
    }
    if ((o.raceKind == fpoll::Kind::RaceSolo || o.raceKind == fpoll::Kind::RaceNet) && !o.haveInfo) {
        if (++noInfoFrames_ == 180 && !noInfoLogged_) {
            noInfoLogged_ = true;
            HWARN("game: a race is running but the local player's info was not found (or its RaceState is out of range): "
                  "race start/finish events stay off");
        }
    } else {
        noInfoFrames_ = 0;
    }

    const fpoll::Step s = tracker_.step(o);
    if (s.mapGone) setCurrentMap(nullptr);
    if (s.mapLoaded) emitMapLoaded(o);
    if (s.raceStart) dispatch(HORIZON_EVENT_RACE_START, o, 0, false);
    if (s.finish) dispatch(HORIZON_EVENT_FINISH, o, s.finishTime, s.pb);
}

class NullAdapter final : public GameAdapter {
public:
    const char* name() const override { return "none"; }
    HorizonGameState gameState() override { return HORIZON_STATE_MAIN_MENU; }
};

GameAdapter* g_game = nullptr;

} // namespace

GameAdapter* makeForeverAdapter() { return new ForeverAdapter(); }
GameAdapter* makeNullAdapter() { return new NullAdapter(); }

GameAdapter& game() {
    if (!g_game) g_game = makeNullAdapter();
    return *g_game;
}

void setGame(GameAdapter* adapter) { g_game = adapter; }

} // namespace horizon
