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
#include "gpusync.h"
#include "gpusync_live.h"
#include "perf_core.h"
#include "series.h"
#include "shadercache.h"
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
    int threadCores = 0, timer1ms = 0, mmcss = 0; // smoothing switches, independent of the preset (docs/performance-smooth.md)
    int shaderCache = 0;    // shader disk cache, ESWC/Sunrise/Original only (shadercache.h)
    int preset = 0;         // perf::Preset: 0 Smooth (default), 1 Low latency, 2 Custom
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
    S.threadCores = gi("thread_cores", S.threadCores);
    S.timer1ms = gi("timer_1ms", S.timer1ms);
    S.mmcss = gi("mmcss", S.mmcss);
    S.shaderCache = gi("shader_cache", S.shaderCache);
    // the choice: saved, or Custom for a player who set options before presets existed, else Smooth
    S.preset = gi("preset", -1);
    if (S.preset < perf::PresetSmooth || S.preset > perf::PresetCustom) {
        bool own = false;
        for (const char* k : {"count", "filter", "limit_fps", "vsync", "priority", "affinity", "power_throttling_off", "fine_timer"})
            own = own || gi(k, -9999) != -9999;
        S.preset = own ? perf::PresetCustom : perf::PresetSmooth;
    }
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
double g_liveHookCalls = -1; // calls through my Direct3D hooks per frame in those 10 s (-1: no hooks)
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
perf::StartBy g_startBy = perf::StartBy::None; // how the launcher request started (Key: the scene is not checked)
bool g_keyEdge = false;       // the benchmark key was pressed while a request waits (read by benchTick)
double g_sceneLostAt = 0;     // series: when the scene went away (0 = it is there)
std::string g_doneText;       // the banner after the end ("Benchmark fertig: ...")
double g_doneAt = 0;
// frames that are not gameplay (FrameSample::skip)
double g_settleUntil = 0;     // frames before this time are "settle" (after a Reset, a focus or state change)
bool g_fgPrev = true;         // the game window was in front at the last frame
bool g_presentFailed = false; // the last Present returned an error (device lost)
bool g_assumeFront = false;   // tests (HORIZON_TEST_FOREGROUND=1): the test window counts as in front

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
            {"threadCores", i(S.threadCores)},
            {"timer1ms", i(S.timer1ms)},
            {"mmcss", i(S.mmcss)},
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
    const perf::Summary s = perf::summarize(frames, perf::kSkipUnfocused | perf::kSkipLost | perf::kSkipSettle);
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
bool bannerLines(std::string& l1, std::string& l2);

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
    o.threadCores = S.threadCores;
    o.timer1ms = S.timer1ms;
    o.mmcss = S.mmcss;
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
    S.threadCores = o.threadCores;
    S.timer1ms = o.timer1ms;
    S.mmcss = o.mmcss;
    applyProcessOptions();
}

void benchSeriesFinished(bool completed, const std::string& why) {
    writeSeriesFile(completed, why);
    const bool map = g_req.series && g_req.series->where == "map";
    g_benchStatus = completed ? "series done: " + std::to_string(g_series.runs().size()) + " windows" : "series stopped: " + why;
    g_doneText = completed ? "Benchmark fertig: " + g_seriesMeta.stamp + "_series.json im Ordner bench" : "Benchmark abgebrochen: " + why;
    g_doneAt = perf::sys::nowMs();
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

// The scene a series measures went away / came back. Short losses (a replay restarting, a loading hitch) only
// skip frames; after 5 s the window starts over (replay) or the series stops (map: the race ended).
bool seriesScene(double now, int st) {
    const std::string& where = g_req.series->where;
    if (perf::benchSceneOk(where, g_startBy, st)) {
        if (g_sceneLostAt > 0) {
            g_sceneLostAt = 0;
            const double settle = g_req.series->settle * 1000.0;
            if (now + settle > g_settleUntil) g_settleUntil = now + settle; // a replay restart counts as settle
            if (g_series.waiting()) g_series.resume(now);
        }
        return true;
    }
    if (g_sceneLostAt <= 0) g_sceneLostAt = now;
    if (now - g_sceneLostAt > 5000) {
        if (where == "map") {
            g_series.abort("the race ended during the series", g_seriesIO);
            return false;
        }
        g_series.pause(); // the window starts over when the replay is back
    }
    return false;
}

void benchTick(double now, const perf::FrameSample* frame) {
    const int st = g_host->get_game_state();
    const bool key = g_keyEdge;
    g_keyEdge = false;
    switch (g_bench) {
    case Bench::Idle: return;
    case Bench::WaitMenu:
        if ((g_req.series && g_req.series->where != "map") || (!g_req.series && g_req.mode == "replay")) {
            g_bench = Bench::WaitRace;
            g_benchT = now;
            g_benchStatus = g_req.series ? "Benchmark wartet" : "open a replay now; recording starts by itself";
            g_keyEdge = key; // a key press in this frame still counts
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
        const std::string where = g_req.series ? g_req.series->where : g_req.mode;
        perf::StartBy by = perf::benchStartBy(where, st, key);
        if (by == perf::StartBy::None && where == "now" && g_req.series && g_req.series->startAfter >= 0 &&
            now - g_benchT >= g_req.series->startAfter * 1000.0)
            by = perf::StartBy::Key; // unattended "now" (tests): whatever is on screen
        if (by != perf::StartBy::None) {
            g_startBy = by;
            g_bench = Bench::Warmup;
            g_benchT = now;
            g_benchStatus = "warm-up";
            logf(HORIZON_LOG_INFO, "benchmark: starting (%s, game state %s)", by == perf::StartBy::Key ? "by its key" : "the scene is on screen",
                 stateName(st));
        } else if (g_req.mode == "map" && now - g_benchT > 120000) {
            benchAbort("the race did not start within 2 minutes");
        }
        return;
    }
    case Bench::Warmup: {
        if (key) {
            benchAbort("stopped by hand");
            return;
        }
        const std::string where = g_req.series ? g_req.series->where : g_req.mode;
        if (g_startBy == perf::StartBy::Scene && !perf::benchSceneOk(where, g_startBy, st)) {
            g_bench = Bench::WaitRace; // the replay was closed again before the start
            g_benchT = now;
            return;
        }
        if (now - g_benchT < g_req.warmup * 1000.0) return;
        if (g_req.series) startSeries(now);
        else benchStartRecording(now);
        return;
    }
    case Bench::Series: {
        if (key) {
            g_series.abort("stopped by hand", g_seriesIO);
            return;
        }
        perf::FrameSample f;
        const perf::FrameSample* fp = nullptr;
        if (frame) {
            f = *frame;
            fp = &f;
        }
        if (!seriesScene(now, st)) {
            if (!g_series.active()) return;
            f.skip |= perf::kSkipScene;
        }
        if (g_series.waiting()) {
            g_benchStatus = "Benchmark pausiert: Replay wieder öffnen";
            return;
        }
        g_series.tick(now, fp, g_seriesIO);
        if (g_series.active()) g_benchStatus = g_series.progress();
        return;
    }
    case Bench::Recording: {
        if (key) {
            benchAbort("stopped by hand");
            return;
        }
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
const char* benchKeyText() {
    const char* k = g_benchHotkey && g_host ? g_host->hotkey_text(g_host->plugin, g_benchHotkey) : "";
    return k ? k : "";
}
bool requestWaiting() { return g_reqFromLauncher && (g_bench == Bench::WaitMenu || g_bench == Bench::WaitRace); }
// The benchmark key: starts a waiting launcher request, stops a running one, else a manual recording.
void HORIZON_CALL onBenchHotkey(void*) {
    if (requestWaiting() || (g_reqFromLauncher && g_bench != Bench::Idle)) {
        g_keyEdge = true; // benchTick: start while waiting, stop while running
        return;
    }
    startManualBench();
}

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
        g_benchStatus = "Benchmark wartet";
        g_startBy = perf::StartBy::None;
        logf(HORIZON_LOG_INFO, "benchmark series from the launcher: %u sets, %d runs of %.0f s, %s%s%s", static_cast<unsigned>(g_req.series->sets.size()),
             g_req.runs, g_req.seconds, g_req.series->where.c_str(), g_seriesReplay.empty() ? "" : ", replay ", g_seriesReplay.c_str());
    }
    logf(HORIZON_LOG_INFO, "benchmark from the launcher: %s, %s, %.0f s x %d, warm-up %.0f s", g_req.label.c_str(), g_req.mode.c_str(),
         g_req.seconds, g_req.runs, g_req.warmup);
}

// ---- options
perf::SeriesOptions g_custom; // the player's own options (Custom), as in the settings file
int g_refreshHz = 0;          // refresh rate of the monitor the game window is on (0 unknown)
bool g_vsyncPending = false;  // the V-Sync choice changed: the device takes it at the next start or Alt+Enter

int monitorRefreshHz() {
    HMONITOR mon = MonitorFromWindow(g_window ? g_window : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return 0;
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return dm.dmDisplayFrequency > 1 ? static_cast<int>(dm.dmDisplayFrequency) : 0; // 0/1: "hardware default"
}

// The options in effect from the choice at the top: Smooth / Low latency fill them in, Custom uses the player's.
void applyPreset() {
    if (g_series.active()) return; // a benchmark series sets its own options
    g_refreshHz = monitorRefreshHz();
    const perf::SeriesOptions o = perf::presetOptions(S.preset, g_refreshHz, g_custom);
    S.count = o.count;
    S.filter = o.filter;
    S.limitFps = o.limitFps;
    S.vsync = o.vsync;
    S.priority = o.priority;
    S.affinity = o.affinity;
    S.powerThrottlingOff = o.powerThrottlingOff;
    S.fineTimer = o.fineTimer;
    S.threadCores = o.threadCores;
    S.timer1ms = o.timer1ms;
    S.mmcss = o.mmcss;
}

// ---- smoothing switches on the game thread (the thread that calls Present: messages, logic, physics, rendering)
DWORD g_gameThread = 0;       // set at the first Present
bool g_threadDirty = true;    // the thread options changed: applied at the next Present, on the game thread
int g_appliedThreadCores = 0, g_appliedMmcss = 0, g_appliedTimer1ms = 0;

// Runs on the game thread (MMCSS can only be joined and left by the thread itself).
void applyThreadOptions() {
    g_threadDirty = false;
    const int cores = g_enabled ? S.threadCores : 0;
    if (cores != g_appliedThreadCores) {
        const bool ok = perf::sys::setThreadPerfCores(g_gameThread, cores != 0);
        if (cores) logf(ok ? HORIZON_LOG_INFO : HORIZON_LOG_WARN, "smoothing: game thread %lu on performance cores: %s", static_cast<unsigned long>(g_gameThread), perf::sys::threadCoresNote());
        else logf(HORIZON_LOG_INFO, "smoothing: game thread back on all cores%s", ok ? "" : " (failed)");
        g_appliedThreadCores = cores;
    }
    const int mm = g_enabled ? S.mmcss : 0;
    if (mm != g_appliedMmcss) {
        const bool ok = perf::sys::setMmcssGames(mm != 0);
        if (mm) logf(ok ? HORIZON_LOG_INFO : HORIZON_LOG_WARN, "smoothing: MMCSS %s for the game thread%s%s", ok ? "on" : "not applied",
                     ok ? ", " : ": ", perf::sys::mmcssNote());
        else logf(HORIZON_LOG_INFO, "smoothing: MMCSS off for the game thread%s", ok ? "" : " (failed)");
        g_appliedMmcss = mm; // also after a failure: the next change of the switch tries again, not every frame
    }
}

void applyProcessOptions() {
    if (!perf::sys::setPriority(S.priority)) logf(HORIZON_LOG_WARN, "priority %d refused by Windows", S.priority);
    if (!perf::sys::setAffinity(S.affinity)) logf(HORIZON_LOG_WARN, "core selection %d refused by Windows", S.affinity);
    // "Timer 1 ms" also keeps Windows from throttling the process (and from ignoring its timer request when hidden)
    perf::sys::setPowerThrottlingOff(S.powerThrottlingOff != 0 || S.timer1ms != 0);
    perf::sys::setFineTimer(S.fineTimer != 0);
    if (S.timer1ms != g_appliedTimer1ms) {
        const bool ok = perf::sys::setTimer1ms(S.timer1ms != 0);
        if (S.timer1ms) logf(ok ? HORIZON_LOG_INFO : HORIZON_LOG_WARN, "smoothing: timer 1 ms %s, power throttling off (timer now %.2f ms)", ok ? "on" : "refused", perf::sys::currentTimerMs());
        else logf(HORIZON_LOG_INFO, "smoothing: timer 1 ms off%s", ok ? "" : " (failed)");
        g_appliedTimer1ms = S.timer1ms;
    }
    g_threadDirty = true;
    if (g_gameThread && GetCurrentThreadId() == g_gameThread) applyThreadOptions();
    g_pacer.setFps(S.limitFps);
    g_pacer.reset();
    perf::d3d::setMode(S.count && callsAllowed(), S.filter && callsAllowed());
    perf::stall::setEnabled(S.hangLog != 0);
    perf::stall::setThresholdMs(S.hangMs);
}

// ---- shader disk cache (2006 engine only; TMNF/TMUF never: Competition Patch, no hooks in the exe there)
bool legacyGame() { return (g_game & HORIZON_GAME_LEGACY) != 0; }
std::string g_scBucket = "menu and loading"; // what the current hit/miss count belongs to

void scLog(int level, const char* fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logf(level, "%s", buf);
}

// Writes the count of the bucket that ends (one line, only if a shader was asked for) and starts `next`.
void shaderCacheFlush(const char* next) {
    if (perf::shadercache::installed()) {
        const perf::shadercache::Stats s = perf::shadercache::take();
        if (s.hits || s.misses || s.fallbacks)
            logf(HORIZON_LOG_INFO, "shader cache: %s: %s", g_scBucket.c_str(), perf::shadercache::statsLine(s).c_str());
    }
    if (next) g_scBucket = next;
}

// Installs the hook the first time it is wanted (bytes checked, refused with a log line otherwise) and switches
// it between caching and pass-through.
void shaderCacheApply() {
    if (!legacyGame()) return;
    const bool want = g_enabled && S.shaderCache;
    static bool tried = false;
    if (want && !perf::shadercache::installed() && !tried) {
        tried = true;
        const HorizonGameInfo* gi = g_host->game_info();
        const perf::shadercache::Spec* spec = gi ? perf::shadercache::specFor(gi->exe_timestamp) : nullptr;
        if (!spec || g_gameKey != spec->game) {
            logf(HORIZON_LOG_WARN, "shader cache: off, unknown exe build (PE timestamp %08lx)", static_cast<unsigned long>(gi ? gi->exe_timestamp : 0));
        } else {
            perf::shadercache::Config cfg;
            cfg.folder = g_data + L"shadercache\\" + widen(spec->game) + L"\\";
            char id[64];
            snprintf(id, sizeof(id), "%s-%08lx-d3dx-static", spec->game, static_cast<unsigned long>(spec->timestamp));
            cfg.exeId = id;
            cfg.log = &scLog;
            cfg.delta = gi ? static_cast<intptr_t>(gi->exe_base) - 0x00400000 : 0;
            perf::shadercache::install(*spec, cfg);
        }
    }
    if (perf::shadercache::installed() && perf::shadercache::active() != want) {
        if (!want) shaderCacheFlush(nullptr);
        perf::shadercache::setActive(want);
        logf(HORIZON_LOG_INFO, "shader cache: %s", want ? "caching" : "pass-through (switched off)");
    }
}
const char* shaderCacheNote() {
    if (perf::shadercache::refusedWhy()[0]) return perf::shadercache::refusedWhy();
    if (S.shaderCache && legacyGame() && !perf::shadercache::installed()) return "unknown exe build";
    return "";
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
    if (S.preset != perf::PresetCustom && g_enabled) { // now the monitor of the game window is known
        const int before = S.limitFps;
        applyPreset();
        applyProcessOptions();
        if (S.limitFps != before) logf(HORIZON_LOG_INFO, "preset: frame limit %d fps (monitor %d Hz)", S.limitFps, g_refreshHz);
    }
    ensureHooks(device);
}
void HORIZON_CALL cbAdjustParams(void*, void* params) {
    if (!params || S.vsync == 0) return;
    auto* pp = static_cast<D3DPRESENT_PARAMETERS*>(params);
    pp->PresentationInterval = S.vsync == 1 ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
}
void settleFor(double ms) {
    const double until = perf::sys::nowMs() + ms;
    if (until > g_settleUntil) g_settleUntil = until;
}
void HORIZON_CALL cbBeforeReset(void*, void*) {
    perf::stall::setBusy(true); // a Reset can take seconds (Alt+Tab back): not a hang, never sample it
    perf::d3d::onReset();
    settleFor(1000);
}
void HORIZON_CALL cbAfterReset(void*, void*, int32_t) {
    perf::d3d::onReset();
    g_pacer.reset();
    settleFor(1000);
    perf::stall::setBusy(false);
}

// The game window is in front and not minimised (no window known yet: yes).
bool windowInFront() {
    if (!g_window) return true;
    if (IsIconic(g_window)) return false;
    if (g_assumeFront) return true;
    HWND f = GetForegroundWindow();
    return f && (f == g_window || GetAncestor(f, GA_ROOTOWNER) == GetAncestor(g_window, GA_ROOTOWNER));
}

std::string hookCostText(double callsPerFrame); // below, with the page's live numbers

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
    // not gameplay: background, device lost, just after a Reset / focus / state change, loading
    const bool fg = windowInFront();
    if (fg && !g_fgPrev) settleFor(1000); // back in front: the first second is the game catching up
    if (!fg || !g_fgPrev) f.skip |= perf::kSkipUnfocused;
    g_fgPrev = fg;
    if (g_presentFailed) f.skip |= perf::kSkipLost;
    if (tPre < g_settleUntil || g_tPrevPre < g_settleUntil) f.skip |= perf::kSkipSettle;
    const int gs = g_host->get_game_state();
    if (gs == HORIZON_STATE_LOADING || gs == HORIZON_STATE_UNKNOWN) f.skip |= perf::kSkipLoading;
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
        double total = 0, hookCalls = 0;
        for (size_t i = g_ring.size(); i > 0 && total < 30000; i--, n++) {
            const perf::FrameSample& x = g_ring.at(i - 1);
            total += x.frameMs;
            hookCalls += x.calls.hookCalls;
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
        char left[200] = "";
        const std::string hooks = hookCostText(n ? hookCalls / static_cast<double>(n) : -1);
        if (s.skipped)
            snprintf(left, sizeof(left), "; %u frames left out (background, loading, after a Reset)", static_cast<unsigned>(s.skipped));
        if (!hooks.empty()) {
            const size_t used = strlen(left);
            snprintf(left + used, sizeof(left) - used, "; hooks: %s", hooks.c_str());
        }
        if (withCalls) {
            const double k = 1.0 / static_cast<double>(withCalls);
            logf(HORIZON_LOG_INFO,
                 "stats: %.0f fps, 1%% low %.0f, %u stutters, game %.2f ms, Present %.2f ms; per frame: %.0f draws (%.0f software VP), "
                 "%.0f states (%.0f same), %.1f locks, GPU wait %.2f ms, filtered %.0f, limiter %.2f ms%s",
                 s.avgFps, s.low1Fps, static_cast<unsigned>(s.stutters), s.cpuMsAvg, s.presentMsAvg, sum.draws * k, sum.drawsSoftwareVP * k,
                 sum.renderStates * k, sum.renderStatesSame * k, sum.locks * k, sum.queryWaitMs * k, sum.filtered * k, s.sleepMsAvg, left);
        } else {
            logf(HORIZON_LOG_INFO, "stats: %.0f fps, 1%% low %.0f, %u stutters, game %.2f ms, Present %.2f ms, limiter %.2f ms%s", s.avgFps,
                 s.low1Fps, static_cast<unsigned>(s.stutters), s.cpuMsAvg, s.presentMsAvg, s.sleepMsAvg, left);
        }
    }
}

int g_gsPending = -1; // Cpu/Gpu synchro picked on the page, applied in the next pre_present (game thread, between frames)
void applyGpuSync(int value);
void logGpuSync();

void HORIZON_CALL cbPrePresent(void*, void* device) {
    const double t = perf::sys::nowMs();
    if (!g_gameThread) g_gameThread = GetCurrentThreadId();
    if (g_threadDirty && GetCurrentThreadId() == g_gameThread) applyThreadOptions();
    if (g_gsPending >= 0 && GetCurrentThreadId() == g_gameThread) {
        const int v = g_gsPending;
        g_gsPending = -1;
        applyGpuSync(v);
    }
    if (g_frameNo == 300) logGpuSync(); // once, when the game's viewport is up
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

void HORIZON_CALL cbPostPresent(void*, void*, int32_t hr) {
    const double t = perf::sys::nowMs();
    g_presentFailed = hr < 0;
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

// ---- Cpu/Gpu synchro (gpusync.h, gpusync_live.h): the game's wait for the GPU after each frame
void* pointerAt(void* base, int32_t off) {
    if (!base || off < 0) return nullptr;
    __try {
        return *reinterpret_cast<void**>(static_cast<unsigned char*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
void* gameViewport() {
    if (!g_host->reflect_offset) return nullptr;
    return pointerAt(g_host->engine_pointer(HORIZON_ENGINE_APP), g_host->reflect_offset("CGameApp", "Viewport"));
}
// CGameApp.Viewport -> CHmsViewport.Config -> CSystemConfig.Display (CSystemConfigDisplay); *off: its GpuSync member
void* gameDisplayConfig(int32_t* off) {
    *off = g_host->reflect_offset ? g_host->reflect_offset("CSystemConfigDisplay", "GpuSync") : -1;
    if (*off < 0) return nullptr;
    void* config = pointerAt(gameViewport(), g_host->reflect_offset("CHmsViewport", "Config"));
    return pointerAt(config, g_host->reflect_offset("CSystemConfig", "Display"));
}
std::wstring systemConfigFile() {
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring dir(exe);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"Default.SystemConfig.Gbx";
}
int g_gsFile = -1;      // ESWC/Sunrise/Original: the value in Default.SystemConfig.Gbx (-1 unknown)
std::string g_gsNote;    // the last change's result, one short line on the page
bool foreverGame() { return (g_game & (HORIZON_GAME_TMNF | HORIZON_GAME_TMUF)) != 0; }
// with Competition Patch loaded Horizon touches nothing beyond its overlay on TMNF/TMUF (owner's rule)
bool gsLiveAllowed() { return !(foreverGame() && g_competitionPatch); }

void readSystemConfigFile() {
    g_gsFile = -1;
    if (foreverGame()) return;
    FILE* f = _wfopen(systemConfigFile().c_str(), L"rb");
    if (!f) return;
    std::string in;
    char buf[4096];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0 && in.size() < (1u << 20)) in.append(buf, got);
    fclose(f);
    g_gsFile = perf::gpusync::readFromConfig(in);
}

void logGpuSync() {
    const perf::gpusync::Live l = perf::gpusync::readLive(gameViewport(), g_game);
    if (l.verified)
        logf(HORIZON_LOG_INFO, "Cpu/Gpu synchro: %s (live switch %s)%s%s", perf::gpusync::name(l.value),
             gsLiveAllowed() ? "available" : "off: Competition Patch", g_gsFile >= 0 ? ", file: " : "", g_gsFile >= 0 ? perf::gpusync::name(g_gsFile) : "");
    else
        logf(HORIZON_LOG_INFO, "Cpu/Gpu synchro: no live switch (%s)%s%s", l.why.c_str(), g_gsFile >= 0 ? ", file: " : "",
             g_gsFile >= 0 ? perf::gpusync::name(g_gsFile) : "");
}

void applyGpuSync(int value) {
    const perf::gpusync::Live before = perf::gpusync::readLive(gameViewport(), g_game);
    const char* was = before.verified ? perf::gpusync::name(before.value) : "?";
    std::string why;
    bool live = false;
    if (gsLiveAllowed()) live = perf::gpusync::setLive(gameViewport(), g_game, value, &why);
    else why = "Competition Patch loaded";
    if (live) logf(HORIZON_LOG_INFO, "Cpu/Gpu synchro: %s -> %s (live)", was, perf::gpusync::name(value));
    else logf(HORIZON_LOG_WARN, "Cpu/Gpu synchro: not switched live (%s)", why.c_str());
    if (foreverGame()) {
        // the binary, compressed Documents config is never written; the launcher keeps the setting
        g_gsNote = live ? "This session only." : "Not changed: " + why + ".";
        return;
    }
    // the engine's own config value too, so a later ApplySystemConfig or config save keeps the choice
    int32_t off = -1;
    void* display = gameDisplayConfig(&off);
    const int oldDisplay = perf::gpusync::readDisplayValue(display, off);
    // only when that field holds what the file or the viewport says (else it is not the field I think it is)
    const bool plausible = oldDisplay >= 0 && (oldDisplay == g_gsFile || (before.verified && oldDisplay == before.value));
    if (plausible && perf::gpusync::setDisplayValue(display, off, value))
        logf(HORIZON_LOG_INFO, "Cpu/Gpu synchro: the game's display config %s -> %s", perf::gpusync::name(oldDisplay), perf::gpusync::name(value));
    else
        logf(HORIZON_LOG_INFO, "Cpu/Gpu synchro: the game's display config left as it is (%s)", oldDisplay < 0 ? "not found" : "does not match");
    int old = -1;
    std::string msg;
    const bool saved = perf::gpusync::persist(systemConfigFile(), value, &old, &msg);
    logf(saved ? HORIZON_LOG_INFO : HORIZON_LOG_WARN, "Cpu/Gpu synchro: Default.SystemConfig.Gbx %s -> %s: %s", perf::gpusync::name(old),
         perf::gpusync::name(value), msg.c_str());
    readSystemConfigFile();
    if (saved) g_gsNote = live ? "Now and at the next start." : "At the next start.";
    else g_gsNote = std::string(live ? "Now only. " : "Not changed. ") + "File: " + msg + ".";
}

// ---- drawing
ImVec4 col(const HorizonColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }

void refreshLive(double now) {
    if (now - g_liveAt < 500 && g_live.frames) return;
    g_liveAt = now;
    // the last ~10 s
    size_t n = 0;
    double sum = 0;
    double hooks = 0;
    for (size_t i = g_ring.size(); i > 0 && sum < 10000; i--, n++) {
        sum += g_ring.at(i - 1).frameMs;
        hooks += g_ring.at(i - 1).calls.hookCalls;
    }
    g_live = perf::summarize(g_ring.last(n));
    g_liveHookCalls = perf::d3d::attached() && n ? hooks / static_cast<double>(n) : -1;
}

// What my Direct3D hooks cost a frame: "4210 hooked calls, about 0.08 ms" ("" without hooks).
std::string hookCostText(double callsPerFrame) {
    if (callsPerFrame < 0 || !perf::d3d::attached()) return "";
    char b[96];
    const double us = perf::d3d::perCallUs();
    if (us >= 0) snprintf(b, sizeof(b), "%.0f hooked calls, about %.2f ms", callsPerFrame, callsPerFrame * us / 1000.0);
    else snprintf(b, sizeof(b), "%.0f hooked calls", callsPerFrame);
    return b;
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
    std::string l1, l2;
    if (g_reqFromLauncher && bannerLines(l1, l2)) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", l1.c_str());
        if (!l2.empty()) ImGui::TextDisabled("%s", l2.c_str());
    } else if (g_bench != Bench::Idle) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Benchmark: %s", g_benchStatus.c_str());
    }
}

// The benchmark's line at the top of the screen: always visible while a launcher request waits or runs (the
// frame-time window may be off or docked in the menu), and 20 s after the end.
bool bannerLines(std::string& l1, std::string& l2) {
    const double now = perf::sys::nowMs();
    const std::string key = benchKeyText();
    char b[300];
    if (g_bench == Bench::Idle || !g_reqFromLauncher) {
        if (g_bench == Bench::Idle && !g_doneText.empty() && now - g_doneAt < 20000) {
            l1 = g_doneText;
            return true;
        }
        return false;
    }
    const std::string where = g_req.series ? g_req.series->where : g_req.mode;
    const std::string stopKey = key.empty() ? "" : key + " bricht ab";
    switch (g_bench) {
    case Bench::WaitMenu:
    case Bench::WaitRace:
        if (where == "replay") {
            const int minS = static_cast<int>(2 * g_req.seconds + 0.5);
            if (g_seriesReplay.empty()) snprintf(b, sizeof(b), "Warte auf Replay: \xC3\xB6" "ffne ein Replay (mind. %d s)", minS);
            else snprintf(b, sizeof(b), "Warte auf Replay: \xC3\xB6" "ffne %s (mind. %d s)", fileNameOf(g_seriesReplay).c_str(), minS);
            l1 = b;
            l2 = key.empty() ? "Startet von selbst. Sonst im Horizon-Men\xC3\xBC: Performance > Start series"
                             : "Startet von selbst. Sonst " + key + " dr\xC3\xBC" "cken, sobald das Replay l\xC3\xA4" "uft";
        } else if (where == "now") {
            l1 = key.empty() ? "Benchmark bereit: Szene \xC3\xB6" "ffnen, dann im Horizon-Men\xC3\xBC Performance > Start series"
                             : "Benchmark bereit: Szene \xC3\xB6" "ffnen, dann " + key + " dr\xC3\xBC" "cken";
        } else {
            l1 = g_bench == Bench::WaitMenu ? "Benchmark: warte auf das Hauptmen\xC3\xBC" : "Benchmark: Strecke l\xC3\xA4" "dt";
        }
        return true;
    case Bench::Warmup:
        snprintf(b, sizeof(b), "Benchmark startet in %.0f s", g_req.warmup - (now - g_benchT) / 1000.0 > 0 ? g_req.warmup - (now - g_benchT) / 1000.0 : 0);
        l1 = b;
        l2 = stopKey;
        return true;
    case Bench::Series: {
        const int left = g_series.windowsLeft();
        const double perWindow = g_req.series->seconds + g_req.series->settle;
        snprintf(b, sizeof(b), "%s  (noch etwa %.0f min)", g_series.waiting() ? g_benchStatus.c_str() : g_series.progress().c_str(),
                 left * perWindow / 60.0 + 0.49);
        l1 = b;
        if (g_series.waiting()) l2 = "";
        else if (g_sceneLostAt > 0) l2 = where == "map" ? "Rennen ist weg" : "Pausiert: Replay wieder \xC3\xB6" "ffnen";
        else if (!g_fgPrev) l2 = "Pausiert: Spielfenster ist im Hintergrund";
        else l2 = stopKey;
        return true;
    }
    case Bench::Recording:
        snprintf(b, sizeof(b), "Benchmark l\xC3\xA4" "uft: %s, Lauf %d/%d", g_req.label.c_str(), g_run, g_req.runs);
        l1 = b;
        l2 = stopKey;
        return true;
    default: return false;
    }
}

void drawBanner() {
    std::string l1, l2;
    if (!g_imgui || !bannerLines(l1, l2)) return;
    const HorizonStyle* st = g_host->style();
    const float sc = st && st->scale > 0 ? st->scale : 1.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const ImVec2 s1 = ImGui::CalcTextSize(l1.c_str());
    const ImVec2 s2 = l2.empty() ? ImVec2(0, 0) : ImGui::CalcTextSize(l2.c_str());
    const float pad = 8 * sc, gap = l2.empty() ? 0 : 2 * sc;
    const float w = (s1.x > s2.x ? s1.x : s2.x) + 2 * pad, h = s1.y + gap + s2.y + 2 * pad;
    const ImVec2 p0((screen.x - w) * 0.5f, 40 * sc);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(16, 18, 24, 225), st ? st->radius : 6 * sc);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(255, 204, 77, 200), st ? st->radius : 6 * sc, 0, 1.5f * sc);
    dl->AddText(ImVec2(p0.x + (w - s1.x) * 0.5f, p0.y + pad), IM_COL32(255, 214, 102, 255), l1.c_str());
    if (!l2.empty()) dl->AddText(ImVec2(p0.x + (w - s2.x) * 0.5f, p0.y + pad + s1.y + gap), IM_COL32(220, 224, 232, 255), l2.c_str());
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
// A muted line that wraps at the window edge (pop-outs, small windows).
void dimWrapped(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", b);
    ImGui::PopStyleColor();
}

void exportLast60() {
    size_t n = 0;
    double sum = 0;
    for (size_t i = g_ring.size(); i > 0 && sum < 60000; i--, n++) sum += g_ring.at(i - 1).frameMs;
    const std::wstring dir = g_data + L"bench\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + widen(stampNow() + "_last60s.csv");
    const bool ok = writeFile(path, perf::framesCsv(g_ring.last(n)));
    g_lastResult = ok ? "Saved (CSV)" : "CSV not written";
}

// Cpu/Gpu synchro: the game waits for the GPU after each frame (Immediate by default in all five games). The choice
// shows what the game runs with now; ESWC/Sunrise/Original also keep it for the next start (their text config file),
// TMNF/TMUF only for this session (their config is a compressed binary file: the launcher sets it).
void drawGpuSync(float fieldW) {
    using namespace perf::gpusync;
    static const int kChoices[3] = {kImmediate, kHalfFrame, kOneFrame};
    const Live live = readLive(gameViewport(), g_game);
    const bool forever = foreverGame();
    const bool canLive = live.verified && gsLiveAllowed();
    const bool canPick = forever ? canLive : true; // ESWC/Sunrise/Original: the file at least
    const int shown = live.verified && live.value >= 0 ? live.value : (!forever ? g_gsFile : -1);
    ImGui::BeginDisabled(!canPick || g_gsPending >= 0);
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::BeginCombo("Cpu/Gpu synchro", shown >= 0 ? name(shown) : "Unknown")) {
        for (int v : kChoices)
            if (ImGui::Selectable(name(v), v == shown) && v != shown) g_gsPending = v;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    hint("More FPS, about one frame more input delay.");
    if (forever) {
        if (!gsLiveAllowed()) dimWrapped("Competition Patch: not changed here. Set it in TM Horizon or TmForeverLauncher > Advanced.");
        else if (!live.verified) dimWrapped("Not on this exe (%s). Set it in TM Horizon or TmForeverLauncher > Advanced.", live.why.c_str());
        else dimWrapped("This session only. To keep it: TM Horizon or TmForeverLauncher > Advanced.");
    } else {
        if (!live.verified) dimWrapped("Next start only (%s).", live.why.c_str());
        else if (g_gsFile >= 0 && g_gsFile != live.value) dimWrapped("Next start: %s", name(g_gsFile));
    }
    if (!g_gsNote.empty()) dimWrapped("%s", g_gsNote.c_str());
}

// The options one by one (Custom). Every change is also the new Custom set.
void drawCustomOptions(float fieldW) {
    const perf::SeriesOptions before = currentOptions();
    ImGui::SeparatorText("Options");
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::InputInt("FPS limit", &S.limitFps, 10, 30)) {
        if (S.limitFps < 0) S.limitFps = 0;
        if (S.limitFps > 0 && S.limitFps < 30) S.limitFps = 30;
        if (S.limitFps > 1000) S.limitFps = 1000;
        saveInt("limit_fps", S.limitFps);
        applyProcessOptions();
    }
    hint("0 = off. Waits after Present for steadier frames.");
    static const char* kVsync[] = {"Default", "Off", "On"};
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::Combo("V-Sync", &S.vsync, kVsync, 3)) saveInt("vsync", S.vsync);
    hint("Applies at the next start or Alt+Enter.");
    ImGui::BeginDisabled(!callsAllowed() || perf::d3d::filterBlockedWhy()[0] != 0);
    if (checkbox("Skip repeated states", &S.filter, "filter")) applyProcessOptions();
    ImGui::EndDisabled();
    hint("Experimental: drops state calls that repeat the value.");
    if (perf::d3d::filterBlockedWhy()[0]) dimWrapped("Off: %s", perf::d3d::filterBlockedWhy());
    if (checkbox("Fine timer (0.5 ms)", &S.fineTimer, "fine_timer")) applyProcessOptions();
    hint("0.5 ms instead of the game's 1 ms.");
    ImGui::SameLine();
    ImGui::TextDisabled("Now %.2f ms", perf::sys::currentTimerMs());
    if (checkbox("No power throttling", &S.powerThrottlingOff, "power_throttling_off")) applyProcessOptions();
    hint("No EcoQoS slowdown in the background.");
    static const char* kPrio[] = {"Default", "Above normal", "High"};
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::Combo("Priority", &S.priority, kPrio, 3)) {
        saveInt("priority", S.priority);
        applyProcessOptions();
    }
    static const char* kAff[] = {"All", "All but CPU 0", "P-cores"};
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::Combo("CPU cores", &S.affinity, kAff, 3)) {
        saveInt("affinity", S.affinity);
        applyProcessOptions();
    }
    if (S.affinity) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", perf::sys::affinityNote());
    }
    if (currentOptions() != before) g_custom = currentOptions();
}

void drawMenu() {
    const double now = perf::sys::nowMs();
    refreshLive(now);
    const HorizonStyle* st = g_host->style();
    const float sc = st && st->scale > 0 ? st->scale : 1.0f;
    const float fieldW = 160.0f * sc; // input fields, scaled like the menu
    if (g_competitionPatch) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Competition Patch: call counts off");

    // the choice: Smooth / Low latency set the options, Custom shows them
    {
        static const char* kPreset[] = {"Smooth", "Low latency", "Custom"};
        static const char* kTip[] = {"Frame limit at the refresh rate, V-Sync as the game has it.",
                                     "Frame limit at the refresh rate, V-Sync off.", "Your own options."};
        int pick = S.preset;
        for (int i = 0; i < 3; i++) {
            if (i) ImGui::SameLine();
            ImGui::RadioButton(kPreset[i], &pick, i);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kTip[i]);
        }
        if (pick != S.preset) {
            const int vsyncBefore = S.vsync;
            S.preset = pick;
            saveInt("preset", pick);
            applyPreset();
            applyProcessOptions();
            if (S.vsync != vsyncBefore) g_vsyncPending = true;
            logf(HORIZON_LOG_INFO, "preset %s: limit %d fps, V-Sync %s, priority %d", kPreset[pick], S.limitFps,
                 S.vsync == 0 ? "as the game" : S.vsync == 1 ? "off" : "on", S.priority);
        }
        if (S.preset != perf::PresetCustom) {
            if (S.limitFps) dimWrapped("Limit %d fps (%d Hz)   V-Sync %s   Above normal", S.limitFps, g_refreshHz, S.vsync == 1 ? "off" : "game");
            else dimWrapped("No limit (refresh rate unknown)   V-Sync %s   Above normal", S.vsync == 1 ? "off" : "game");
        }
        if (g_vsyncPending) dimWrapped("V-Sync: next start or Alt+Enter");
        // the cap in effect now, and whether it takes anything away (it only waits when the game is faster)
        if (S.limitFps > 0 && g_live.frames) {
            if (g_live.sleepMsAvg >= 0.05) dimWrapped("Cap %d fps: waits %.2f ms a frame", S.limitFps, g_live.sleepMsAvg);
            else dimWrapped("Cap %d fps: game below it, no wait", S.limitFps);
        }
    }

    // smoothing switches: independent of the choice above, all off by default (docs/performance-smooth.md)
    ImGui::SeparatorText("Smoothing");
    if (checkbox("Pin thread", &S.threadCores, "thread_cores")) {
        g_custom.threadCores = S.threadCores;
        applyProcessOptions();
    }
    hint("Keeps the game thread on performance cores. Hybrid CPUs only.");
    if (S.threadCores && perf::sys::threadCoresNote()[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", perf::sys::threadCoresNote());
    }
    if (checkbox("Timer 1 ms", &S.timer1ms, "timer_1ms")) {
        g_custom.timer1ms = S.timer1ms;
        applyProcessOptions();
    }
    hint("1 ms timer, no power throttling. ESWC and Original never set it.");
    ImGui::SameLine();
    ImGui::TextDisabled("Now %.2f ms", perf::sys::currentTimerMs());
    if (checkbox("MMCSS", &S.mmcss, "mmcss")) {
        g_custom.mmcss = S.mmcss;
        applyProcessOptions();
    }
    hint("Windows schedules the game thread as a game.");
    if (S.mmcss && perf::sys::mmcssNote()[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", perf::sys::mmcssNote());
    }
    if (legacyGame()) {
        if (checkbox("Shader cache", &S.shaderCache, "shader_cache")) shaderCacheApply();
        hint("Keeps compiled shaders on disk. No compile stutter when a map runs again.");
        if (shaderCacheNote()[0]) {
            dimWrapped("Off: %s", shaderCacheNote());
        } else if (S.shaderCache && perf::shadercache::active()) {
            const perf::shadercache::Stats cs = perf::shadercache::peek();
            dimWrapped("Now: %u from cache, %u compiled", cs.hits, cs.misses);
        }
    }
    drawGpuSync(fieldW);

    ImGui::SeparatorText("Measure");
    ImGui::TextWrapped("10 s: %.0f fps   1%% %.0f   0.1%% %.0f   Stutters %u", g_live.avgFps, g_live.low1Fps, g_live.low01Fps,
                       static_cast<unsigned>(g_live.stutters));
    dimWrapped("Game %.2f   Present %.2f   Wait %.2f ms", g_live.cpuMsAvg, g_live.presentMsAvg, g_live.sleepMsAvg);
    {
        const std::string hooks = hookCostText(g_liveHookCalls);
        if (!hooks.empty()) dimWrapped("Hooks: %s", hooks.c_str());
    }
    if (checkbox("Frame-time window", &S.overlay, "overlay")) registerOverlay();
    hint("FPS, lows and a frame graph on screen.");
    ImGui::BeginDisabled(!callsAllowed());
    if (checkbox("Count Direct3D calls", &S.count, "count")) {
        g_custom.count = S.count;
        applyProcessOptions();
    }
    ImGui::EndDisabled();
    hint("Calls per frame; costs a little time.");
    if (S.count) {
        ImGui::SameLine();
        checkbox("In window", &S.overlayCalls, "overlay_calls");
    }
    if (perf::d3d::attached()) {
        const auto& f = perf::d3d::facts();
        dimWrapped("Device 0x%08lx  %s%s%s", static_cast<unsigned long>(f.behaviorFlags), f.pure ? "pure, " : "",
                   f.mixed ? "mixed VP" : f.software ? "software VP" : "hardware VP", f.multithreaded ? ", multithreaded" : "");
        if (g_ring.size() && g_ring.newest().hasCalls && g_ring.newest().calls.drawsSoftwareVP)
            dimWrapped("Software-VP draws: %u per frame", g_ring.newest().calls.drawsSoftwareVP);
    }
    if (ImGui::Button("Save last 60 s")) exportLast60();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("CSV in the bench folder.");

    ImGui::SeparatorText("Benchmark");
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::SliderInt("Seconds", &S.benchSeconds, 10, 120)) saveInt("bench_seconds", S.benchSeconds);
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::InputText("Label", S.benchLabel, sizeof(S.benchLabel))) g_host->set_string(g_host->plugin, "bench_label", S.benchLabel);
    if (requestWaiting()) {
        if (ImGui::Button("Start series")) g_keyEdge = true;
        ImGui::SameLine();
        ImGui::TextDisabled("Request waiting");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open the scene first.");
    } else if (g_reqFromLauncher && g_bench != Bench::Idle) {
        if (ImGui::Button("Stop")) g_keyEdge = true;
    } else if (ImGui::Button(g_bench == Bench::Idle ? "Start" : "Stop")) {
        startManualBench();
    }
    const char* hk = g_benchHotkey ? g_host->hotkey_text(g_host->plugin, g_benchHotkey) : "";
    if (hk && *hk) {
        ImGui::SameLine();
        ImGui::TextDisabled("Key: %s", hk);
    }
    if (!g_benchStatus.empty()) ImGui::TextWrapped("%s", g_benchStatus.c_str());
    if (!g_lastResult.empty()) dimWrapped("%s", g_lastResult.c_str());

    if (S.preset == perf::PresetCustom) drawCustomOptions(fieldW);

    ImGui::SeparatorText("Hang log");
    if (checkbox("Log stalls", &S.hangLog, "hang_log")) applyProcessOptions();
    hint("Writes hangs.log on long stalls, never in a race.");
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::SliderInt("After (ms)", &S.hangMs, 500, 5000)) {
        saveInt("hang_ms", S.hangMs);
        applyProcessOptions();
    }
    const perf::stall::Last ls = perf::stall::last();
    if (ls.count) dimWrapped("Stalls: %d, last %.1f s in %s", ls.count, ls.seconds, ls.top[0] ? ls.top : "-");

    ImGui::SeparatorText("Engine and memory");
    const EngineView ev = readEngine();
    if (ev.ok) {
        dimWrapped("Engine FPS %.0f   Adaptive view %s (%u fps)", ev.averageFps, ev.targetFpsEnable < 0 ? "?" : ev.targetFpsEnable ? "on" : "off",
                   ev.targetFps);
    } else {
        dimWrapped("Engine: no data");
    }
    const perf::sys::Memory m = perf::sys::memory();
    dimWrapped("Memory %.0f MB   Address space %.0f / %.0f MB", m.privateMB, m.addressSpaceUsedMB, m.addressSpaceTotalMB);
    ImGui::TextDisabled("Large address aware: %s", m.largeAddressAware ? "yes" : "no");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", m.largeAddressAware ? "The game can use up to 4 GB of memory." : "The game can use only 2 GB of memory.");
    char folder[300];
    WideCharToMultiByte(CP_UTF8, 0, g_data.c_str(), -1, folder, sizeof(folder), nullptr, nullptr);
    dimWrapped("Files: %s", folder);
}

const HorizonPluginInfo kInfo = {
    HORIZON_ABI,
    "performance",
    "Horizon Performance",
    "0.1.0",
    HORIZON_GAME_ESWC | HORIZON_GAME_SUNRISE | HORIZON_GAME_ORIGINAL | HORIZON_GAME_TMNF | HORIZON_GAME_TMUF,
    HORIZON_CAP_MENU | HORIZON_CAP_OVERLAY | HORIZON_CAP_DEVICE | HORIZON_CAP_GAME_EVENTS | HORIZON_CAP_GAME_CONTROL,
    HORIZON_FLAG_ONLINE_OK | HORIZON_FLAG_LIVE_TOGGLE,
    "cheatoskar",
    "Frame times, benchmark and frame limiter.",
    HORIZON_ABI_MINOR,
};

} // namespace

extern "C" {

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) { return &kInfo; }

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    g_host = host;
    const HorizonGameInfo* gi = host->game_info();
    g_game = gi ? gi->game : 0;
    g_gameKey = g_game == HORIZON_GAME_ESWC ? "eswc" : g_game == HORIZON_GAME_SUNRISE ? "sunrise" : g_game == HORIZON_GAME_ORIGINAL ? "original" : g_game == HORIZON_GAME_TMNF ? "tmnf"
              : g_game == HORIZON_GAME_TMUF ? "tmuf" : "other";
    g_imgui = horizon_imgui_attach(host);
    if (!g_imgui) logf(HORIZON_LOG_WARN, "Dear ImGui could not attach: no page, measuring still works");
    g_data = host->data_folder_w(host->plugin);
    g_competitionPatch = GetModuleHandleW(L"TMCompPatch.dll") != nullptr;
    g_hasPostPresent = host->abi_minor >= 3;
    {
        wchar_t v[4] = {};
        g_assumeFront = GetEnvironmentVariableW(L"HORIZON_TEST_FOREGROUND", v, 4) && v[0] == L'1';
        perf::stall::setAssumeFront(g_assumeFront);
    }
    loadSettings();
    readSystemConfigFile();
    g_custom = currentOptions();
    applyPreset();
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
    shaderCacheApply();

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
    g_benchHotkey = host->register_hotkey(host->plugin, "bench", "Benchmark start/stop", "Ctrl+Shift+B", &onBenchHotkey, nullptr);
    readLauncherRequest();
    registerOverlay();
    logf(HORIZON_LOG_INFO, "started: post_present %s, timer %.2f ms, %s", g_hasPostPresent ? "yes" : "no (older host: limiter before Present)",
         perf::sys::currentTimerMs(), g_competitionPatch ? "Competition Patch: call counter and filter off" : "all options available");
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    perf::stall::note("shutdown");
    shaderCacheFlush(nullptr);
    perf::shadercache::uninstall();
    if (g_series.active()) g_series.abort("the game was closed", g_seriesIO);
    perf::stall::stop();
    perf::d3d::detach();
    perf::sys::setFineTimer(false);
    perf::sys::setTimer1ms(false);
    if (g_appliedThreadCores) perf::sys::setThreadPerfCores(g_gameThread, false);
    if (g_appliedMmcss) perf::sys::setMmcssGames(false); // only works on the game thread; else it ends with the thread
    g_host = nullptr;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawMenu(void) {
    if (g_imgui) drawMenu();
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawOverlay(void) {
    if (g_enabled) drawBanner();
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_OnEvent(const HorizonEvent* e) {
    if (!e || e->size < offsetof(HorizonEvent, game_state) + sizeof(uint32_t)) return;
    const double t = perf::sys::secondsSinceProcessStart();
    switch (e->type) {
    case HORIZON_EVENT_GAME_STATE: {
        const int s = static_cast<int>(e->game_state);
        perf::stall::setState(stateName(s), s == HORIZON_STATE_SOLO_RACE || s == HORIZON_STATE_NET_RACE);
        perf::stall::note("state: %s", stateName(s));
        settleFor(1000);
        logf(HORIZON_LOG_INFO, "timeline: %s at %.1f s after the game started", stateName(s), t);
        break;
    }
    case HORIZON_EVENT_MAP_LOADED:
        perf::stall::note("map loaded: %s", e->map && e->map->name ? e->map->name : "?");
        settleFor(1000);
        shaderCacheFlush(e->map && e->map->name && e->map->name[0] ? e->map->name : "map");
        break;
    case HORIZON_EVENT_MENU_ENTERED:
        shaderCacheFlush("menu and loading");
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
        if (g_appliedTimer1ms) {
            perf::sys::setTimer1ms(false);
            g_appliedTimer1ms = 0;
            logf(HORIZON_LOG_INFO, "smoothing: timer 1 ms off (module off)");
        }
        if (g_gameThread && GetCurrentThreadId() == g_gameThread) {
            applyThreadOptions(); // g_enabled is false: thread options off
        } else if (g_appliedThreadCores || g_appliedMmcss) {
            if (g_appliedThreadCores && perf::sys::setThreadPerfCores(g_gameThread, false)) g_appliedThreadCores = 0;
            if (g_appliedMmcss) logf(HORIZON_LOG_WARN, "smoothing: MMCSS stays on the game thread until the module is on again or the game ends");
        }
        perf::stall::setEnabled(false);
        g_pacer.reset();
    } else {
        applyPreset();
        applyProcessOptions();
    }
    shaderCacheApply();
}

} // extern "C"
