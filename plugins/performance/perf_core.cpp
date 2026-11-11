// Horizon Performance: statistics, limiter maths, state cache, benchmark request and result files.
// No Windows or Direct3D here (see perf_core.h).
#include "perf_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/json.h"
#include "series.h"

namespace perf {

std::vector<FrameSample> FrameRing::last(size_t n) const {
    if (n > count_) n = count_;
    std::vector<FrameSample> out;
    out.reserve(n);
    for (size_t i = count_ - n; i < count_; i++) out.push_back(at(i));
    return out;
}

double percentileSorted(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0;
    if (p <= 0) return sorted.front();
    if (p >= 100) return sorted.back();
    // nearest rank: the smallest value with at least p % of the values at or below it
    const double rank = std::ceil(p / 100.0 * static_cast<double>(sorted.size()));
    size_t i = rank < 1 ? 0 : static_cast<size_t>(rank) - 1;
    if (i >= sorted.size()) i = sorted.size() - 1;
    return sorted[i];
}

namespace {
// mean of the slowest `share` of the frames (at least one frame)
double slowestMean(const std::vector<double>& sorted, double share) {
    if (sorted.empty()) return 0;
    size_t n = static_cast<size_t>(std::floor(static_cast<double>(sorted.size()) * share));
    if (n < 1) n = 1;
    double sum = 0;
    for (size_t i = sorted.size() - n; i < sorted.size(); i++) sum += sorted[i];
    return sum / static_cast<double>(n);
}
} // namespace

Summary summarize(const std::vector<FrameSample>& frames) {
    Summary s;
    s.frames = frames.size();
    if (frames.empty()) return s;
    std::vector<double> ms;
    ms.reserve(frames.size());
    double sum = 0, present = 0, cpu = 0, sleep = 0, draws = 0;
    size_t withCalls = 0;
    for (const auto& f : frames) {
        ms.push_back(f.frameMs);
        sum += f.frameMs;
        present += f.presentMs;
        cpu += f.cpuMs;
        sleep += f.sleepMs;
        if (f.hasCalls) {
            draws += f.calls.draws + f.calls.drawsUP;
            withCalls++;
        }
    }
    std::sort(ms.begin(), ms.end());
    const double n = static_cast<double>(frames.size());
    s.seconds = sum / 1000.0;
    s.avgMs = sum / n;
    s.avgFps = s.avgMs > 0 ? 1000.0 / s.avgMs : 0;
    s.medianMs = percentileSorted(ms, 50);
    s.p99Ms = percentileSorted(ms, 99);
    s.p999Ms = percentileSorted(ms, 99.9);
    s.maxMs = ms.back();
    const double w1 = slowestMean(ms, 0.01), w01 = slowestMean(ms, 0.001);
    s.low1Fps = w1 > 0 ? 1000.0 / w1 : 0;
    s.low01Fps = w01 > 0 ? 1000.0 / w01 : 0;
    for (double v : ms)
        if (v > 2.0 * s.medianMs) s.stutters++;
    s.presentMsAvg = present / n;
    s.cpuMsAvg = cpu / n;
    s.sleepMsAvg = sleep / n;
    s.drawsPerFrame = withCalls ? draws / static_cast<double>(withCalls) : -1;
    return s;
}

// ------------------------------------------------------------------ StateCache
void StateCache::invalidate() {
    memset(rsKnown_, 0, sizeof(rsKnown_));
    memset(ssKnown_, 0, sizeof(ssKnown_));
    memset(tsKnown_, 0, sizeof(tsKnown_));
    forgetPointers();
}

void StateCache::forgetPointers() {
    memset(texKnown_, 0, sizeof(texKnown_));
    vsKnown_ = psKnown_ = false;
    vs_ = ps_ = nullptr;
}

// ------------------------------------------------------------------ benchmark request
bool parseBenchRequest(const std::string& text, BenchRequest& out, std::string& why) {
    horizon::Json j;
    std::string err;
    if (!horizon::JsonParser::parse(text, j, err)) {
        why = "not JSON: " + err;
        return false;
    }
    if (j.type != horizon::Json::Object) {
        why = "not a JSON object";
        return false;
    }
    BenchRequest r;
    if (const auto* v = j.get("version")) r.version = static_cast<int>(v->asInt(1));
    if (r.version != 1) {
        why = "unknown version";
        return false;
    }
    if (const auto* sv = j.get("series")) {
        auto plan = std::make_shared<SeriesPlan>();
        if (!parseSeries(*sv, *plan, why)) return false;
        r.series = plan;
        r.mode = plan->where == "map" ? "map" : "replay"; // "now" needs no game state either; the plugin looks at series->where
        r.map = plan->map;
        r.seconds = plan->seconds;
        r.warmup = 0;
        r.runs = plan->runs;
        r.returnToMenu = plan->returnToMenu;
        if (const auto* v = j.get("label")) r.label = v->asString();
        else r.label = "series";
        r.label = safeLabel(r.label);
        out = r;
        return true;
    }
    if (const auto* v = j.get("mode")) r.mode = v->asString();
    if (r.mode != "map" && r.mode != "replay") {
        why = "mode must be \"map\" or \"replay\"";
        return false;
    }
    if (const auto* v = j.get("map")) r.map = v->asString();
    if (r.mode == "map" && r.map.empty()) {
        why = "mode \"map\" needs \"map\"";
        return false;
    }
    if (const auto* v = j.get("seconds"); v && v->isNumber()) r.seconds = v->num;
    if (const auto* v = j.get("warmup"); v && v->isNumber()) r.warmup = v->num;
    if (const auto* v = j.get("label")) r.label = v->asString();
    if (const auto* v = j.get("runs")) r.runs = static_cast<int>(v->asInt(1));
    if (const auto* v = j.get("returnToMenu"); v && v->type == horizon::Json::Bool) r.returnToMenu = v->b;
    if (!(r.seconds >= 5 && r.seconds <= 600)) {
        why = "seconds must be 5..600";
        return false;
    }
    if (!(r.warmup >= 0 && r.warmup <= 120)) {
        why = "warmup must be 0..120";
        return false;
    }
    if (r.runs < 1 || r.runs > 10) {
        why = "runs must be 1..10";
        return false;
    }
    r.label = safeLabel(r.label);
    out = r;
    return true;
}

std::string safeLabel(const std::string& label) {
    std::string s;
    for (char c : label) {
        if (s.size() >= 40) break;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        s += ok ? c : '_';
    }
    while (!s.empty() && (s.front() == '.' || s.front() == '_')) s.erase(s.begin());
    return s.empty() ? std::string("run") : s;
}

// ------------------------------------------------------------------ result files
std::string framesCsv(const std::vector<FrameSample>& frames) {
    std::string out =
        "frame,frame_ms,present_ms,cpu_ms,sleep_ms,draws,draws_up,primitives,draws_swvp,render_states,render_states_same,"
        "sampler_states,sampler_states_same,stage_states,stage_states_same,textures,textures_same,vertex_shaders,"
        "vertex_shaders_same,pixel_shaders,pixel_shaders_same,vs_const_calls,ps_const_calls,streams,render_targets,locks,"
        "locks_plain,lock_ms,readbacks,readback_ms,query_polls,query_busy,query_wait_ms,filtered\n";
    char line[512];
    for (size_t i = 0; i < frames.size(); i++) {
        const FrameSample& f = frames[i];
        int n = snprintf(line, sizeof(line), "%u,%.3f,%.3f,%.3f,%.3f", static_cast<unsigned>(i), f.frameMs, f.presentMs, f.cpuMs, f.sleepMs);
        if (f.hasCalls) {
            const CallCounts& c = f.calls;
            snprintf(line + n, sizeof(line) - n,
                     ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.3f,%u,%.3f,%u,%u,%.3f,%u\n", c.draws,
                     c.drawsUP, c.primitives, c.drawsSoftwareVP, c.renderStates, c.renderStatesSame, c.samplerStates,
                     c.samplerStatesSame, c.textureStageStates, c.textureStageStatesSame, c.textures, c.texturesSame, c.vertexShaders,
                     c.vertexShadersSame, c.pixelShaders, c.pixelShadersSame, c.vsConstCalls, c.psConstCalls, c.streams,
                     c.renderTargets, c.locks, c.locksPlain, c.lockMs, c.readbacks, c.readbackMs, c.queryPolls, c.queryBusy,
                     c.queryWaitMs, c.filtered);
        } else {
            snprintf(line + n, sizeof(line) - n, ",,,,,,,,,,,,,,,,,,,,,,,,,,,,,\n");
        }
        out += line;
    }
    return out;
}

namespace {
void num(std::string& out, const char* key, double v, int decimals = 2) {
    char buf[96];
    if (std::isfinite(v)) snprintf(buf, sizeof(buf), "  \"%s\": %.*f,\n", key, decimals, v);
    else snprintf(buf, sizeof(buf), "  \"%s\": null,\n", key);
    out += buf;
}
void str(std::string& out, const char* key, const std::string& v) {
    out += "  \"";
    out += key;
    out += "\": ";
    horizon::jsonQuote(out, v);
    out += ",\n";
}
} // namespace

std::string summaryJson(const Summary& s, const ResultMeta& m) {
    std::string out = "{\n  \"version\": 1,\n";
    str(out, "game", m.game);
    str(out, "label", m.label);
    str(out, "request", m.request);
    num(out, "run", m.run, 0);
    str(out, "started", m.started);
    num(out, "seconds", s.seconds, 3);
    num(out, "frames", static_cast<double>(s.frames), 0);
    num(out, "avgFps", s.avgFps);
    num(out, "avgMs", s.avgMs, 3);
    num(out, "medianMs", s.medianMs, 3);
    num(out, "p99Ms", s.p99Ms, 3);
    num(out, "p999Ms", s.p999Ms, 3);
    num(out, "maxMs", s.maxMs, 3);
    num(out, "low1Fps", s.low1Fps);
    num(out, "low01Fps", s.low01Fps);
    num(out, "stutters", static_cast<double>(s.stutters), 0);
    num(out, "presentMsAvg", s.presentMsAvg, 3);
    num(out, "cpuMsAvg", s.cpuMsAvg, 3);
    num(out, "sleepMsAvg", s.sleepMsAvg, 3);
    if (s.drawsPerFrame >= 0) num(out, "drawsPerFrame", s.drawsPerFrame, 1);
    else out += "  \"drawsPerFrame\": null,\n";
    out += "  \"options\": {";
    for (size_t i = 0; i < m.options.size(); i++) {
        out += i ? ", " : "";
        horizon::jsonQuote(out, m.options[i].first);
        out += ": " + m.options[i].second;
    }
    out += "}\n}\n";
    return out;
}

} // namespace perf
