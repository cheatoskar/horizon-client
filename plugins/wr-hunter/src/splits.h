// WR Hunter: the split logic without game, ImGui or files. A run is a list of split times (the race time
// at each checkpoint, the finish last). Per map: personal best (its splits), gold sectors (the best time of
// each sector over all finished runs), the sum of best, attempts, resets and the last finished runs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wrh {

constexpr int kNoDelta = -2147483647 - 1;
constexpr size_t kMaxRuns = 100;

struct RunRecord {
    long long at = 0;          // unix seconds
    int time = -1;             // final time (ms)
    std::vector<int> splits;   // the finish last
    int respawns = -1;
};

struct MapStats {
    std::string key, uid, name;
    int attempts = 0, resets = 0, finishes = 0;
    std::vector<int> pb;       // splits of the personal best (empty: none)
    std::vector<int> golds;    // best sector times, as many as pb has splits
    std::vector<RunRecord> runs; // finished runs, newest last
    int pbTime() const { return pb.empty() ? -1 : pb.back(); }
};

// Sector times of a split list (split i minus split i-1).
std::vector<int> sectors(const std::vector<int>& splits);
// Sum of the gold sectors, -1 while one is missing.
int sumOfBest(const MapStats& s);

struct FinishResult {
    bool pb = false;
    bool firstRun = false;
    std::vector<bool> gold;    // per sector: a new gold
};
// A finished run: history, PB, golds. A run with another split count than the PB (another route,
// laps) goes into the history only. now: unix seconds.
FinishResult applyFinish(MapStats& s, const std::vector<int>& splits, int respawns, long long now);

// What to compare with.
struct Comparison {
    std::string name;          // "Personal best", "TMX record (Player)", ...
    int time = -1;
    std::vector<int> splits;   // empty: nothing to compare with
};

struct Row {
    int index = 0;             // 0-based split index
    bool finish = false;
    int time = -1;             // this run's split time, -1 not reached
    int pbSplit = -1, targetSplit = -1;
    int dPb = kNoDelta, dTarget = kNoDelta;
    int prevDPb = kNoDelta, prevDTarget = kNoDelta; // the delta at the split before (colour: gaining/losing)
    int sector = -1;           // this run's sector time
    int gold = -1;             // the gold of this sector (before this run)
    bool isGold = false;       // this run beat the gold here
};

// The table for the live run (splits so far) against the PB and the target. count: number of splits of the
// map (PB, target or the live run, whichever knows). finished: the live run's last split is the finish.
// The last row is marked finish only when finished or when the PB/target gives the count.
std::vector<Row> buildRows(const std::vector<int>& live, const MapStats& s, const Comparison& target, bool finished = false);
int splitCount(const std::vector<int>& live, const MapStats& s, const Comparison& target);

// Live delta while driving between splits: once the race time passes the comparison's next split, the
// player is behind by at least (now - that split). kNoDelta while still in time.
int runningDelta(const std::vector<int>& live, int nowMs, const std::vector<int>& cmp);

// A usable split list: 1..kMaxSplits times, each > 0, strictly increasing (the finish last).
constexpr size_t kMaxSplits = 256;
bool saneSplits(const std::vector<int>& splits);
// Drops what an unusable split list left in the stats (0.3.0-rc on Original stored ~280 splits of -1 as the PB):
// a PB that is not sane goes with its golds, a run keeps only its final time.
void sanitize(MapStats& s);

// The host's CHECKPOINT events (ESWC, Sunrise, Original) -> real checkpoints. On Original the OnCheckpoint hook fires
// about 50 times within 0.1 s at every checkpoint (one event per call, each with the next index), with times that are
// -1 or stale. Events less than kSameCpMs after the previous one are the same checkpoint, unless their time is good
// and later than the last split (two checkpoints in one poll on ESWC). A time that is unknown, repeated or ahead of
// the run's own clock (ms since RACE_START) is replaced by that clock.
constexpr int kSameCpMs = 300;
class CpEvents {
public:
    void start(uint64_t nowMs);   // RACE_START
    void clear();                 // no run (map load, menu)
    // A CHECKPOINT event with the host's time at wall clock nowMs. True: a new checkpoint, *split its race time
    // (-1 unknown). lastSplit: the run's last split so far (0 none).
    bool add(int hostTime, uint64_t nowMs, int lastSplit, int* split);
    // True when the last checkpoint came less than kSameCpMs before nowMs (the finish line's own events).
    bool justTaken(uint64_t nowMs) const;

private:
    bool started_ = false, any_ = false;
    uint64_t start_ = 0, last_ = 0;
    int lastHost_ = -2;
};

// JSON (UTF-8) of the stats, and back. False on bad JSON.
std::string toJson(const MapStats& s);
bool fromJson(const std::string& text, MapStats* s);

// A file name for a map key (UID, or a local key): [A-Za-z0-9_-], at most 64 chars.
std::string fileKey(const std::string& key);
// Local key for a map without UID: "local-" + FNV-1a of name and author time.
std::string localKey(const std::string& name, int authorTime);

} // namespace wrh
