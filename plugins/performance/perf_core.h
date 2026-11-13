// Horizon Performance: the parts without Windows or Direct3D, so the unit tests can run them alone.
//   - FrameSample / FrameRing: one entry per Present, a ring of the last frames
//   - summarize(): average, median, percentiles, 1 % / 0.1 % lows, stutters
//   - Pacer: the frame limiter's deadline maths
//   - StateCache: which Direct3D 9 state calls repeat the value already set
//   - BenchRequest: the launcher's bench-request.json
#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace perf {

struct SeriesPlan; // series.h

// Per-frame counters of Direct3D 9 calls (filled by d3d_hooks.cpp while counting is on).
struct CallCounts {
    uint32_t draws = 0, drawsUP = 0, primitives = 0, drawsSoftwareVP = 0;
    uint32_t renderStates = 0, renderStatesSame = 0;
    uint32_t samplerStates = 0, samplerStatesSame = 0;
    uint32_t textureStageStates = 0, textureStageStatesSame = 0;
    uint32_t textures = 0, texturesSame = 0;
    uint32_t vertexShaders = 0, vertexShadersSame = 0;
    uint32_t pixelShaders = 0, pixelShadersSame = 0;
    uint32_t vsConstCalls = 0, vsConstVec4 = 0, psConstCalls = 0, psConstVec4 = 0;
    uint32_t streams = 0, indices = 0, declsAndFvf = 0, transforms = 0;
    uint32_t renderTargets = 0, clears = 0, scenes = 0;
    uint32_t locks = 0, locksPlain = 0;     // buffer/texture/surface locks; plain = no DISCARD/NOOVERWRITE/READONLY
    uint32_t readbacks = 0;                 // GetRenderTargetData / GetFrontBufferData
    uint32_t queryPolls = 0, queryBusy = 0; // IDirect3DQuery9::GetData calls, of them S_FALSE (GPU not done)
    uint32_t filtered = 0;                  // calls the state cache did not pass on
    double lockMs = 0, readbackMs = 0, queryWaitMs = 0;
};

struct FrameSample {
    double frameMs = 0;   // Present start to Present start
    double presentMs = 0; // inside Present (with Horizon's overlay): long = waiting for the GPU or V-Sync
    double sleepMs = 0;   // the limiter's wait
    double cpuMs = 0;     // the rest: the game's own work for this frame
    CallCounts calls;
    bool hasCalls = false;
    uint8_t skip = 0;     // kSkip* bits: why this frame is not gameplay (left out of the statistics)
};

// Frames that say nothing about how the game runs: the window was in the background or minimised, Present
// failed (device lost, Alt+Tab out of full screen), the first second after a Reset, a focus change or a game
// state change, the game was loading, or (benchmark series) the scene to measure was not on screen.
constexpr uint8_t kSkipUnfocused = 0x01, kSkipLost = 0x02, kSkipSettle = 0x04, kSkipLoading = 0x08, kSkipScene = 0x10;
constexpr uint8_t kSkipStats = kSkipUnfocused | kSkipLost | kSkipSettle | kSkipLoading; // live numbers and the log line
// A stutter: a frame at least twice the median AND at least this much longer than it. At 400 fps (2.5 ms) the
// factor alone counted every 5 ms frame, thousands per minute in menus, none of them visible.
constexpr double kStutterMinExtraMs = 6.0;

class FrameRing {
public:
    explicit FrameRing(size_t capacity = 4096) : buf_(capacity) {}
    void push(const FrameSample& s) {
        buf_[next_] = s;
        next_ = (next_ + 1) % buf_.size();
        if (count_ < buf_.size()) count_++;
    }
    size_t size() const { return count_; }
    size_t capacity() const { return buf_.size(); }
    // i = 0 is the oldest kept frame
    const FrameSample& at(size_t i) const { return buf_[(next_ + buf_.size() - count_ + i) % buf_.size()]; }
    const FrameSample& newest() const { return at(count_ - 1); }
    void clear() { next_ = count_ = 0; }
    // the last n frames, oldest first
    std::vector<FrameSample> last(size_t n) const;

private:
    std::vector<FrameSample> buf_;
    size_t next_ = 0, count_ = 0;
};

struct Summary {
    size_t frames = 0;
    double seconds = 0;
    double avgMs = 0, medianMs = 0, p99Ms = 0, p999Ms = 0, maxMs = 0;
    double avgFps = 0;
    double low1Fps = 0;  // 1000 / mean of the slowest 1 % of frame times
    double low01Fps = 0; // the same for the slowest 0.1 %
    size_t stutters = 0; // frames longer than 2x the median and at least kStutterMinExtraMs longer than it
    double presentMsAvg = 0, cpuMsAvg = 0, sleepMsAvg = 0;
    double drawsPerFrame = -1; // -1: counting was off
    size_t skipped = 0;  // frames left out (FrameSample::skip & mask)
};

// Percentile of a sorted vector (nearest rank, p in 0..100).
double percentileSorted(const std::vector<double>& sorted, double p);
// Frames with (skip & skipMask) != 0 are left out (counted in Summary::skipped).
Summary summarize(const std::vector<FrameSample>& frames, uint8_t skipMask = kSkipStats);
bool isStutter(double frameMs, double medianMs);

// ------------------------------------------------------------------ frame limiter maths
// Deadlines on a fixed grid of `period` seconds. A frame that comes late by more than one period
// starts a new grid instead of rushing several frames to catch up (no burst after a hitch).
class Pacer {
public:
    void setFps(double fps) { period_ = fps > 0 ? 1.0 / fps : 0; }
    double period() const { return period_; }
    // now: time after Present. Returns the time to wait until (<= now: no wait) and moves the deadline on.
    double next(double now) {
        if (period_ <= 0) return now;
        if (deadline_ <= 0 || now - deadline_ > period_) deadline_ = now; // first frame or late: resync
        deadline_ += period_;
        return deadline_;
    }
    void reset() { deadline_ = 0; }

private:
    double period_ = 0, deadline_ = 0;
};

// ------------------------------------------------------------------ redundant state filter
// Remembers the last value of each state the game set through the device. `same()` says whether a call
// repeats it (counted either way); `set()` records the new value. invalidate() forgets everything: after
// Reset, a state block Apply, or anything else that changes states behind our back.
class StateCache {
public:
    static constexpr int kRenderStates = 256;
    static constexpr int kSamplers = 16 + 5; // 0..15 pixel, D3DDMAPSAMPLER (256) and 4 vertex samplers (257..260)
    static constexpr int kSamplerStates = 14;
    static constexpr int kStages = 8, kStageStates = 33;

    StateCache() { invalidate(); }
    void invalidate();

    // sampler index as Direct3D numbers it -> 0..20, -1 for anything else
    static int samplerSlot(uint32_t sampler) {
        if (sampler < 16) return static_cast<int>(sampler);
        if (sampler >= 256 && sampler <= 260) return static_cast<int>(16 + sampler - 256);
        return -1;
    }

    bool renderStateSame(uint32_t state, uint32_t value) const {
        return state < kRenderStates && rsKnown_[state] && rs_[state] == value;
    }
    void setRenderState(uint32_t state, uint32_t value) {
        if (state >= kRenderStates) return;
        rs_[state] = value;
        rsKnown_[state] = true;
    }
    bool samplerStateSame(uint32_t sampler, uint32_t type, uint32_t value) const {
        const int s = samplerSlot(sampler);
        return s >= 0 && type < kSamplerStates && ssKnown_[s][type] && ss_[s][type] == value;
    }
    void setSamplerState(uint32_t sampler, uint32_t type, uint32_t value) {
        const int s = samplerSlot(sampler);
        if (s < 0 || type >= kSamplerStates) return;
        ss_[s][type] = value;
        ssKnown_[s][type] = true;
    }
    bool stageStateSame(uint32_t stage, uint32_t type, uint32_t value) const {
        return stage < kStages && type < kStageStates && tsKnown_[stage][type] && ts_[stage][type] == value;
    }
    void setStageState(uint32_t stage, uint32_t type, uint32_t value) {
        if (stage >= kStages || type >= kStageStates) return;
        ts_[stage][type] = value;
        tsKnown_[stage][type] = true;
    }
    bool textureSame(uint32_t sampler, const void* tex) const {
        const int s = samplerSlot(sampler);
        return s >= 0 && texKnown_[s] && tex_[s] == tex;
    }
    void setTexture(uint32_t sampler, const void* tex) {
        const int s = samplerSlot(sampler);
        if (s < 0) return;
        tex_[s] = tex;
        texKnown_[s] = true;
    }
    bool vertexShaderSame(const void* vs) const { return vsKnown_ && vs_ == vs; }
    void setVertexShader(const void* vs) { vs_ = vs; vsKnown_ = true; }
    bool pixelShaderSame(const void* ps) const { return psKnown_ && ps_ == ps; }
    void setPixelShader(const void* ps) { ps_ = ps; psKnown_ = true; }
    // A texture or shader the game released may come back at the same address: forget pointers each frame.
    void forgetPointers();

private:
    uint32_t rs_[kRenderStates];
    bool rsKnown_[kRenderStates];
    uint32_t ss_[kSamplers][kSamplerStates];
    bool ssKnown_[kSamplers][kSamplerStates];
    uint32_t ts_[kStages][kStageStates];
    bool tsKnown_[kStages][kStageStates];
    const void* tex_[kSamplers];
    bool texKnown_[kSamplers];
    const void* vs_ = nullptr;
    const void* ps_ = nullptr;
    bool vsKnown_ = false, psKnown_ = false;
};

// ------------------------------------------------------------------ the launcher's benchmark request
struct BenchRequest {
    int version = 1;
    std::string mode = "map"; // "map": the module starts `map` in solo; "replay": I open a replay myself
    std::string map;          // UTF-8 path of a .Challenge.Gbx
    double seconds = 30, warmup = 5;
    std::string label = "run";
    int runs = 1;
    bool returnToMenu = true;
    std::shared_ptr<SeriesPlan> series; // set: a benchmark series ("series" in the request), mode/map/seconds follow it
};
// false (and why) when the text is not a usable request
bool parseBenchRequest(const std::string& json, BenchRequest& out, std::string& why);
// A label safe for a file name: [A-Za-z0-9._-], at most 40 characters, never empty.
std::string safeLabel(const std::string& label);

// CSV of frames (header + one line per frame) and the summary as JSON (format in docs/performance.md).
std::string framesCsv(const std::vector<FrameSample>& frames);
struct ResultMeta {
    std::string game, label, started, request; // started: ISO time; request: "map"/"replay"/"manual"
    int run = 1;
    std::vector<std::pair<std::string, std::string>> options; // name -> JSON value text
};
std::string summaryJson(const Summary& s, const ResultMeta& m);

} // namespace perf
