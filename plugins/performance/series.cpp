// Horizon Performance: the benchmark series (see series.h). No Windows or Direct3D here.
#include "series.h"

#include <cmath>
#include <cstdio>

#include "common/json.h"

namespace perf {

SeriesOptions presetOptions(int preset, int refreshHz, const SeriesOptions& custom) {
    if (preset != PresetSmooth && preset != PresetLowLatency) return custom;
    SeriesOptions o;
    o.count = custom.count;
    o.filter = 1;
    o.powerThrottlingOff = 1;
    o.priority = 1;
    o.limitFps = refreshHz >= 30 && refreshHz <= 1000 ? refreshHz : 0; // unknown: no limit
    o.vsync = preset == PresetLowLatency ? 1 : 0;
    return o;
}

std::string seriesOptionsJson(const SeriesOptions& o) {
    char b[300];
    snprintf(b, sizeof(b),
             "{\"count\": %d, \"filter\": %d, \"limitFps\": %d, \"vsync\": %d, \"priority\": %d, \"affinity\": %d, "
             "\"powerThrottlingOff\": %d, \"fineTimer\": %d}",
             o.count, o.filter, o.limitFps, o.vsync, o.priority, o.affinity, o.powerThrottlingOff, o.fineTimer);
    return b;
}

std::vector<SeriesSet> defaultSeriesSets() {
    std::vector<SeriesSet> v(7);
    v[0].label = "baseline";
    v[1].label = "limiter+vsync-off";
    v[1].options.limitFps = 120;
    v[1].options.vsync = 1;
    v[2].label = "priority-high";
    v[2].options.priority = 2;
    v[3].label = "perf-cores";
    v[3].options.affinity = 2;
    v[4].label = "skip-repeated-states";
    v[4].options.filter = 1;
    v[5].label = "fine-timer";
    v[5].options.fineTimer = 1;
    v[6].label = "no-power-throttling";
    v[6].options.powerThrottlingOff = 1;
    return v;
}

namespace {
bool readInt(const horizon::Json& v, int& out) {
    if (v.type == horizon::Json::Bool) {
        out = v.b ? 1 : 0;
        return true;
    }
    if (v.type == horizon::Json::Number) {
        out = static_cast<int>(v.num);
        return true;
    }
    return false;
}

bool parseOptions(const horizon::Json& j, SeriesOptions& o, std::string& why) {
    if (j.type != horizon::Json::Object) {
        why = "\"options\" must be an object";
        return false;
    }
    for (const auto& kv : j.obj) {
        int v = 0;
        if (!readInt(kv.second, v)) {
            why = "option \"" + kv.first + "\" needs a number";
            return false;
        }
        int lo = 0, hi = 1;
        int* dst = nullptr;
        if (kv.first == "count") dst = &o.count;
        else if (kv.first == "filter") dst = &o.filter;
        else if (kv.first == "powerThrottlingOff") dst = &o.powerThrottlingOff;
        else if (kv.first == "fineTimer") dst = &o.fineTimer;
        else if (kv.first == "vsync") { dst = &o.vsync; hi = 2; }
        else if (kv.first == "priority") { dst = &o.priority; hi = 2; }
        else if (kv.first == "affinity") { dst = &o.affinity; hi = 2; }
        else if (kv.first == "limitFps") { dst = &o.limitFps; hi = 1000; }
        if (!dst) {
            why = "unknown option \"" + kv.first + "\"";
            return false;
        }
        if (v < lo || v > hi || (kv.first == "limitFps" && v != 0 && v < 30)) {
            why = "option \"" + kv.first + "\" out of range";
            return false;
        }
        *dst = v;
    }
    return true;
}
} // namespace

bool parseSeries(const horizon::Json& j, SeriesPlan& out, std::string& why) {
    if (j.type != horizon::Json::Object) {
        why = "\"series\" must be an object";
        return false;
    }
    SeriesPlan p;
    if (const auto* v = j.get("replay")) p.replay = v->asString();
    if (const auto* v = j.get("map")) p.map = v->asString();
    p.where = p.map.empty() ? "replay" : "map";
    if (const auto* v = j.get("mode")) p.where = v->asString();
    if (p.where != "replay" && p.where != "map" && p.where != "now") {
        why = "series mode must be \"replay\", \"map\" or \"now\"";
        return false;
    }
    if (p.where == "map" && p.map.empty()) {
        why = "series mode \"map\" needs \"map\"";
        return false;
    }
    if (const auto* v = j.get("seconds"); v && v->isNumber()) p.seconds = v->num;
    if (const auto* v = j.get("settle"); v && v->isNumber()) p.settle = v->num;
    if (const auto* v = j.get("startAfter"); v && v->isNumber()) p.startAfter = v->num;
    if (const auto* v = j.get("runs")) p.runs = static_cast<int>(v->asInt(p.runs));
    if (const auto* v = j.get("warmup")) p.warmup = static_cast<int>(v->asInt(p.warmup));
    if (const auto* v = j.get("returnToMenu"); v && v->type == horizon::Json::Bool) p.returnToMenu = v->b;
    if (!(p.seconds >= 1 && p.seconds <= 600)) {
        why = "series seconds must be 1..600";
        return false;
    }
    if (!(p.settle >= 0 && p.settle <= 60)) {
        why = "series settle must be 0..60";
        return false;
    }
    if (p.runs < 1 || p.runs > 10) {
        why = "series runs must be 1..10";
        return false;
    }
    if (p.warmup < 0 || p.warmup > 3) {
        why = "series warmup must be 0..3";
        return false;
    }
    const horizon::Json* sets = j.get("sets");
    if (!sets || (sets->type == horizon::Json::Array && sets->arr.empty())) {
        p.sets = defaultSeriesSets();
    } else {
        if (sets->type != horizon::Json::Array) {
            why = "\"sets\" must be a list";
            return false;
        }
        if (sets->arr.size() > 12) {
            why = "at most 12 sets";
            return false;
        }
        for (const auto& s : sets->arr) {
            if (s.type != horizon::Json::Object) {
                why = "every set must be an object";
                return false;
            }
            SeriesSet set;
            if (const auto* l = s.get("label")) set.label = l->asString();
            if (set.label.empty()) {
                why = "every set needs a \"label\"";
                return false;
            }
            for (const auto& other : p.sets)
                if (other.label == set.label) {
                    why = "duplicate set label \"" + set.label + "\"";
                    return false;
                }
            if (const auto* o = s.get("options"); o && !parseOptions(*o, set.options, why)) return false;
            p.sets.push_back(set);
        }
    }
    out = p;
    return true;
}

StartBy benchStartBy(const std::string& where, int gameState, bool keyPressed) {
    if (keyPressed) return StartBy::Key;
    if (where == "replay" && gameState == HORIZON_STATE_REPLAY) return StartBy::Scene;
    if (where == "map" && gameState == HORIZON_STATE_SOLO_RACE) return StartBy::Scene;
    return StartBy::None; // "now": only by its key (or startAfter, handled by the plugin)
}

bool benchSceneOk(const std::string& where, StartBy by, int gameState) {
    if (by == StartBy::Key || where == "now") return true;
    if (where == "replay") return gameState == HORIZON_STATE_REPLAY;
    return gameState == HORIZON_STATE_SOLO_RACE || gameState == HORIZON_STATE_END_RACE;
}

std::vector<SeriesStep> seriesOrder(int nSets, int runs, int warmup) {
    std::vector<SeriesStep> v;
    if (nSets <= 0) return v;
    const int rounds = warmup + runs;
    for (int r = 0; r < rounds; r++)
        for (int i = 0; i < nSets; i++) {
            SeriesStep s;
            s.set = (i + r) % nSets;
            s.round = r;
            s.warmup = r < warmup;
            s.run = s.warmup ? 0 : r - warmup + 1;
            v.push_back(s);
        }
    return v;
}

// ------------------------------------------------------------------ the machine
SeriesOptions Series::effective(const SeriesOptions& wanted) const {
    SeriesOptions e = wanted;
    e.vsync = user_.vsync; // applied at device creation: stays what the game was started with
    return e;
}

bool Series::start(const SeriesPlan& plan, const SeriesCaps& caps, const SeriesOptions& user, double nowMs, SeriesIO& io, std::string& why) {
    plan_ = plan;
    caps_ = caps;
    user_ = user;
    active_.clear();
    unavailable_.clear();
    results_.clear();
    frames_.clear();
    for (const SeriesSet& s : plan.sets) {
        const bool needsRestart = s.options.vsync != 0 && s.options.vsync != caps.vsyncInEffect;
        SeriesOptions rest = s.options;
        rest.vsync = 0;
        if (needsRestart && rest.allOff()) {
            SeriesRun r;
            r.set = -1;
            r.label = s.label;
            r.skipped = true;
            r.skipReason = "needs restart: vsync";
            results_.push_back(r);
            continue;
        }
        active_.push_back(s);
    }
    if (active_.empty()) {
        why = "no set can run without a restart";
        return false;
    }
    unavailable_.assign(active_.size(), 0);
    steps_ = seriesOrder(static_cast<int>(active_.size()), plan.runs, plan.warmup);
    step_ = 0;
    beginStep(nowMs, io);
    return true;
}

void Series::beginStep(double nowMs, SeriesIO& io) {
    while (step_ < steps_.size() && unavailable_[steps_[step_].set]) step_++;
    if (step_ >= steps_.size()) {
        finish(true, "", io);
        return;
    }
    io.apply(effective(active_[steps_[step_].set].options));
    phase_ = Phase::Settle;
    t0_ = nowMs;
    settledMs_ = recordedMs_ = 0;
    frames_.clear();
}

void Series::advance(double nowMs, SeriesIO& io) {
    step_++;
    beginStep(nowMs, io);
}

void Series::tick(double nowMs, const FrameSample* frame, SeriesIO& io) {
    if (step_ >= steps_.size()) return;
    const SeriesStep& st = steps_[step_];
    const SeriesSet& set = active_[st.set];
    switch (phase_) {
    case Phase::Settle: {
        // the time counts in frames that show the scene (background, loading, a replay restart: not)
        if (frame && !(frame->skip & kSkipSeries)) settledMs_ += frame->frameMs;
        if (settledMs_ < plan_.settle * 1000.0) return;
        const std::string why = io.unavailable(set.options);
        if (!why.empty()) {
            unavailable_[st.set] = 1;
            SeriesRun r;
            r.set = st.set;
            r.label = set.label;
            r.skipped = true;
            r.skipReason = why;
            results_.push_back(r);
            advance(nowMs, io);
            return;
        }
        phase_ = Phase::Recording;
        t0_ = nowMs;
        recordedMs_ = 0;
        frames_.clear();
        frames_.reserve(static_cast<size_t>(plan_.seconds * 400));
        started_ = io.isoNow();
        return;
    }
    case Phase::Recording: {
        // a window is `seconds` of frames that show the scene: skipped frames neither count nor end it
        if (frame && !(frame->skip & kSkipSeries)) {
            frames_.push_back(*frame);
            recordedMs_ += frame->frameMs;
        }
        if (recordedMs_ < plan_.seconds * 1000.0) return;
        SeriesRun r;
        r.set = st.set;
        r.label = set.label;
        r.run = st.run;
        r.warmupNo = st.warmup ? st.round + 1 : 0;
        r.started = started_;
        r.applied = effective(set.options);
        r.note = io.note(set.options);
        if (set.options.vsync != 0 && set.options.vsync != caps_.vsyncInEffect) {
            r.note += (r.note.empty() ? "" : "; ");
            r.note += "vsync not applied (needs restart)";
        }
        r.summary = summarize(frames_, kSkipSeries);
        results_.push_back(r);
        io.runDone(results_.back(), frames_);
        advance(nowMs, io);
        return;
    }
    default: return;
    }
}

void Series::pause() {
    if (phase_ == Phase::Settle || phase_ == Phase::Recording) {
        phase_ = Phase::Waiting;
        frames_.clear();
    }
}

void Series::resume(double nowMs) {
    if (phase_ != Phase::Waiting) return;
    phase_ = Phase::Settle;
    t0_ = nowMs;
    settledMs_ = recordedMs_ = 0;
    frames_.clear();
}

void Series::abort(const std::string& why, SeriesIO& io) {
    if (active()) finish(false, why, io);
}

void Series::finish(bool completed, const std::string& why, SeriesIO& io) {
    io.apply(user_);
    phase_ = completed ? Phase::Done : Phase::Aborted;
    io.finished(completed, why);
}

std::string Series::progress() const {
    if (!active() || step_ >= steps_.size()) return "";
    int widx = 0, wtot = 0, midx = 0, mtot = 0;
    for (size_t i = 0; i < steps_.size(); i++) {
        if (unavailable_[steps_[i].set]) continue;
        int& tot = steps_[i].warmup ? wtot : mtot;
        tot++;
        if (i <= step_) (steps_[i].warmup ? widx : midx)++;
    }
    const SeriesStep& st = steps_[step_];
    const std::string& label = active_[st.set].label;
    char b[200];
    if (st.warmup) snprintf(b, sizeof(b), "Benchmark Warmup %d/%d: %s", widx, wtot, label.c_str());
    else snprintf(b, sizeof(b), "Benchmark %d/%d: %s, Lauf %d/%d", midx, mtot, label.c_str(), st.run, plan_.runs);
    return b;
}

int Series::windowsLeft() const {
    if (!active()) return 0;
    int n = 0;
    for (size_t i = step_; i < steps_.size(); i++)
        if (!unavailable_[steps_[i].set]) n++;
    return n;
}

// ------------------------------------------------------------------ the series file
namespace {
void kv(std::string& out, const char* key, const std::string& v) {
    horizon::jsonQuote(out, key);
    out += ": ";
    horizon::jsonQuote(out, v);
}
void kvn(std::string& out, const char* key, double v, int decimals) {
    char b[96];
    if (std::isfinite(v)) snprintf(b, sizeof(b), "\"%s\": %.*f", key, decimals, v);
    else snprintf(b, sizeof(b), "\"%s\": null", key);
    out += b;
}
} // namespace

std::string seriesJson(const SeriesPlan& plan, const std::vector<SeriesSet>& activeSets, const std::vector<SeriesRun>& runs,
                       const SeriesMeta& m) {
    (void)activeSets;
    std::string o = "{\n  \"version\": 1,\n  \"kind\": \"series\",\n  ";
    kv(o, "stamp", m.stamp);
    o += ",\n  ";
    kv(o, "started", m.started);
    o += ",\n  ";
    kv(o, "finished", m.finished);
    o += std::string(",\n  \"completed\": ") + (m.completed ? "true" : "false") + ",\n  ";
    kv(o, "abortedWhy", m.abortedWhy);
    o += ",\n  ";
    kv(o, "game", m.game);
    o += ",\n  ";
    kv(o, "gpu", m.gpu);
    o += ",\n  ";
    kv(o, "cpu", m.cpu);
    o += ",\n  ";
    kv(o, "resolution", m.resolution);
    o += std::string(",\n  \"tmvs\": ") + (m.tmvs ? "true" : "false") + ",\n  ";
    kv(o, "tmvsNote", m.tmvsNote);
    o += ",\n  ";
    kv(o, "mode", m.where);
    o += ",\n  ";
    kv(o, "replay", m.replay);
    o += ",\n  ";
    kv(o, "map", m.map);
    o += ",\n  ";
    kvn(o, "seconds", plan.seconds, 1);
    o += ", ";
    kvn(o, "settle", plan.settle, 1);
    o += ", ";
    kvn(o, "runsPerSet", plan.runs, 0);
    o += ", ";
    kvn(o, "warmupPerSet", plan.warmup, 0);
    o += ",\n  \"userOptions\": " + seriesOptionsJson(m.user);
    o += ",\n  ";
    kvn(o, "vsyncInEffect", m.vsyncInEffect, 0);
    o += ",\n  \"sets\": [\n";
    for (size_t i = 0; i < plan.sets.size(); i++) {
        const SeriesSet& s = plan.sets[i];
        std::string reason;
        for (const SeriesRun& r : runs)
            if (r.skipped && r.label == s.label) reason = r.skipReason;
        o += "    {";
        kv(o, "label", s.label);
        o += ", \"options\": " + seriesOptionsJson(s.options) + ", ";
        kv(o, "status", reason.empty() ? "ok" : "skipped");
        o += ", ";
        kv(o, "skipReason", reason);
        o += i + 1 < plan.sets.size() ? "},\n" : "}\n";
    }
    o += "  ],\n  \"runs\": [\n";
    bool first = true;
    int order = 0;
    for (const SeriesRun& r : runs) {
        if (r.skipped) continue;
        const Summary& s = r.summary;
        o += first ? "    {" : ",\n    {";
        first = false;
        kvn(o, "order", ++order, 0);
        o += ", ";
        kv(o, "set", r.label);
        o += ", ";
        kvn(o, "run", r.run, 0);
        o += std::string(", \"warmup\": ") + (r.run == 0 ? "true" : "false") + ", ";
        kv(o, "started", r.started);
        o += ", ";
        kv(o, "file", r.file);
        o += ", ";
        kv(o, "note", r.note);
        o += ", ";
        kvn(o, "seconds", s.seconds, 3);
        o += ", ";
        kvn(o, "frames", static_cast<double>(s.frames), 0);
        o += ", ";
        kvn(o, "avgFps", s.avgFps, 2);
        o += ", ";
        kvn(o, "avgMs", s.avgMs, 3);
        o += ", ";
        kvn(o, "medianMs", s.medianMs, 3);
        o += ", ";
        kvn(o, "p99Ms", s.p99Ms, 3);
        o += ", ";
        kvn(o, "p999Ms", s.p999Ms, 3);
        o += ", ";
        kvn(o, "maxMs", s.maxMs, 3);
        o += ", ";
        kvn(o, "low1Fps", s.low1Fps, 2);
        o += ", ";
        kvn(o, "low01Fps", s.low01Fps, 2);
        o += ", ";
        kvn(o, "stutters", static_cast<double>(s.stutters), 0);
        o += ", ";
        kvn(o, "presentMsAvg", s.presentMsAvg, 3);
        o += ", ";
        kvn(o, "cpuMsAvg", s.cpuMsAvg, 3);
        o += ", ";
        kvn(o, "sleepMsAvg", s.sleepMsAvg, 3);
        o += ", \"options\": " + seriesOptionsJson(r.applied) + "}";
    }
    o += "\n  ]\n}\n";
    return o;
}

} // namespace perf
