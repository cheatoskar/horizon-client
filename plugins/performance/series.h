// Horizon Performance: the benchmark series (bench-request.json "series"). No Windows or Direct3D here, so the
// unit tests can drive the state machine with a fake host (tests/perf_test.cpp).
//
// A series runs several option sets one window after another: warm-up rounds first, then the measured
// rounds; the order of the sets is rotated every round (A B C, B C A, C A B ...) so slow drift (heat, background
// load) does not favour one set. Every set starts from "everything off" plus its own options, so a set is
// always compared with the baseline set. The user's own settings are restored at the end (or on abort).
#pragma once

#include <string>
#include <vector>

#include "perf_core.h"
#include "horizon_plugin.h"

namespace horizon {
struct Json;
}

namespace perf {

// Frames a series window leaves out (FrameSample::skip). Loading is not in it: the scene check covers that, and a
// series started by its key on a game without replay detection would otherwise see "loading" all the time.
constexpr uint8_t kSkipSeries = kSkipUnfocused | kSkipLost | kSkipSettle | kSkipScene;

// The options a set can switch. All 0 = everything off / "as the game sets it". Names as in the result files.
struct SeriesOptions {
    int count = 0, filter = 0, limitFps = 0, vsync = 0, priority = 0, affinity = 0, powerThrottlingOff = 0, fineTimer = 0;
    bool operator==(const SeriesOptions& o) const {
        return count == o.count && filter == o.filter && limitFps == o.limitFps && vsync == o.vsync && priority == o.priority &&
               affinity == o.affinity && powerThrottlingOff == o.powerThrottlingOff && fineTimer == o.fineTimer;
    }
    bool operator!=(const SeriesOptions& o) const { return !(*this == o); }
    bool allOff() const { return *this == SeriesOptions{}; }
};
std::string seriesOptionsJson(const SeriesOptions& o); // {"count": 0, ...}

struct SeriesSet {
    std::string label; // shown and used in file names (made safe)
    SeriesOptions options;
};

struct SeriesPlan {
    std::string where;  // "replay" (a replay is open), "map" (the plugin starts `map`), "now" (whatever is on screen)
    std::string replay; // replay path or name; "" = the newest one in Replays\Autosaves (resolved by the plugin)
    std::string map;
    double startAfter = -1; // "now": start this many seconds after the game start; -1 = when the start key is pressed
    double seconds = 20; // one window
    double settle = 2;   // after switching options, before recording
    int runs = 3, warmup = 1;
    bool returnToMenu = true;
    std::vector<SeriesSet> sets;
};

// baseline, limiter+vsync-off, priority-high, perf-cores, skip-repeated-states, fine-timer, no-power-throttling
std::vector<SeriesSet> defaultSeriesSets();
// j: the value of "series". Empty "sets" = the defaults. false (and why) when it is not usable.
bool parseSeries(const horizon::Json& j, SeriesPlan& out, std::string& why);

struct SeriesStep {
    int set = 0;      // index into the active sets
    int round = 0;    // 0-based, warm-up rounds first
    bool warmup = false;
    int run = 0;      // 1-based measured run (0 for warm-up)
};
// ---- start condition of a launcher request (bench-request.json), pure logic for the tests
// Scene: the game shows what the request asks for (replay: the host reports a replay; map: the solo race).
// Key: the player pressed the benchmark key (the plugin cannot see the scene, e.g. a replay on Sunrise, or
// on ESWC before the host knew the replay viewer); from then on the scene is not checked.
enum class StartBy { None, Scene, Key };
// gameState: HORIZON_STATE_*; keyPressed: the start key went down since the last frame.
StartBy benchStartBy(const std::string& where, int gameState, bool keyPressed);
// While the series runs: does this frame show what is measured?
bool benchSceneOk(const std::string& where, StartBy by, int gameState);

// rounds = warmup + runs; round r visits the sets starting at set (r % nSets), wrapping around
std::vector<SeriesStep> seriesOrder(int nSets, int runs, int warmup);

// What the plugin can do right now.
struct SeriesCaps {
    int vsyncInEffect = 0; // V-Sync is applied at device creation: it cannot change inside a series
};

// One window of the series (a skipped one has no summary).
struct SeriesRun {
    int set = 0;
    std::string label;
    int run = 0; // 1-based measured run, 0 = warm-up
    int warmupNo = 0;
    bool skipped = false;
    std::string skipReason;
    std::string note;
    std::string started;
    SeriesOptions applied;
    Summary summary;
    std::string file; // per-run file (set by the plugin)
};

class SeriesIO {
public:
    virtual ~SeriesIO() {}
    // Switch the options now (runtime ones only: the machine never changes V-Sync).
    virtual void apply(const SeriesOptions& o) = 0;
    // After the settle time: "" when everything the set asks for works, else why not (state filter blocked ...).
    virtual std::string unavailable(const SeriesOptions& o) = 0;
    // A short remark for the result ("no hybrid CPU found: all cores").
    virtual std::string note(const SeriesOptions& o) = 0;
    virtual std::string isoNow() = 0;
    // A window is complete (the frames are the recorded ones; not called for skipped windows).
    virtual void runDone(SeriesRun& r, const std::vector<FrameSample>& frames) = 0;
    // The series ended (the user's options are back already).
    virtual void finished(bool completed, const std::string& why) = 0;
};

class Series {
public:
    enum class Phase { Idle, Settle, Recording, Waiting, Done, Aborted };

    // user: the user's own options, put back at the end. Returns false (and why) with no usable set.
    bool start(const SeriesPlan& plan, const SeriesCaps& caps, const SeriesOptions& user, double nowMs, SeriesIO& io, std::string& why);
    // Once per frame while the series runs. frame: the frame just finished (null for a loading hitch). Settle and
    // window time count only frames without kSkipSeries bits: the window continues after a focus change, a replay
    // restart or a short loss of the scene instead of measuring it.
    void tick(double nowMs, const FrameSample* frame, SeriesIO& io);
    // The scene is gone (replay closed): drop the current window, wait for resume().
    void pause();
    void resume(double nowMs);
    void abort(const std::string& why, SeriesIO& io);

    bool active() const { return phase_ == Phase::Settle || phase_ == Phase::Recording || phase_ == Phase::Waiting; }
    bool waiting() const { return phase_ == Phase::Waiting; }
    Phase phase() const { return phase_; }
    std::string progress() const; // "Benchmark 7/21: priority-high, Lauf 2/3"
    int windowsLeft() const;       // this one included
    const std::vector<SeriesRun>& runs() const { return results_; }
    const SeriesPlan& plan() const { return plan_; }
    const std::vector<SeriesSet>& activeSets() const { return active_; }

private:
    void beginStep(double nowMs, SeriesIO& io);
    void advance(double nowMs, SeriesIO& io);
    void finish(bool completed, const std::string& why, SeriesIO& io);
    SeriesOptions effective(const SeriesOptions& wanted) const;

    Phase phase_ = Phase::Idle;
    SeriesPlan plan_;
    SeriesCaps caps_;
    SeriesOptions user_;
    std::vector<SeriesSet> active_;         // sets that can run (not restart-only)
    std::vector<int> unavailable_;          // per active set: 1 once found unavailable
    std::vector<SeriesStep> steps_;
    size_t step_ = 0;
    double t0_ = 0;
    double settledMs_ = 0, recordedMs_ = 0; // scene time in the current settle / window
    std::string started_;
    std::vector<FrameSample> frames_;
    std::vector<SeriesRun> results_;        // skipped sets first (run = 0, skipped), then every window in order
};

// Facts for the series file.
struct SeriesMeta {
    std::string stamp, started, finished;
    std::string game, gpu, cpu, resolution, replay, map, where;
    bool tmvs = false;
    bool completed = false;
    std::string abortedWhy;
    SeriesOptions user;
    int vsyncInEffect = 0;
    std::string tmvsNote;
};
std::string seriesJson(const SeriesPlan& plan, const std::vector<SeriesSet>& activeSets, const std::vector<SeriesRun>& runs,
                       const SeriesMeta& meta);

} // namespace perf
