// Game adapter for TrackMania Nations/United Forever (loaded as the TMLoader product "Horizon Host"),
// and the null adapter (test program).
//
// Competition Patch 2.0 validates runs on its server: this adapter never hooks the game's simulation,
// timer, input or replay code, and never writes game memory. It only reads the game state through
// gamectl (reflection by name, read-only). DirectInput is not blocked here (CP replaces the game's input).
// Finish/checkpoint events for Forever are not wired yet (candidate addresses: tm-modding-knowledge
// modding/game-control.md; to be prologue-checked and kept read-only).
#include "host/host.h"

#include "common/util.h"
#include "host/game/glue.h"

namespace horizon {
namespace {

class ForeverAdapter final : public GameAdapter {
public:
    const char* name() const override { return "Forever"; }
    void install() override {
        known_ = gamectl::game() == gamectl::Game::Tmnf || gamectl::game() == gamectl::Game::Tmuf;
        HLOG("game: Forever adapter (%s): no game hooks, no input blocking; state and online %s", gamectl::gameName(),
             known_ ? "read by gamectl (reflection, read-only)" : "unknown for this exe: solo-only plugins stay paused");
    }
    HorizonGameState gameState() override { return known_ ? fromGamectl(gamectl::state()) : HORIZON_STATE_UNKNOWN; }
    int isOnline() override { return known_ && gamectl::isOnline() ? 1 : 0; }
    bool onlineKnown() const override { return known_; }
    bool raceRunning() override {
        if (!known_) return true; // unknown: treat as running (Competition Patch gate stays closed)
        const gamectl::State s = gamectl::state();
        return s == gamectl::State::SoloRace || s == gamectl::State::NetRace;
    }
    HorizonResult playMapFile(const char* path, uint32_t) override {
        if (!known_) return HORIZON_ERR_NOT_SUPPORTED;
        return fromGamectl(gamectl::playMapFile(wide(path).c_str()));
    }
    HorizonResult returnToMenu() override {
        if (!known_) return HORIZON_ERR_NOT_SUPPORTED;
        return fromGamectl(gamectl::returnToMenu());
    }
    bool usesDirectInputBlock() const override { return false; }
    std::string status() const override { return known_ ? gameStateText(const_cast<ForeverAdapter*>(this)->gameState()) : "state unknown"; }

private:
    bool known_ = false;
};

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
