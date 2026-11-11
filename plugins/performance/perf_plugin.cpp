// Horizon Performance: frame-time recorder, Direct3D 9 call counter, benchmark, frame limiter and a few
// process options, plus a hang log for long loading stalls. Docs: docs/performance.md.
//
// Rules this plugin keeps (Competition Patch, TMX, online):
//   - Rendering and process settings only: nothing touches physics, input timing, replays or game values.
//   - Every optimisation is a switch, off by default, with its effect measured by the benchmark.
//   - Competition Patch loaded: the Direct3D call counter and the state filter stay off (they patch d3d9's
//     method tables); the limiter, V-Sync, timer, priority and core options work through Present or Windows.
#define HORIZON_PLUGIN_BUILD
#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <string>
#include <vector>

#include "imgui.h"
#include "horizon_imgui.h"
#include "horizon_plugin.h"

#include "d3d_hooks.h"
#include "perf_core.h"
#include "series.h"
#include "stall.h"
#include "sysopts.h"

namespace {

const HorizonHostApi* g_host = nullptr;
int32_t g_benchHotkey = 0;
bool g_imgui = false;
bool g_enabled = true;
bool g_competitionPatch = false;
bool g_hasPostPresent = false;
std::wstring g_data;     // data folder, trailing backslash
uint32_t g_game = 0;
std::string g_gameKey;   // "tmnf" ...
std::wstring g_userFolder; // the game's user data folder (Documents\TrackMania...), trailing backslash, "" unknown

// ---- settings (names are the keys in horizon\settings\performance.json)
struct Settings {
    int overlay = 1;        // graph window
    int overlayCalls = 1;   // second line with the call counts
    int count = 0;          // count Direct3D calls
    int filter = 0;         // skip repeated states
    int limitFps = 0;       // 0 = off
    int vsync = 0;          // 0 = as the game sets it, 1 = off, 2 = on
    int priority = 0, affinity = 0, powerThrottlingOff = 0, fineTimer = 0;
    int hangLog = 1;
    int hangMs = 1500;
    int benchSeconds = 30;
    char benchLabel[48] = "manual";
} S;

void logf(int level, const char* fmt, ...) {
    if (!g_host) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_host->log(g_host->plugin, level, "%s", buf);
}

void loadSettings() {
    auto gi = [](const char* k, int d) { return g_host->get_int(g_host->plugin, k, d); };
    S.overlay = gi("overlay", S.overlay);
    S.overlayCalls = gi("overlay_calls", S.overlayCalls);
    S.count = gi("count", S.count);
    S.filter = gi("filter", S.filter);
    S.limitFps = gi("limit_fps", S.limitFps);
    S.vsync = gi("vsync", S.vsync);
    S.priority = gi("priority", S.priority);
    S.affinity = gi("affinity", S.affinity);
    S.powerThrottlingOff = gi("power_throttling_off", S.powerThrottlingOff);
    S.fineTimer = gi("fine_timer", S.fineTimer);
    S.hangLog = gi("hang_log", S.hangLog);
    S.hangMs = gi("hang_ms", S.hangMs);
    S.benchSeconds = gi("bench_seconds", S.benchSeconds);
    g_host->get_string(g_host->plugin, "bench_label", "manual", S.benchLabel, sizeof(S.benchLabel));
}
void saveInt(const char* k, int v) { g_host->set_int(g_host->plugin, k, v); }

bool callsAllowed() { return !g_competitionPatch; }

// ---- timing state
perf::FrameRing g_ring(8192);
double g_tPre = 0, g_tPrevPre = 0, g_tPost = 0, g_sleepMs = 0, g_presentMs = 0;
perf::Pacer g_pacer;
uint64_t g_frameNo = 0;
perf::Summary g_live;   // last 10 s, refreshed twice a second
double g_liveAt = 0;
HWND g_window = nullptr;

// ---- benchmark
enum class Bench { Idle, WaitMenu, WaitRace, Warmup, Recording, Series };
Bench g_bench = Bench::Idle;
perf::BenchRequest g_req;
bool g_reqFromLauncher = false;
int g_run = 0;
double g_benchT = 0;       // when the current step started (ms)
std::vector<perf::FrameSample> g_recorded;
std::string g_benchStatus = "";
std::string g_lastResult = "";
// series (docs/performance.md, "Benchmark series")
perf::Series g_series;
perf::SeriesMeta g_seriesMeta;
bool g_seriesHooks = false;   // the series needs the Direct3D hooks in every window (filter / count sets)
bool g_forceOverlay = false;  // the progress line lives in the frame-time window: show it during a series
std::string g_seriesReplay;   // replay path chosen for the series ("" none)

std::string isoNow() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[40];
    snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}
std::string stampNow() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[40];
    snprintf(b, sizeof(b), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}
std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
bool writeFile(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
bool readFile(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < (1 << 20)) out.append(buf, n);
    fclose(f);
    return true;
}

std::vector<std::pair<std::string, std::string>> optionList() {
    auto i = [](int v) { return std::to_string(v); };
    return {{"count", i(S.count && callsAllowed())},
            {"filter", i(S.filter && callsAllowed() && !perf::d3d::filterBlockedWhy()[0])},
            {"limitFps", i(S.limitFps)},
            {"vsync", i(S.vsync)},
            {"priority", i(S.priority)},
            {"affinity", i(S.affinity)},
            {"powerThrottlingOff", i(S.powerThrottlingOff)},
            {"fineTimer", i(S.fineTimer)},
            {"competitionPatch", i(g_competitionPatch)},
            {"wrapperD3d9", i(perf::d3d::filterBlockedWhy()[0] != 0)}};
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

void writeResult(const std::vector<perf::FrameSample>& frames, const std::string& label, int run, const char* request) {
    const perf::Summary s = perf::summarize(frames);
    perf::ResultMeta m;
    m.game = g_gameKey;
    m.label = perf::safeLabel(label);
    m.run = run;
    m.request = request;
    m.started = isoNow();
    m.options = optionList();
    const std::wstring dir = g_data + L"bench\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring base = dir + widen(stampNow() + "_" + m.label + (run > 1 ? "_" + std::to_string(run) : ""));
    const bool ok = writeFile(base + L".json", perf::summaryJson(s, m)) && writeFile(base + L".csv", perf::framesCsv(frames));
    char line[300];
    snprintf(line, sizeof(line), "%s run %d: %.1f fps avg, 1%% low %.1f, 0.1%% low %.1f, %u stutters, %u frames", m.label.c_str(), run,
             s.avgFps, s.low1Fps, s.low01Fps, static_cast<unsigned>(s.stutters), static_cast<unsigned>(s.frames));
    g_lastResult = line;
    logf(ok ? HORIZON_LOG_INFO : HORIZON_LOG_ERROR, "benchmark %s%s", line, ok ? "" : " (files could not be written)");
}

void benchStartRecording(double now) {
    g_recorded.clear();
    g_recorded.reserve(static_cast<size_t>(g_req.seconds * 400));
    g_bench = Bench::Recording;
    g_benchT = now;
    g_benchStatus = "recording run " + std::to_string(g_run);
}

void benchAbort(const char* why) {
    logf(HORIZON_LOG_WARN, "benchmark stopped: %s", why);
    g_benchStatus = std::string("stopped: ") + why;
    g_bench = Bench::Idle;
}

const char* stateName(int s) {
    switch (s) {
    case HORIZON_STATE_MAIN_MENU: return "main menu";
    case HORIZON_STATE_SOLO_RACE: return "solo race";
    case HORIZON_STATE_END_RACE: return "end of race";
    case HORIZON_STATE_NET_RACE: return "online race";
    case HORIZON_STATE_EDITOR: return "editor";
    case HORIZON_STATE_REPLAY: return "replay";
    case HORIZON_STATE_LOADING: return "loading";
    default: return "unknown";
    }
}

void registerOverlay();
void applyProcessOptions();

// ---- benchmark series
perf::SeriesOptions currentOptions() {
    perf::SeriesOptions o;
    o.count = S.count;
    o.filter = S.filter;
    o.limitFps = S.limitFps;
    o.vsync = S.vsync;
    o.priority = S.priority;
    o.affinity = S.affinity;
    o.powerThrottlingOff = S.powerThrottlingOff;
    o.fineTimer = S.fineTimer;
    return o;
}

std::string cpuName() {
    int r[4] = {};
    char b[49] = {};
    __cpuid(r, 0x80000000);
    if (static_cast<unsigned>(r[0]) < 0x80000004u) return "unknown";
    for (int i = 0; i < 3; i++) {
        __cpuid(r, 0x80000002 + i);
        memcpy(b + 16 * i, r, 16);
    }
    std::string s(b);
    const size_t a = s.find_first_not_of(' ');
    return a == std::string::npos ? "unknown" : s.substr(a);
}

void gpuAndResolution(std::string& gpu, std::string& res) {
    auto* dev = static_cast<IDirect3DDevice9*>(g_host->device());
    if (!dev) return;
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d) {
        D3DADAPTER_IDENTIFIER9 id = {};
        if (SUCCEEDED(dev->GetCreationParameters(&cp)) && SUCCEEDED(d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &id))) gpu = id.Description;
        d3d->Release();
    }
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt) {
        D3DSURFACE_DESC d = {};
        if (SUCCEEDED(rt->GetDesc(&d))) res = std::to_string(d.Width) + "x" + std::to_string(d.Height);
        rt->Release();
    }
}

// The newest *.Replay.Gbx in a folder ("" none).
std::wstring newestReplayIn(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.Replay.Gbx").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring best;
    ULONGLONG bestT = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const ULONGLONG t = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (t >= bestT) {
            bestT = t;
            best = dir + fd.cFileName;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}
bool fileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The replay the series is about: the given path or name, else the newest one in Tracks\Replays\Autosaves
// (then Tracks\Replays). The game cannot be told to open it (host ABI 1.3): the name is shown on screen.
std::string resolveReplay(const std::string& given) {
    const std::wstring root = g_userFolder.empty() ? L"" : g_userFolder + L"Tracks\\Replays\\";
    if (!given.empty()) {
        const std::wstring g = widen(given);
        if (g.find(L':') != std::wstring::npos || g.compare(0, 2, L"\\\\") == 0) return given;
        if (!root.empty()) {
            const std::wstring c[] = {root + g, root + g + L".Replay.Gbx", root + L"Autosaves\\" + g, root + L"Autosaves\\" + g + L".Replay.Gbx"};
            for (const auto& p : c)
                if (fileExists(p)) return narrow(p);
        }
        return given;
    }
    if (root.empty()) return "";
    std::wstring p = newestReplayIn(root + L"Autosaves\\");
    if (p.empty()) p = newestReplayIn(root);
    return narrow(p);
}

std::string fileNameOf(const std::string& path) {
    const size_t i = path.find_last_of("\\/");
    return i == std::string::npos ? path : path.substr(i + 1);
}

void writeSeriesFile(bool completed, const std::string& why) {
    g_seriesMeta.finished = isoNow();
    g_seriesMeta.completed = completed;
    g_seriesMeta.abortedWhy = why;
    const std::wstring dir = g_data + L"bench\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + widen(g_seriesMeta.stamp + "_series.json");
    const bool ok = writeFile(path, perf::seriesJson(g_series.plan(), g_series.activeSets(), g_series.runs(), g_seriesMeta));
    logf(ok ? HORIZON_LOG_INFO : HORIZON_LOG_ERROR, "benchmark series %s: %s%s%s", completed ? "done" : "stopped", ok ? narrow(path).c_str() : "file could not be written",
         why.empty() ? "" : " - ", why.c_str());
}

void applySeriesOptions(const perf::SeriesOptions& o);
void benchSeriesFinished(bool completed, const std::string& why);

struct PluginSeriesIO : perf::SeriesIO {
    void apply(const perf::SeriesOptions& o) override { applySeriesOptions(o); }
    std::string unavailable(const perf::SeriesOptions& o) override {
        if (o.filter || o.count) {
            if (!callsAllowed()) return "Competition Patch is loaded";
            if (!perf::d3d::attached()) return "the Direct3D hooks are not attached";
            if (o.filter && perf::d3d::filterBlockedWhy()[0]) return perf::d3d::filterBlockedWhy();
        }
        return "";
    }
    std::string note(const perf::SeriesOptions& o) override { return o.affinity ? perf::sys::affinityNote() : ""; }
    std::string isoNow() override { return ::isoNow(); }
    void runDone(perf::SeriesRun& r, const std::vector<perf::FrameSample>& frames) override {
        perf::ResultMeta m;
        m.game = g_gameKey;
        m.label = r.label;
        m.run = r.run;
        m.request = "series";
        m.started = r.started;
        m.options = optionList();
        const std::string base = g_seriesMeta.stamp + "_series_" + perf::safeLabel(r.label) + (r.run ? "_r" + std::to_string(r.run) : "_w" + std::to_string(r.warmupNo));
        const std::wstring dir = g_data + L"bench\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        const bool ok = writeFile(dir + widen(base + ".json"), perf::summaryJson(r.summary, m)) &&
                        writeFile(dir + widen(base + ".csv"), perf::framesCsv(frames));
        r.file = ok ? base + ".json" : "";
        char line[300];
        snprintf(line, sizeof(line), "series %s %s: %.1f fps avg, 1%% low %.1f, %u stutters, %u frames", r.label.c_str(),
                 r.run ? ("run " + std::to_string(r.run)).c_str() : "warm-up", r.summary.avgFps, r.summary.low1Fps,
                 static_cast<unsigned>(r.summary.stutters), static_cast<unsigned>(r.summary.frames));
        g_lastResult = line;
        logf(HORIZON_LOG_INFO, "%s", line);
    }
    void finished(bool completed, const std::string& why) override { benchSeriesFinished(completed, why); }
} g_seriesIO;

void applySeriesOptions(const perf::SeriesOptions& o) {
    // V-Sync is not touched: it is applied at device creation. Nothing is saved: the settings file keeps the user's own values.
    S.count = o.count;
    S.filter = o.filter;
    S.limitFps = o.limitFps;
    S.priority = o.priority;
    S.affinity = o.affinity;
    S.powerThrottlingOff = o.powerThrottlingOff;
    S.fineTimer = o.fineTimer;
    applyProcessOptions();
}

void benchSeriesFinished(bool completed, const std::string& why) {
    writeSeriesFile(completed, why);
    const bool map = g_req.series && g_req.series->where == "map";
    g_benchStatus = completed ? "series done: " + std::to_string(g_series.runs().size()) + " windows" : "series stopped: " + why;
    g_bench = Bench::Idle;
    g_seriesHooks = false;
    g_forceOverlay = false;
    registerOverlay();
    if (completed && map && g_req.returnToMenu) g_host->return_to_menu(g_host->plugin);
    g_reqFromLauncher = false;
}

void startSeries(double now) {
    const perf::SeriesPlan& plan = *g_req.series;
    perf::SeriesCaps caps;
    caps.vsyncInEffect = S.vsync;
    perf::SeriesMeta& m = g_seriesMeta;
    m = perf::SeriesMeta{};
    m.stamp = stampNow();
    m.started = isoNow();
    m.game = g_gameKey;
    m.cpu = cpuName();
    gpuAndResolution(m.gpu, m.resolution);
    m.where = plan.where;
    m.replay = plan.where == "replay" ? g_seriesReplay : "";
    m.map = plan.map;
    m.user = currentOptions();
    m.vsyncInEffect = S.vsync;
    const bool wrapper = perf::d3d::facts().wrapper;
    m.tmvs = GetModuleHandleW(L"TMVibrantShaders.dll") != nullptr || wrapper;
    m.tmvsNote = GetModuleHandleW(L"TMVibrantShaders.dll") ? "TMVibrantShaders.dll is loaded" : wrapper ? "another d3d9.dll is loaded (probably TM Vibrant Shaders standalone)" : "";
    for (const auto& s : plan.sets)
        if ((s.options.filter || s.options.count) && callsAllowed()) g_seriesHooks = true;
    std::string why;
    if (!g_series.start(plan, caps, m.user, now, g_seriesIO, why)) {
        g_seriesHooks = false;
        benchAbort(("series: " + why).c_str());
        return;
    }
    logf(HORIZON_LOG_INFO, "benchmark series started: %u sets, %d warm-up + %d runs, %.0f s each, %s%s%s", static_cast<unsigned>(g_series.activeSets().size()),
         plan.warmup, plan.runs, plan.seconds, plan.where.c_str(), m.replay.empty() ? "" : ", replay ", m.replay.c_str());
    g_bench = Bench::Series;
    g_benchStatus = g_series.progress();
}

void benchTick(double now, const perf::FrameSample* frame) {
    const int st = g_host->get_game_state();
    switch (g_bench) {
    case Bench::Idle: return;
    case Bench::WaitMenu:
        if (g_req.series && g_req.series->where == "now") {
            g_bench = Bench::WaitRace;
            g_benchT = now;
            g_benchStatus = "Benchmark: starting";
            return;
        }
        if (g_req.mode == "replay") {
            g_bench = Bench::WaitRace;
            g_benchT = now;
            g_benchStatus = g_req.series ? "Benchmark: open " + (g_seriesReplay.empty() ? std::string("a replay") : "the replay " + fileNameOf(g_seriesReplay)) +
                                               ", the series starts by itself"
                                         : "open a replay now; recording starts by itself";
            return;
        }
        if (st != HORIZON_STATE_MAIN_MENU) {
            g_benchT = now;
            return;
        }
        if (now - g_benchT < 3000) return; // let the menu settle
        {
            const HorizonResult r = g_host->play_map_file(g_host->plugin, g_req.map.c_str(), 0);
            if (r != HORIZON_OK) {
                char why[160];
                snprintf(why, sizeof(why), "the map could not be started (%d)", static_cast<int>(r));
                benchAbort(why);
                return;
            }
        }
        g_bench = Bench::WaitRace;
        g_benchT = now;
        g_benchStatus = "loading the map";
        return;
    case Bench::WaitRace: {
        const bool anyScene = g_req.series && g_req.series->where == "now";
        const bool ready = anyScene ? now - g_benchT > 1500 : g_req.mode == "replay" ? st == HORIZON_STATE_REPLAY : st == HORIZON_STATE_SOLO_RACE;
        if (ready) {
            g_bench = Bench::Warmup;
            g_benchT = now;
            g_benchStatus = "warm-up";
        } else if (g_req.mode == "map" && now - g_benchT > 120000) {
            benchAbort("the race did not start within 2 minutes");
        }
        return;
    }
    case Bench::Warmup:
        if (now - g_benchT < g_req.warmup * 1000.0) return;
        if (g_req.series) startSeries(now);
        else benchStartRecording(now);
        return;
    case Bench::Series: {
        const std::string& where = g_req.series->where;
        const bool sceneOk = where == "now" || (where == "replay" ? st == HORIZON_STATE_REPLAY : (st == HORIZON_STATE_SOLO_RACE || st == HORIZON_STATE_END_RACE));
        if (!sceneOk) {
            if (where == "replay") { // the replay was closed: wait for it, the window starts over
                g_series.pause();
                g_benchStatus = "Benchmark paused: open the replay again";
                return;
            }
            g_series.abort("the race ended during the series", g_seriesIO);
            return;
        }
        if (g_series.waiting()) g_series.resume(now);
        g_series.tick(now, frame, g_seriesIO);
        if (g_series.active()) g_benchStatus = g_series.progress();
        return;
    }
    case Bench::Recording: {
        const bool stillThere = !g_reqFromLauncher || (g_req.mode == "replay" ? st == HORIZON_STATE_REPLAY
                                                                              : (st == HORIZON_STATE_SOLO_RACE || st == HORIZON_STATE_END_RACE));
        if (frame) g_recorded.push_back(*frame);
        if (!stillThere) {
            if (g_recorded.size() > 30) writeResult(g_recorded, g_req.label + "-partial", g_run, g_reqFromLauncher ? g_req.mode.c_str() : "manual");
            benchAbort("the race or replay ended during the recording");
            return;
        }
        if (now - g_benchT < g_req.seconds * 1000.0) return;
        writeResult(g_recorded, g_req.label, g_run, g_reqFromLauncher ? g_req.mode.c_str() : "manual");
        if (g_run < g_req.runs) {
            g_run++;
            benchStartRecording(now);
            return;
        }
        g_bench = Bench::Idle;
        g_benchStatus = "done: " + std::to_string(g_req.runs) + (g_req.runs == 1 ? " run" : " runs");
        if (g_reqFromLauncher && g_req.mode == "map" && g_req.returnToMenu) g_host->return_to_menu(g_host->plugin);
        g_reqFromLauncher = false;
        return;
    }
    }
}

void startManualBench() {
    if (g_bench == Bench::Series) {
        g_series.abort("stopped by hand", g_seriesIO);
        return;
    }
    if (g_bench != Bench::Idle) {
        benchAbort("stopped by hand");
        return;
    }
    g_req = perf::BenchRequest{};
    g_req.label = perf::safeLabel(S.benchLabel);
    g_req.seconds = S.benchSeconds < 5 ? 5 : S.benchSeconds;
    g_req.warmup = 0;
    g_req.runs = 1;
    g_reqFromLauncher = false;
    g_run = 1;
    benchStartRecording(perf::sys::nowMs());
}
void HORIZON_CALL onBenchHotkey(void*) { startManualBench(); }

void readLauncherRequest() {
    const std::wstring path = g_data + L"bench-request.json";
    std::string text, why;
    if (!readFile(path, text)) return;
    // taken: a crash during the benchmark must not start it again at every start
    MoveFileExW(path.c_str(), (g_data + L"bench-request.taken.json").c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!perf::parseBenchRequest(text, g_req, why)) {
        logf(HORIZON_LOG_WARN, "bench-request.json ignored: %s", why.c_str());
        g_benchStatus = "launcher request ignored: " + why;
        return;
    }
    g_reqFromLauncher = true;
    g_run = 1;
    g_bench = Bench::WaitMenu;
    g_benchT = perf::sys::nowMs();
    g_benchStatus = g_req.mode == "map" ? "waiting for the main menu" : "open a replay now";
    if (g_req.series) {
        g_req.warmup = g_req.series->where == "now" ? 0 : 3;
        if (g_req.series->where == "replay") g_seriesReplay = resolveReplay(g_req.series->replay);
        g_forceOverlay = true;
        g_benchStatus = "Benchmark: waiting";
        logf(HORIZON_LOG_INFO, "benchmark series from the launcher: %u sets, %d runs of %.0f s, %s%s%s", static_cast<unsigned>(g_req.series->sets.size()),
             g_req.runs, g_req.seconds, g_req.series->where.c_str(), g_seriesReplay.empty() ? "" : ", replay ", g_seriesReplay.c_str());
    }
    logf(HORIZON_LOG_INFO, "benchmark from the launcher: %s, %s, %.0f s x %d, warm-up %.0f s", g_req.label.c_str(), g_req.mode.c_str(),
         g_req.seconds, g_req.runs, g_req.warmup);
}

// ---- options
void applyProcessOptions() {
    if (!perf::sys::setPriority(S.priority)) logf(HORIZON_LOG_WARN, "priority %d refused by Windows", S.priority);
    if (!perf::sys::setAffinity(S.affinity)) logf(HORIZON_LOG_WARN, "core selection %d refused by Windows", S.affinity);
    perf::sys::setPowerThrottlingOff(S.powerThrottlingOff != 0);
    perf::sys::setFineTimer(S.fineTimer != 0);
    g_pacer.setFps(S.limitFps);
    g_pacer.reset();
    perf::d3d::setMode(S.count && callsAllowed(), S.filter && callsAllowed());
    perf::stall::setEnabled(S.hangLog != 0);
    perf::stall::setThresholdMs(S.hangMs);
}

void d3dLog(int level, const char* fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logf(level, "%s", buf);
}

void ensureHooks(void* device) {
    if (!device || !callsAllowed() || (!S.count && !S.filter && !g_seriesHooks) || perf::d3d::attached()) return;
    if (GetModuleHandleW(L"TMCompPatch.dll")) {
        g_competitionPatch = true; // loaded after my Init: never hook
        return;
    }
    perf::d3d::attach(static_cast<IDirect3DDevice9*>(device), &d3dLog);
    perf::d3d::setMode(S.count != 0, S.filter != 0);
}

// ---- device callbacks
void HORIZON_CALL cbDeviceCreated(void*, void* device, void* params) {
    if (params) g_window = static_cast<D3DPRESENT_PARAMETERS*>(params)->hDeviceWindow;
    if (device) {
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        if (SUCCEEDED(static_cast<IDirect3DDevice9*>(device)->GetCreationParameters(&cp)) && cp.hFocusWindow) g_window = cp.hFocusWindow;
    }
    perf::stall::note("device created");
    ensureHooks(device);
}
void HORIZON_CALL cbAdjustParams(void*, void* params) {
    if (!params || S.vsync == 0) return;
    auto* pp = static_cast<D3DPRESENT_PARAMETERS*>(params);
    pp->PresentationInterval = S.vsync == 1 ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
}
void HORIZON_CALL cbBeforeReset(void*, void*) { perf::d3d::onReset(); }
void HORIZON_CALL cbAfterReset(void*, void*, int32_t) {
    perf::d3d::onReset();
    g_pacer.reset();
}

void finishFrame(double tPre) {
    if (g_tPrevPre <= 0) {
        g_tPrevPre = tPre;
        return;
    }
    perf::FrameSample f;
    f.frameMs = tPre - g_tPrevPre;
    f.presentMs = g_presentMs;
    f.sleepMs = g_sleepMs;
    f.cpuMs = f.frameMs - f.presentMs - f.sleepMs;
    if (f.cpuMs < 0) f.cpuMs = 0;
    if (perf::d3d::attached()) {
        perf::d3d::endFrame(f.calls);
        f.hasCalls = S.count && callsAllowed();
    }
    g_tPrevPre = tPre;
    g_sleepMs = 0;
    // a frame longer than 5 s is a loading screen or a hang, not a frame: keep it out of the statistics
    if (f.frameMs < 5000) g_ring.push(f);
    benchTick(tPre, f.frameMs < 5000 ? &f : nullptr);
    // one line in the Horizon log after 100 frames and then every 30 s: enough to compare starts
    static double s_nextLog = 0;
    static uint64_t s_frames = 0;
    if (++s_frames == 100 || (s_nextLog > 0 && tPre >= s_nextLog)) {
        s_nextLog = tPre + 30000;
        perf::CallCounts sum;
        size_t n = 0, withCalls = 0;
        double total = 0;
        for (size_t i = g_ring.size(); i > 0 && total < 30000; i--, n++) {
            const perf::FrameSample& x = g_ring.at(i - 1);
            total += x.frameMs;
            if (!x.hasCalls) continue;
            withCalls++;
            sum.draws += x.calls.draws + x.calls.drawsUP;
            sum.renderStates += x.calls.renderStates + x.calls.samplerStates + x.calls.textureStageStates;
            sum.renderStatesSame += x.calls.renderStatesSame + x.calls.samplerStatesSame + x.calls.textureStageStatesSame;
            sum.locks += x.calls.locks;
            sum.filtered += x.calls.filtered;
            sum.queryWaitMs += x.calls.queryWaitMs;
            sum.drawsSoftwareVP += x.calls.drawsSoftwareVP;
        }
        const perf::Summary s = perf::summarize(g_ring.last(n));
        if (withCalls) {
            const double k = 1.0 / static_cast<double>(withCalls);
            logf(HORIZON_LOG_INFO,
                 "stats: %.0f fps, 1%% low %.0f, %u stutters, game %.2f ms, Present %.2f ms; per frame: %.0f draws (%.0f software VP), "
                 "%.0f states (%.0f same), %.1f locks, GPU wait %.2f ms, filtered %.0f",
                 s.avgFps, s.low1Fps, static_cast<unsigned>(s.stutters), s.cpuMsAvg, s.presentMsAvg, sum.draws * k, sum.drawsSoftwareVP * k,
                 sum.renderStates * k, sum.renderStatesSame * k, sum.locks * k, sum.queryWaitMs * k, sum.filtered * k);
        } else {
            logf(HORIZON_LOG_INFO, "stats: %.0f fps, 1%% low %.0f, %u stutters, game %.2f ms, Present %.2f ms, limiter %.2f ms", s.avgFps,
                 s.low1Fps, static_cast<unsigned>(s.stutters), s.cpuMsAvg, s.presentMsAvg, s.sleepMsAvg);
        }
    }
}

void HORIZON_CALL cbPrePresent(void*, void* device) {
    const double t = perf::sys::nowMs();
    perf::stall::frame(g_window);
    ensureHooks(device);
    finishFrame(t);
    g_tPre = t;
    if (++g_frameNo % 60 == 0) {
        // TMLoader may load Competition Patch after Horizon: from then on no method-table hooks of mine
        if (!g_competitionPatch && GetModuleHandleW(L"TMCompPatch.dll")) {
            g_competitionPatch = true;
            perf::d3d::detach();
            logf(HORIZON_LOG_INFO, "Competition Patch loaded later: call counter and state filter switched off");
            perf::stall::note("Competition Patch loaded");
        }
        if (perf::d3d::attached()) perf::d3d::recheck();
    }
    if (g_hasPostPresent && perf::d3d::attached()) perf::d3d::setOverlay(true); // Horizon's own drawing follows
    if (!g_hasPostPresent && S.limitFps > 0) {
        // older host: pace before Present (one frame more input delay than after it)
        const double target = g_pacer.next(t / 1000.0) * 1000.0; // the pacer works in seconds
        g_sleepMs = perf::sys::waitUntil(target);
    }
}

void HORIZON_CALL cbPostPresent(void*, void*, int32_t) {
    const double t = perf::sys::nowMs();
    g_tPost = t;
    g_presentMs = t - g_tPre;
    if (perf::d3d::attached()) perf::d3d::setOverlay(false);
    if (S.limitFps > 0) {
        const double target = g_pacer.next(t / 1000.0) * 1000.0; // the pacer works in seconds
        g_sleepMs = perf::sys::waitUntil(target);
    }
}

// ---- engine facts by reflection name (read only)
struct EngineView {
    bool ok = false;
    float averageFps = 0;
    int targetFpsEnable = -1;
    unsigned targetFps = 0;
};
EngineView readEngine() {
    EngineView v;
    if (!g_host->reflect_offset) return v;
    void* app = g_host->engine_pointer(HORIZON_ENGINE_APP);
    const int32_t vpOff = g_host->reflect_offset("CGameApp", "Viewport");
    const int32_t fpsOff = g_host->reflect_offset("CHmsViewport", "AverageFps");
    const int32_t tfeOff = g_host->reflect_offset("CHmsViewport", "TargetFpsEnable");
    const int32_t tfOff = g_host->reflect_offset("CHmsViewport", "TargetFps");
    if (!app || vpOff < 0) return v;
    __try {
        auto* vp = *reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(app) + vpOff);
        if (!vp) return v;
        if (fpsOff >= 0) v.averageFps = *reinterpret_cast<float*>(vp + fpsOff);
        if (tfeOff >= 0) v.targetFpsEnable = *reinterpret_cast<int*>(vp + tfeOff) ? 1 : 0;
        if (tfOff >= 0) v.targetFps = *reinterpret_cast<unsigned*>(vp + tfOff);
        v.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v.ok = false;
    }
    return v;
}

// ---- drawing
ImVec4 col(const HorizonColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }

void refreshLive(double now) {
    if (now - g_liveAt < 500 && g_live.frames) return;
    g_liveAt = now;
    // the last ~10 s
    size_t n = 0;
    double sum = 0;
    for (size_t i = g_ring.size(); i > 0 && sum < 10000; i--, n++) sum += g_ring.at(i - 1).frameMs;
    g_live = perf::summarize(g_ring.last(n));
}

void drawGraph(float width, float height) {
    const HorizonStyle* st = g_host->style();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), ImGui::GetColorU32(ImVec4(0, 0, 0, 0.35f)), st ? st->radius_sm : 3);
    const size_t want = static_cast<size_t>(width / 2);
    const std::vector<perf::FrameSample> fr = g_ring.last(want);
    double maxMs = 1;
    for (const auto& f : fr) maxMs = f.frameMs > maxMs ? f.frameMs : maxMs;
    const double med = g_live.medianMs > 0 ? g_live.medianMs : 16.7;
    double scale = med * 3 > maxMs ? med * 3 : maxMs;
    if (scale > 100) scale = 100;
    const ImU32 okC = ImGui::GetColorU32(st ? col(st->accent) : ImVec4(0.3f, 0.8f, 1, 1));
    const ImU32 badC = ImGui::GetColorU32(st ? col(st->bad) : ImVec4(1, 0.3f, 0.3f, 1));
    const ImU32 lineC = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.25f));
    const float y2 = p.y + height - static_cast<float>(2 * med / scale * height);
    if (y2 > p.y) dl->AddLine(ImVec2(p.x, y2), ImVec2(p.x + width, y2), lineC); // 2x median: the stutter line
    for (size_t i = 0; i < fr.size(); i++) {
        const float x = p.x + width - static_cast<float>(fr.size() - i) * 2.0f;
        double v = fr[i].frameMs / scale;
        if (v > 1) v = 1;
        const float h = static_cast<float>(v * height);
        dl->AddRectFilled(ImVec2(x, p.y + height - h), ImVec2(x + 1.5f, p.y + height), fr[i].frameMs > 2 * med ? badC : okC);
    }
    ImGui::Dummy(ImVec2(width, height));
}

void HORIZON_CALL drawOverlayPanel(void*) {
    if (!g_imgui) return;
    const double now = perf::sys::nowMs();
    refreshLive(now);
    const HorizonStyle* st = g_host->style();
    const float sc = st && st->scale > 0 ? st->scale : 1.0f;
    ImGui::Text("%5.0f fps  %5.2f ms  1%% %4.0f  0.1%% %4.0f  stutter %u", g_live.avgFps, g_live.avgMs, g_live.low1Fps, g_live.low01Fps,
                static_cast<unsigned>(g_live.stutters));
    ImGui::TextDisabled("game %.2f ms  present %.2f ms  wait %.2f ms", g_live.cpuMsAvg, g_live.presentMsAvg, g_live.sleepMsAvg);
    drawGraph(320 * sc, 70 * sc);
    if (S.overlayCalls && g_ring.size() && g_ring.newest().hasCalls) {
        const perf::CallCounts& c = g_ring.newest().calls;
        const uint32_t states = c.renderStates + c.samplerStates + c.textureStageStates;
        const uint32_t same = c.renderStatesSame + c.samplerStatesSame + c.textureStageStatesSame;
        ImGui::TextDisabled("draws %u  states %u (%u same)  tex %u  shaders %u", c.draws + c.drawsUP, states, same, c.textures,
                            c.vertexShaders + c.pixelShaders);
        ImGui::TextDisabled("locks %u (%.2f ms)  readback %u  GPU wait %.2f ms  filtered %u", c.locks, c.lockMs, c.readbacks, c.queryWaitMs,
                            c.filtered);
    }
    if (g_bench == Bench::Series) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", g_benchStatus.c_str());
    else if (g_bench != Bench::Idle) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Benchmark: %s", g_benchStatus.c_str());
}

void registerOverlay() {
    if (!g_host || g_host->abi_minor < 2) return;
    if (!S.overlay && !g_forceOverlay) {
        g_host->unregister_panel(g_host->plugin, "graph");
        return;
    }
    HorizonPanelDef d = {};
    d.size = sizeof(d);
    d.key = "graph";
    d.title = "Frame times";
    d.icon = HZ_ICON_TIMER;
    d.flags = HORIZON_PANEL_HUD;
    d.draw = &drawOverlayPanel;
    d.default_x = 0.70f;
    d.default_y = 0.06f;
    g_host->register_panel(g_host->plugin, &d);
}

bool checkbox(const char* label, int* v, const char* key) {
    bool b = *v != 0;
    if (!ImGui::Checkbox(label, &b)) return false;
    *v = b ? 1 : 0;
    saveInt(key, *v);
    return true;
}
void hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void exportLast60() {
    size_t n = 0;
    double sum = 0;
    for (size_t i = g_ring.size(); i > 0 && sum < 60000; i--, n++) sum += g_ring.at(i - 1).frameMs;
    const std::wstring dir = g_data + L"bench\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + widen(stampNow() + "_last60s.csv");
    const bool ok = writeFile(path, perf::framesCsv(g_ring.last(n)));
    g_lastResult = ok ? "saved the last 60 s as CSV" : "CSV could not be written";
}

void drawMenu() {
    const double now = perf::sys::nowMs();
    refreshLive(now);
    if (g_competitionPatch)
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Competition Patch is loaded: call counting and the state filter are off.");

    ImGui::SeparatorText("Measure");
    ImGui::Text("Last 10 s: %.0f fps avg, 1%% low %.0f, 0.1%% low %.0f, %u stutters", g_live.avgFps, g_live.low1Fps, g_live.low01Fps,
                static_cast<unsigned>(g_live.stutters));
    ImGui::TextDisabled("per frame: game %.2f ms, Present %.2f ms, limiter %.2f ms", g_live.cpuMsAvg, g_live.presentMsAvg, g_live.sleepMsAvg);
    if (checkbox("Frame-time window", &S.overlay, "overlay")) registerOverlay();
    hint("A small window with FPS, lows and a graph of the last frames. Stays on screen with the menu closed.");
    ImGui::BeginDisabled(!callsAllowed());
    if (checkbox("Count Direct3D calls", &S.count, "count")) applyProcessOptions();
    ImGui::EndDisabled();
    hint("Draw calls, state changes, locks and GPU waits per frame. Costs a little time itself.");
    if (S.count) {
        ImGui::SameLine();
        checkbox("in the window", &S.overlayCalls, "overlay_calls");
    }
    if (perf::d3d::attached()) {
        const auto& f = perf::d3d::facts();
        ImGui::TextDisabled("Device: flags 0x%08lx, %s%s%s", static_cast<unsigned long>(f.behaviorFlags), f.pure ? "pure, " : "",
                            f.mixed ? "mixed vertex processing" : f.software ? "software vertex processing" : "hardware vertex processing",
                            f.multithreaded ? ", multithreaded" : "");
        if (g_ring.size() && g_ring.newest().hasCalls && g_ring.newest().calls.drawsSoftwareVP)
            ImGui::TextDisabled("%u draws per frame use software vertex processing (CPU)", g_ring.newest().calls.drawsSoftwareVP);
    }
    if (ImGui::Button("Save last 60 s")) exportLast60();
    ImGui::SameLine();
    ImGui::TextDisabled("CSV in the bench folder");

    ImGui::SeparatorText("Benchmark");
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("Seconds", &S.benchSeconds, 10, 120)) saveInt("bench_seconds", S.benchSeconds);
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputText("Label", S.benchLabel, sizeof(S.benchLabel))) g_host->set_string(g_host->plugin, "bench_label", S.benchLabel);
    if (ImGui::Button(g_bench == Bench::Idle ? "Start" : "Stop")) startManualBench();
    ImGui::SameLine();
    const char* hk = g_benchHotkey ? g_host->hotkey_text(g_host->plugin, g_benchHotkey) : "";
    ImGui::TextDisabled("records from now on%s%s", hk && *hk ? ", hotkey " : "", hk ? hk : "");
    if (!g_benchStatus.empty()) ImGui::Text("%s", g_benchStatus.c_str());
    if (!g_lastResult.empty()) ImGui::TextDisabled("%s", g_lastResult.c_str());

    ImGui::SeparatorText("Options (each off until measured)");
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputInt("Frame limit (0 = off)", &S.limitFps, 10, 30)) {
        if (S.limitFps < 0) S.limitFps = 0;
        if (S.limitFps > 0 && S.limitFps < 30) S.limitFps = 30;
        if (S.limitFps > 1000) S.limitFps = 1000;
        saveInt("limit_fps", S.limitFps);
        applyProcessOptions();
    }
    hint("Waits right after Present, so the game reads input just before the next frame. Steadier frame times, less heat.");
    static const char* kVsync[] = {"as the game sets it", "off", "on"};
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo("V-Sync", &S.vsync, kVsync, 3)) saveInt("vsync", S.vsync);
    hint("Applies at the next start or display change (Alt+Enter).");
    ImGui::BeginDisabled(!callsAllowed() || perf::d3d::filterBlockedWhy()[0] != 0);
    if (checkbox("Skip repeated states", &S.filter, "filter")) applyProcessOptions();
    ImGui::EndDisabled();
    hint("Does not pass state calls on that repeat the value already set. Experimental: compare with the benchmark.");
    if (perf::d3d::filterBlockedWhy()[0]) ImGui::TextDisabled("  not available: %s", perf::d3d::filterBlockedWhy());
    if (checkbox("Fine timer (0.5 ms)", &S.fineTimer, "fine_timer")) applyProcessOptions();
    hint("The game already asks Windows for 1 ms. This asks for 0.5 ms (more precise waits, slightly more power).");
    ImGui::SameLine();
    ImGui::TextDisabled("now %.2f ms", perf::sys::currentTimerMs());
    if (checkbox("No power throttling", &S.powerThrottlingOff, "power_throttling_off")) applyProcessOptions();
    hint("Windows must not slow the game down (EcoQoS) or ignore its timer when the window is in the background.");
    static const char* kPrio[] = {"as the game sets it", "above normal", "high"};
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo("Priority", &S.priority, kPrio, 3)) {
        saveInt("priority", S.priority);
        applyProcessOptions();
    }
    static const char* kAff[] = {"all", "all but CPU 0", "performance cores"};
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo("CPU cores", &S.affinity, kAff, 3)) {
        saveInt("affinity", S.affinity);
        applyProcessOptions();
    }
    if (S.affinity) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", perf::sys::affinityNote());
    }

    ImGui::SeparatorText("Hang log");
    if (checkbox("Log where the game waits when no frame comes", &S.hangLog, "hang_log")) applyProcessOptions();
    hint("For long loading stalls and the replay list refresh. Never while a race runs. Writes hangs.log in the plugin's data folder.");
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("after ms", &S.hangMs, 500, 5000)) {
        saveInt("hang_ms", S.hangMs);
        applyProcessOptions();
    }
    const perf::stall::Last ls = perf::stall::last();
    if (ls.count) ImGui::TextDisabled("%d stalls this start, last %.1f s in %s", ls.count, ls.seconds, ls.top[0] ? ls.top : "-");

    ImGui::SeparatorText("Engine and memory (read only)");
    const EngineView ev = readEngine();
    if (ev.ok) {
        ImGui::TextDisabled("Engine's own FPS %.0f, adaptive view distance %s (target %u fps)", ev.averageFps,
                            ev.targetFpsEnable < 0 ? "?" : ev.targetFpsEnable ? "on" : "off", ev.targetFps);
    } else {
        ImGui::TextDisabled("Engine values not readable on this game");
    }
    const perf::sys::Memory m = perf::sys::memory();
    ImGui::TextDisabled("Memory: %.0f MB private, address space %.0f of %.0f MB used, %s", m.privateMB, m.addressSpaceUsedMB,
                        m.addressSpaceTotalMB, m.largeAddressAware ? "large address aware" : "exe not large address aware (2 GB)");
    char folder[300];
    WideCharToMultiByte(CP_UTF8, 0, g_data.c_str(), -1, folder, sizeof(folder), nullptr, nullptr);
    ImGui::TextDisabled("Files: %s", folder);
}

const HorizonPluginInfo kInfo = {
    HORIZON_ABI,
    "performance",
    "Horizon Performance",
    "0.1.0",
    HORIZON_GAME_ESWC | HORIZON_GAME_SUNRISE | HORIZON_GAME_TMNF | HORIZON_GAME_TMUF,
    HORIZON_CAP_MENU | HORIZON_CAP_DEVICE | HORIZON_CAP_GAME_EVENTS | HORIZON_CAP_GAME_CONTROL,
    HORIZON_FLAG_ONLINE_OK | HORIZON_FLAG_LIVE_TOGGLE,
    "cheatoskar",
    "Frame times, Direct3D call counts, benchmark, frame limiter and a hang log.",
    HORIZON_ABI_MINOR,
};

} // namespace

extern "C" {

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) { return &kInfo; }

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    g_host = host;
    const HorizonGameInfo* gi = host->game_info();
    g_game = gi ? gi->game : 0;
    g_gameKey = g_game == HORIZON_GAME_ESWC ? "eswc" : g_game == HORIZON_GAME_SUNRISE ? "sunrise" : g_game == HORIZON_GAME_TMNF ? "tmnf"
              : g_game == HORIZON_GAME_TMUF ? "tmuf" : "other";
    g_imgui = horizon_imgui_attach(host);
    if (!g_imgui) logf(HORIZON_LOG_WARN, "Dear ImGui could not attach: no page, measuring still works");
    g_data = host->data_folder_w(host->plugin);
    g_competitionPatch = GetModuleHandleW(L"TMCompPatch.dll") != nullptr;
    g_hasPostPresent = host->abi_minor >= 3;
    loadSettings();
    perf::sys::captureOriginals();

    std::wstring gameFolder;
    if (gi && gi->game_folder) gameFolder = widen(gi->game_folder);
    if (gi && gi->user_folder && *gi->user_folder) {
        g_userFolder = widen(gi->user_folder);
        if (g_userFolder.back() != L'\\' && g_userFolder.back() != L'/') g_userFolder += L'\\';
    }
    perf::stall::start(g_data.c_str(), gameFolder.c_str(), gi ? gi->exe_timestamp : 0);
    perf::stall::note("Horizon Performance started on %s (host %s, %s)", g_gameKey.c_str(), gi && gi->host_version ? gi->host_version : "?",
                      g_competitionPatch ? "Competition Patch loaded" : "no Competition Patch");
    applyProcessOptions();

    HorizonDeviceCallbacks cb = {};
    cb.size = sizeof(cb);
    cb.device_created = &cbDeviceCreated;
    cb.adjust_present_params = &cbAdjustParams;
    cb.before_reset = &cbBeforeReset;
    cb.after_reset = &cbAfterReset;
    cb.pre_present = &cbPrePresent;
    cb.post_present = &cbPostPresent;
    host->set_device_callbacks(host->plugin, &cb);
    host->subscribe(host->plugin, HORIZON_EVENT_BIT(HORIZON_EVENT_GAME_STATE) | HORIZON_EVENT_BIT(HORIZON_EVENT_MAP_LOADED) |
                                      HORIZON_EVENT_BIT(HORIZON_EVENT_MENU_ENTERED) | HORIZON_EVENT_BIT(HORIZON_EVENT_ONLINE_CHANGED));
    g_benchHotkey = host->register_hotkey(host->plugin, "bench", "Benchmark start/stop", "Ctrl+F10", &onBenchHotkey, nullptr);
    readLauncherRequest();
    registerOverlay();
    logf(HORIZON_LOG_INFO, "started: post_present %s, timer %.2f ms, %s", g_hasPostPresent ? "yes" : "no (older host: limiter before Present)",
         perf::sys::currentTimerMs(), g_competitionPatch ? "Competition Patch: call counter and filter off" : "all options available");
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    perf::stall::note("shutdown");
    if (g_series.active()) g_series.abort("the game was closed", g_seriesIO);
    perf::stall::stop();
    perf::d3d::detach();
    perf::sys::setFineTimer(false);
    g_host = nullptr;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawMenu(void) {
    if (g_imgui) drawMenu();
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_OnEvent(const HorizonEvent* e) {
    if (!e || e->size < offsetof(HorizonEvent, game_state) + sizeof(uint32_t)) return;
    const double t = perf::sys::secondsSinceProcessStart();
    switch (e->type) {
    case HORIZON_EVENT_GAME_STATE: {
        const int s = static_cast<int>(e->game_state);
        perf::stall::setState(stateName(s), s == HORIZON_STATE_SOLO_RACE || s == HORIZON_STATE_NET_RACE);
        perf::stall::note("state: %s", stateName(s));
        logf(HORIZON_LOG_INFO, "timeline: %s at %.1f s after the game started", stateName(s), t);
        break;
    }
    case HORIZON_EVENT_MAP_LOADED:
        perf::stall::note("map loaded: %s", e->map && e->map->name ? e->map->name : "?");
        break;
    case HORIZON_EVENT_ONLINE_CHANGED:
        perf::stall::note("online: %s", e->is_online ? "yes" : "no");
        break;
    default: break;
    }
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_SetEnabled(int32_t enabled) {
    g_enabled = enabled != 0;
    if (!g_enabled && g_series.active()) g_series.abort("the plugin was switched off", g_seriesIO);
    if (!g_enabled) {
        perf::d3d::setMode(false, false);
        perf::sys::setPriority(0);
        perf::sys::setAffinity(0);
        perf::sys::setPowerThrottlingOff(false);
        perf::sys::setFineTimer(false);
        perf::stall::setEnabled(false);
        g_pacer.reset();
    } else {
        applyProcessOptions();
    }
}

} // extern "C"
