// WR Hunter: a LiveSplit-style split widget for TrackMania. At each checkpoint: the split time against the
// personal best and against a chosen target (TMX record, the race's best ghost, a replay file), coloured
// like LiveSplit (ahead/behind, gaining/losing, gold sector), the sum of best sectors, attempts and resets.
// Per map (by UID) the PB, the gold sectors and the last 100 finished runs are kept in the plugin's data
// folder (horizon\data\wr-hunter\maps\<uid>.json).
//
// Where the times come from:
//   ESWC, Sunrise, Original: the host's CHECKPOINT / FINISH events; a burst of CHECKPOINT events (Original sends
//               ~50 per checkpoint) counts once, unknown times come from the run's own clock (wrh::CpEvents).
//   TMNF, TMUF: read-only polling of the local player's checkpoint list (client-common/gameread), by
//               reflection member names; no hook, no write, no game call (Competition Patch 2.0 rule).
// TMX rules: split times are allowed online; nothing shows where a checkpoint is, nothing draws a path.
#define HORIZON_PLUGIN_BUILD
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "horizon_imgui.h"
#include "horizon_plugin.h"

#include "ckui.h"
#include "common/json.h"
#include "common/util.h"
#include "gameread.h"
#include "mapident.h"
#include "replayfile.h"
#include "src/splits.h"

namespace {

using ck::fmtDelta;
using ck::fmtTime;
using wrh::kNoDelta;

const HorizonHostApi* g_host = nullptr;
bool g_ui = false;
bool g_enabled = true;
unsigned g_generation = 1; // bridge answers of an older generation (before a map change / Shutdown) are ignored
ck::LiveMem g_mem;
std::unique_ptr<ck::Reader> g_reader;
bool g_forever = false;
ck::IdTable g_ids;
bool g_idsTried = false;
int g_uidOffset = -1;

// the map and its stats
bool g_haveMap = false;
std::string g_mapName;
int g_authorTime = -1;
bool g_uidKnown = false;
wrh::MapStats g_stats;
bool g_dirty = false;

// the live run
std::vector<int> g_live;
int g_runState = 0; // 0 none, 1 running, 2 finished
int g_nowMs = -1;
int g_respawns = -1;
wrh::CpEvents g_cpEvents;    // ESWC/Sunrise/Original: the host's CHECKPOINT events -> real checkpoints
uint64_t g_testClock = 0;    // test hook "clock <ms>": a fixed wall clock instead of GetTickCount64
wrh::FinishResult g_lastFinish;
uint64_t g_flashUntil = 0;
// the detailed table: at most this many split lines, then it scrolls
const size_t kRowsShown = 8;
int g_rowsShown = 0;         // lines in the last drawn table (test hook)
size_t g_rowsFollowed = 0;

// the target
enum Target { kTargetPb = 0, kTargetTmx = 1, kTargetGhost = 2, kTargetFile = 3 };
wrh::Comparison g_target;
std::string g_targetStatus;
int g_ghostTimer = 0;
bool g_tmxAsked = false;

// replay picker (worker thread)
struct ReplayEntry {
    std::wstring path;
    std::string driver, file;
    int time = -1;
};
std::mutex g_pickMutex;
std::vector<ReplayEntry> g_picked;
std::atomic<bool> g_scanning{false};
std::thread g_scanThread;
std::string g_scanUid;

// all maps (History page)
struct MapRow {
    std::string name, key;
    int pb = -1, attempts = 0, finishes = 0;
};
std::vector<MapRow> g_allMaps;
bool g_allMapsLoaded = false;

uint64_t clockMs() { return g_testClock ? g_testClock : GetTickCount64(); }

int setting(const char* k, int fallback) { return g_host->get_int(g_host->plugin, k, fallback); }
void setSetting(const char* k, int v) { g_host->set_int(g_host->plugin, k, v); }

std::wstring dataFolder() { return g_host->data_folder_w(g_host->plugin); }
std::wstring mapsFolder() { return dataFolder() + L"maps\\"; }
std::wstring statsPath() { return mapsFolder() + horizon::wide(wrh::fileKey(g_stats.key)) + L".json"; }

void save() {
    if (!g_haveMap || g_stats.key.empty() || !g_dirty) return;
    horizon::ensureDir(mapsFolder());
    if (horizon::writeFileAtomic(statsPath(), wrh::toJson(g_stats))) g_dirty = false;
    else g_host->log(g_host->plugin, HORIZON_LOG_WARN, "could not write %s", horizon::utf8(statsPath()).c_str());
    g_allMapsLoaded = false;
}

// ---------------------------------------------------------------- the map's identity
std::string readMapUid(const HorizonEvent* e) {
    if (e && e->map && e->map->uid && *e->map->uid) return e->map->uid;
    const uintptr_t challenge = reinterpret_cast<uintptr_t>(e && e->engine_challenge ? e->engine_challenge : g_host->engine_pointer(HORIZON_ENGINE_CHALLENGE));
    if (!challenge) return std::string();
    if (!g_idsTried) {
        g_idsTried = true;
        const HorizonGameInfo* gi = g_host->game_info();
        const size_t size = gi ? ck::imageSize(g_mem, gi->exe_base) : 0;
        if (size && ck::findIdTable(g_mem, gi->exe_base, size, &g_ids))
            g_host->log(g_host->plugin, HORIZON_LOG_INFO, "map UIDs: id table found (stride %d)", g_ids.stride);
        else
            g_host->log(g_host->plugin, HORIZON_LOG_INFO, "map UIDs: id table not found; maps are kept by name");
        if (g_forever) g_uidOffset = 0xdc;
    }
    return ck::challengeUid(g_mem, g_ids, challenge, &g_uidOffset);
}

// ---------------------------------------------------------------- the target
void setTargetFromReplay(const ck::Replay& r, const std::string& name) {
    g_target = wrh::Comparison();
    g_target.name = name;
    g_target.time = r.raceTime;
    g_target.splits = r.checkpoints;
}

void HORIZON_CALL onReplayBytes(void* user, int32_t status, const char*, const void* body, uint32_t len) {
    if (reinterpret_cast<uintptr_t>(user) != g_generation || !g_host) return;
    if (status != 200 || !body || !len) {
        g_targetStatus = status == HORIZON_BRIDGE_ERR_RATE_LIMIT ? "Too many requests, try later." : "Record replay not available.";
        return;
    }
    std::vector<unsigned char> bytes(static_cast<const unsigned char*>(body), static_cast<const unsigned char*>(body) + len);
    ck::Replay r;
    std::string err;
    if (!ck::parseReplay(bytes, true, r, err)) {
        g_targetStatus = "Record replay unreadable (" + err + ").";
        return;
    }
    horizon::ensureDir(dataFolder() + L"records\\");
    horizon::writeFileAtomic(dataFolder() + L"records\\" + horizon::wide(wrh::fileKey(g_stats.uid)) + L".Replay.Gbx", std::string(bytes.begin(), bytes.end()));
    setTargetFromReplay(r, "TMX record" + (r.driver.empty() ? std::string() : " (" + ck::stripCodes(r.driver) + ")"));
    g_targetStatus = "";
}

void HORIZON_CALL onRecordInfo(void* user, int32_t status, const char*, const void* body, uint32_t len) {
    if (reinterpret_cast<uintptr_t>(user) != g_generation || !g_host) return;
    if (status != 200 || !body) {
        g_targetStatus = status == HORIZON_BRIDGE_ERR_NOT_CONNECTED ? "Start TM Horizon for the TMX record." : "TMX did not answer.";
        return;
    }
    horizon::Json j;
    std::string err;
    if (!horizon::JsonParser::parse(std::string(static_cast<const char*>(body), len), j, err)) {
        g_targetStatus = "TMX answer unreadable.";
        return;
    }
    const horizon::Json* found = j.get("found");
    const horizon::Json* rid = j.get("replayId");
    if (!found || found->type != horizon::Json::Bool || !found->b || !rid) {
        g_targetStatus = "This map has no record on TMX.";
        return;
    }
    const std::string path = std::string("/v1/tmx/replay?game=") + ck::siteCode(g_host) + "&id=" + std::to_string(rid->asInt(0));
    g_targetStatus = "Loading the record replay...";
    g_host->bridge_request(g_host->plugin, "GET", path.c_str(), nullptr, 0, &onReplayBytes, reinterpret_cast<void*>(static_cast<uintptr_t>(g_generation)));
}

void refreshTarget() {
    const int mode = setting("Target", kTargetTmx);
    g_target = wrh::Comparison();
    g_targetStatus.clear();
    g_tmxAsked = false;
    if (!g_haveMap) return;
    if (mode == kTargetPb) return;
    if (mode == kTargetTmx) {
        if (!g_uidKnown) {
            g_targetStatus = "Map UID unknown: no TMX record.";
            return;
        }
        // cached record replay first
        const std::wstring cached = dataFolder() + L"records\\" + horizon::wide(wrh::fileKey(g_stats.uid)) + L".Replay.Gbx";
        ck::Replay r;
        std::string err;
        if (horizon::fileExists(cached) && ck::readReplay(cached, r, err) && r.mapUid == g_stats.uid) {
            setTargetFromReplay(r, "TMX record" + (r.driver.empty() ? std::string() : " (" + ck::stripCodes(r.driver) + ")"));
            return;
        }
        if (!ck::abiAtLeast(g_host, 3) || !g_host->bridge_request) {
            g_targetStatus = "This Horizon Client has no TM Horizon bridge.";
            return;
        }
        const std::string path = std::string("/v1/tmx/record?game=") + ck::siteCode(g_host) + "&uid=" + ck::urlEscape(g_stats.uid);
        const HorizonResult hr =
            g_host->bridge_request(g_host->plugin, "GET", path.c_str(), nullptr, 0, &onRecordInfo, reinterpret_cast<void*>(static_cast<uintptr_t>(g_generation)));
        g_tmxAsked = true;
        g_targetStatus = hr == HORIZON_OK ? "Asking TMX..." : "TM Horizon connection is off.";
        return;
    }
    if (mode == kTargetGhost) {
        g_targetStatus = g_forever ? "Waiting for a ghost in the race." : "Ghosts in the race are read on TMNF/TMUF only.";
        g_ghostTimer = 0;
        return;
    }
    if (mode == kTargetFile) {
        const std::string path = ck::getString(g_host, "TargetFile", "");
        ck::Replay r;
        std::string err;
        if (path.empty()) g_targetStatus = "Pick a replay in Settings.";
        else if (!ck::readReplay(horizon::wide(path), r, err)) g_targetStatus = "Replay unreadable (" + err + ").";
        else if (g_uidKnown && r.mapUid != g_stats.uid) g_targetStatus = "That replay is of another map.";
        else setTargetFromReplay(r, "Replay" + (r.driver.empty() ? std::string() : " (" + ck::stripCodes(r.driver) + ")"));
    }
}

void pollGhost() {
    if (setting("Target", kTargetTmx) != kTargetGhost || !g_forever || !g_reader) return;
    if (++g_ghostTimer % 60 != 1) return;
    ck::GhostTimes gt;
    if (g_reader->bestGhost(g_mem, &gt) && (gt.raceTime != g_target.time || gt.cps != g_target.splits)) {
        g_target.name = "Ghost";
        g_target.time = gt.raceTime;
        g_target.splits = gt.cps;
        g_targetStatus.clear();
    }
}

// ---------------------------------------------------------------- run tracking
void startRun() {
    if (!g_haveMap) return;
    if (g_runState == 1 && (!g_live.empty() || g_nowMs > 500)) {
        g_stats.resets++;
        g_dirty = true;
    }
    g_stats.attempts++;
    g_dirty = true;
    g_live.clear();
    g_runState = 1;
    g_respawns = -1;
    g_cpEvents.start(clockMs());
    g_lastFinish = wrh::FinishResult();
    save();
}

void addSplit(size_t index, int time) {
    if (g_runState != 1) return;
    if (index != g_live.size() || g_live.size() >= wrh::kMaxSplits) return;
    g_live.push_back(time);
}

// ESWC/Sunrise/Original: a CHECKPOINT event of the host. Its index is not used: on Original the host sends one event
// per OnCheckpoint call, about 50 at each checkpoint (wrh::CpEvents keeps the first of them).
void onCheckpointEvent(int hostTime) {
    if (g_runState != 1) return;
    int last = 0;
    for (int t : g_live)
        if (t > last) last = t;
    int split = -1;
    if (g_cpEvents.add(hostTime, clockMs(), last, &split)) addSplit(g_live.size(), split);
}

void finishRun(int time, int respawns) {
    if (!g_haveMap || time <= 0) return;
    if (g_runState != 1) {
        // a finish without a start we saw (plugin switched on mid-run): count it, no reset
        g_runState = 1;
    }
    if (g_live.empty() || g_live.back() != time) g_live.push_back(time);
    g_runState = 2;
    const bool hadPb = !g_stats.pb.empty();
    const int oldPb = g_stats.pbTime();
    g_lastFinish = wrh::applyFinish(g_stats, g_live, respawns, static_cast<long long>(::time(nullptr)));
    g_dirty = true;
    save();
    if (g_lastFinish.pb && hadPb && setting("PbToast", 1)) {
        const std::string text = fmtTime(time) + "  (" + fmtDelta(time - oldPb) + ")";
        ck::toast(g_host, HORIZON_TOAST_OK, "New personal best", text.c_str(), HZ_ICON_TROPHY);
    }
    g_flashUntil = GetTickCount64() + 2500;
}

void loadMap(const HorizonEvent* e) {
    save();
    g_generation++;
    g_haveMap = true;
    g_mapName = e->map && e->map->name ? e->map->name : "";
    g_authorTime = e->map ? e->map->author_time : -1;
    const std::string uid = readMapUid(e);
    g_uidKnown = !uid.empty();
    const std::string key = g_uidKnown ? uid : wrh::localKey(g_mapName, g_authorTime);
    g_stats = wrh::MapStats();
    g_stats.key = key;
    std::string text;
    if (horizon::readFile(statsPath(), text)) wrh::fromJson(text, &g_stats);
    g_stats.key = key;
    g_stats.uid = uid;
    if (!g_mapName.empty()) g_stats.name = g_mapName;
    g_live.clear();
    g_runState = 0;
    g_nowMs = -1;
    g_cpEvents.clear();
    g_lastFinish = wrh::FinishResult();
    g_host->log(g_host->plugin, HORIZON_LOG_INFO, "map %s (%s): %d attempts, PB %s", g_mapName.c_str(), g_uidKnown ? "UID" : "local key",
                g_stats.attempts, fmtTime(g_stats.pbTime()).c_str());
    refreshTarget();
}

// Forever: the checkpoint list of the local player, read once a frame.
void pollForever() {
    if (!g_forever || !g_reader || !g_haveMap) return;
    void* race = g_host->engine_pointer(HORIZON_ENGINE_RACE);
    g_reader->poll(g_mem, reinterpret_cast<uintptr_t>(race));
    const ck::Run& r = g_reader->run();
    if (!r.race) return;
    if (r.state == 1) g_nowMs = r.timeMs;
    if (g_runState != 1) return;
    // new splits (the finish comes with the FINISH event: the list's last entry once state is 2)
    for (size_t i = g_live.size(); i < r.cps.size(); i++) {
        if (r.state == 2 && i + 1 == r.cps.size()) break;
        addSplit(i, r.cps[i]);
    }
}

// ---------------------------------------------------------------- drawing
// text cut with "..." to a pixel width (map names can be long)
std::string fitText(const std::string& text, float maxW) {
    if (maxW <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= maxW) return text;
    std::string cut = text;
    while (!cut.empty()) {
        cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0u) == 0x80u) cut.pop_back();
        if (ImGui::CalcTextSize((cut + "...").c_str()).x <= maxW) return cut + "...";
    }
    return "...";
}

// One line of the latest checkpoint deltas, each in its colour (blue faster, red slower; target first, else PB).
void drawCpStrip(const ck::Palette& p, const std::vector<wrh::Row>& rows, int done, float lineW) {
    const float s = ck::uiScale(g_host);
    std::vector<const wrh::Row*> shown;
    for (int i = 0; i < done && i < static_cast<int>(rows.size()); ++i) {
        const wrh::Row& r = rows[static_cast<size_t>(i)];
        if (r.dTarget != kNoDelta || r.dPb != kNoDelta) shown.push_back(&r);
    }
    if (shown.empty()) return;
    // as many of the latest as fit into the line
    const float cellW = ImGui::CalcTextSize("+00.00").x + 10.0f * s;
    const size_t fit = static_cast<size_t>(std::max(1.0f, std::floor(lineW / cellW)));
    const size_t first = shown.size() > fit ? shown.size() - fit : 0;
    const float x0 = ImGui::GetCursorPosX();
    for (size_t i = first; i < shown.size(); ++i) {
        const wrh::Row& r = *shown[i];
        const bool target = r.dTarget != kNoDelta;
        const int d = target ? r.dTarget : r.dPb;
        const ImVec4 c = ck::deltaColour(p, d, target ? r.prevDTarget : r.prevDPb, !target && r.isGold);
        if (i != first) ImGui::SameLine(x0 + cellW * static_cast<float>(i - first));
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::TextUnformatted(fmtDelta(d).c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s %d: %s vs %s", r.finish ? "Finish" : "CP", r.index + 1, fmtDelta(d).c_str(), target ? g_target.name.c_str() : "PB");
    }
}

void drawCompact(const ck::Palette& p, const std::vector<wrh::Row>& rows) {
    const float s = ck::uiScale(g_host);
    ImFont* mono = ck::font(g_host, 2);
    ImFont* title = ck::font(g_host, 1);
    const int n = static_cast<int>(rows.size());
    const int done = static_cast<int>(g_live.size());
    // the last split's delta (target first, else PB)
    int delta = kNoDelta, prev = kNoDelta;
    bool gold = false;
    const char* against = "";
    if (done > 0 && done <= n) {
        const wrh::Row& r = rows[static_cast<size_t>(done - 1)];
        gold = r.isGold;
        if (r.dTarget != kNoDelta) {
            delta = r.dTarget;
            prev = r.prevDTarget;
            against = "target";
        } else if (r.dPb != kNoDelta) {
            delta = r.dPb;
            prev = r.prevDPb;
            against = "PB";
        }
    }
    // running: show the delta once behind the next split
    if (g_runState == 1 && g_nowMs >= 0) {
        const std::vector<int>& cmp = !g_target.splits.empty() ? g_target.splits : g_stats.pb;
        const int run = wrh::runningDelta(g_live, g_nowMs, cmp);
        if (run != kNoDelta && (delta == kNoDelta || run > delta)) {
            delta = run;
            prev = kNoDelta;
            gold = false;
        }
    }
    // The widget fits its content (auto-resize). Right-aligning against GetContentRegionAvail() made the window
    // as wide as the text's right edge, which moved the edge again next frame: the window grew every frame
    // until it spanned the screen (seen at the finish). Lines now have a fixed width: kLineW, names cut to fit.
    const float lineW = 300.0f * s;
    char cp[32] = "";
    if (n > 0) {
        if (done >= n) snprintf(cp, sizeof(cp), "Finish");
        else snprintf(cp, sizeof(cp), "CP %d/%d", done, n - 1);
    }
    const float cpW = cp[0] ? ImGui::CalcTextSize(cp).x : 0.0f;
    ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
    ImGui::TextUnformatted(fitText(g_mapName.empty() ? std::string("WR Hunter") : g_mapName, lineW - cpW - 12.0f * s).c_str());
    ImGui::PopStyleColor();
    if (cp[0]) {
        ImGui::SameLine(ImGui::GetCursorPosX() + lineW - cpW);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text2);
        ImGui::TextUnformatted(cp);
        ImGui::PopStyleColor();
    }
    // big delta
    ImGui::PushFont(title);
    const std::string big = delta == kNoDelta ? (g_runState == 1 && g_nowMs >= 0 ? fmtTime(g_nowMs) : std::string("--")) : fmtDelta(delta);
    ImGui::PushStyleColor(ImGuiCol_Text, delta == kNoDelta ? p.text : ck::deltaColour(p, delta, prev, gold));
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted(big.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (*against) {
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f * s);
        ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
        ImGui::Text("vs %s", against);
        ImGui::PopStyleColor();
    }
    // the second comparison + the clock
    ImGui::PushFont(mono);
    if (done > 0 && done <= n) {
        const wrh::Row& r = rows[static_cast<size_t>(done - 1)];
        if (r.dTarget != kNoDelta && r.dPb != kNoDelta) {
            ImGui::PushStyleColor(ImGuiCol_Text, ck::deltaColour(p, r.dPb, r.prevDPb, r.isGold));
            ImGui::Text("PB %s", fmtDelta(r.dPb).c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 14.0f * s);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, p.text2);
        ImGui::Text("Split %s", fmtTime(r.time).c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, p.text2);
        ImGui::TextUnformatted(g_runState == 1 ? "Go!" : "Waiting for the start");
        ImGui::PopStyleColor();
    }
    if (setting("CpStrip", 1)) drawCpStrip(p, rows, std::min(done, n), lineW);
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::PushFont(mono);
    ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
    ImGui::Text("PB  %s", fmtTime(g_stats.pbTime()).c_str());
    if (setting("ShowSob", 1)) {
        ImGui::SameLine(0, 14.0f * s);
        ImGui::Text("SoB %s", fmtTime(wrh::sumOfBest(g_stats)).c_str());
    }
    if (!g_target.splits.empty()) ImGui::Text("%s %s", g_target.name.c_str(), fmtTime(g_target.time).c_str());
    else if (!g_targetStatus.empty() && setting("Target", kTargetTmx) != kTargetPb) ImGui::TextUnformatted(fitText(g_targetStatus, 300.0f * s).c_str());
    if (setting("ShowResets", 1)) ImGui::Text("Runs %d  Resets %d  Finishes %d", g_stats.attempts, g_stats.resets, g_stats.finishes);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void drawDetailed(const ck::Palette& p, const std::vector<wrh::Row>& rows) {
    const float s = ck::uiScale(g_host);
    ImFont* mono = ck::font(g_host, 2);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    ImGui::TextUnformatted(fitText(g_mapName.empty() ? std::string("WR Hunter") : g_mapName, 380.0f * s).c_str());
    ImGui::PopStyleColor();
    const bool haveTarget = !g_target.splits.empty();
    const int cols = haveTarget ? 5 : 4;
    // only the checkpoints this run crossed, at most kRowsShown lines (scrolls, follows the latest); fixed width
    const size_t crossed = std::min(g_live.size(), rows.size());
    g_rowsShown = static_cast<int>(crossed);
    ImGui::PushFont(mono);
    if (crossed == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
        ImGui::TextUnformatted(g_haveMap ? "Drive a run: the splits show up here." : "No map loaded.");
        ImGui::PopStyleColor();
    } else {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float rowH = ImGui::GetTextLineHeight() + st.CellPadding.y * 2.0f;
        const float tableW = (44.0f + 74.0f + 62.0f + (haveTarget ? 62.0f : 0.0f) + 62.0f) * s + st.CellPadding.x * 2.0f * static_cast<float>(cols) +
                             st.WindowPadding.x;
        const float tableH = rowH * static_cast<float>(std::min(crossed, kRowsShown) + 1) + 2.0f;
        if (ImGui::BeginTable("##splits", cols, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_ScrollY,
                              ImVec2(tableW, tableH))) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 44.0f * s);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 74.0f * s);
            ImGui::TableSetupColumn("PB", ImGuiTableColumnFlags_WidthFixed, 62.0f * s);
            if (haveTarget) ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 62.0f * s);
            ImGui::TableSetupColumn("Sector", ImGuiTableColumnFlags_WidthFixed, 62.0f * s);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
            ImGui::TableHeadersRow();
            ImGui::PopStyleColor();
            for (size_t k = 0; k < crossed; k++) {
                const wrh::Row& r = rows[k];
                ImGui::TableNextRow();
                if (k + 1 == crossed && g_runState == 1) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ck::u32(p.accent, 0.18f));
                if (k + 1 == crossed && crossed != g_rowsFollowed) {
                    ImGui::SetScrollHereY(1.0f); // the latest split stays in view
                    g_rowsFollowed = crossed;
                }
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, p.text2);
                if (r.finish) ImGui::TextUnformatted("Fin");
                else ImGui::Text("%d", r.index + 1);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                if (r.time >= 0) {
                    ImGui::TextUnformatted(fmtTime(r.time).c_str());
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
                    ImGui::TextUnformatted(r.pbSplit >= 0 ? fmtTime(r.pbSplit).c_str() : "-");
                    ImGui::PopStyleColor();
                }
                ImGui::TableNextColumn();
                if (r.dPb != kNoDelta) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ck::deltaColour(p, r.dPb, r.prevDPb, r.isGold));
                    ImGui::TextUnformatted(fmtDelta(r.dPb).c_str());
                    ImGui::PopStyleColor();
                }
                if (haveTarget) {
                    ImGui::TableNextColumn();
                    if (r.dTarget != kNoDelta) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ck::deltaColour(p, r.dTarget, r.prevDTarget, false));
                        ImGui::TextUnformatted(fmtDelta(r.dTarget).c_str());
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::TableNextColumn();
                if (r.sector > 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, r.isGold ? p.gold : p.text2);
                    ImGui::TextUnformatted(fmtTime(r.sector).c_str() + (r.sector < 60000 ? 2 : 0));
                    ImGui::PopStyleColor();
                } else if (r.gold > 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
                    ImGui::TextUnformatted(fmtTime(r.gold).c_str() + (r.gold < 60000 ? 2 : 0));
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::PushFont(mono);
    ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
    if (g_runState == 1 && g_nowMs >= 0) ImGui::Text("Time %s", fmtTime(g_nowMs).c_str());
    ImGui::Text("PB  %s", fmtTime(g_stats.pbTime()).c_str());
    if (setting("ShowSob", 1)) {
        ImGui::SameLine(0, 14.0f * s);
        ImGui::Text("SoB %s", fmtTime(wrh::sumOfBest(g_stats)).c_str());
    }
    if (haveTarget) ImGui::Text("%s %s", g_target.name.c_str(), fmtTime(g_target.time).c_str());
    else if (!g_targetStatus.empty() && setting("Target", kTargetTmx) != kTargetPb) ImGui::TextUnformatted(fitText(g_targetStatus, 300.0f * s).c_str());
    if (setting("ShowResets", 1)) ImGui::Text("Runs %d  Resets %d  Finishes %d", g_stats.attempts, g_stats.resets, g_stats.finishes);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void HORIZON_CALL drawSplits(void*) {
    if (!g_ui) return;
    const ck::Palette p = ck::palette(g_host);
    const std::vector<wrh::Row> rows = wrh::buildRows(g_live, g_stats, g_target);
    ImGui::PushID("wrh-splits");
    g_rowsShown = 0;
    if (setting("Layout", 0) == 1) drawDetailed(p, rows);
    else drawCompact(p, rows);
    if (g_runState == 2 && GetTickCount64() < g_flashUntil && g_lastFinish.pb) {
        ImGui::PushStyleColor(ImGuiCol_Text, p.gold);
        ImGui::TextUnformatted(g_lastFinish.firstRun ? "First finish" : "Personal best!");
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
}

void loadAllMaps() {
    g_allMaps.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((mapsFolder() + L"*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        g_allMapsLoaded = true;
        return;
    }
    do {
        std::string text;
        wrh::MapStats st;
        if (horizon::readFile(mapsFolder() + fd.cFileName, text) && wrh::fromJson(text, &st))
            g_allMaps.push_back(MapRow{st.name.empty() ? st.key : st.name, st.key, st.pbTime(), st.attempts, st.finishes});
    } while (FindNextFileW(h, &fd) && g_allMaps.size() < 2000);
    FindClose(h);
    std::sort(g_allMaps.begin(), g_allMaps.end(), [](const MapRow& a, const MapRow& b) { return a.attempts > b.attempts; });
    g_allMapsLoaded = true;
}

void HORIZON_CALL drawHistory(void*) {
    if (!g_ui) return;
    const ck::Palette p = ck::palette(g_host);
    const float s = ck::uiScale(g_host);
    if (g_haveMap) {
        ck::section(p, g_mapName.empty() ? "This map" : g_mapName.c_str());
        ImGui::Text("PB %s   Sum of best %s   Runs %d   Resets %d   Finishes %d", fmtTime(g_stats.pbTime()).c_str(),
                    fmtTime(wrh::sumOfBest(g_stats)).c_str(), g_stats.attempts, g_stats.resets, g_stats.finishes);
        if (!g_stats.golds.empty() && g_stats.golds.size() == g_stats.pb.size()) {
            const std::vector<int> pbSec = wrh::sectors(g_stats.pb);
            ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
            ImGui::TextUnformatted("Gold sectors (time to gain against the PB)");
            ImGui::PopStyleColor();
            std::string line;
            for (size_t i = 0; i < g_stats.golds.size(); i++) {
                char b[64];
                snprintf(b, sizeof(b), "%s%s %s (%s)", i ? "   " : "", i + 1 == g_stats.golds.size() ? "Fin" : std::to_string(i + 1).c_str(),
                         fmtTime(g_stats.golds[i]).c_str() + (g_stats.golds[i] < 60000 ? 2 : 0), fmtDelta(g_stats.golds[i] - pbSec[i]).c_str());
                line += b;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, p.gold);
            ImGui::TextWrapped("%s", line.c_str());
            ImGui::PopStyleColor();
        }
        if (g_stats.runs.empty()) {
            ck::hint(p, "No finished run on this map yet.");
        } else if (ImGui::BeginTable("##runs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                                     ImVec2(0, 220.0f * s))) {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 90.0f * s);
            ImGui::TableSetupColumn("vs PB", ImGuiTableColumnFlags_WidthFixed, 80.0f * s);
            ImGui::TableSetupColumn("Respawns", ImGuiTableColumnFlags_WidthFixed, 80.0f * s);
            ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (size_t k = g_stats.runs.size(); k-- > 0;) {
                const wrh::RunRecord& r = g_stats.runs[k];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool isPb = r.time == g_stats.pbTime();
                ImGui::PushStyleColor(ImGuiCol_Text, isPb ? p.gold : p.text);
                ImGui::TextUnformatted(fmtTime(r.time).c_str());
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                const int d = r.time - g_stats.pbTime();
                ImGui::PushStyleColor(ImGuiCol_Text, d <= 0 ? p.aheadGain : p.text2);
                ImGui::TextUnformatted(fmtDelta(d).c_str());
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                if (r.respawns >= 0) ImGui::Text("%d", r.respawns);
                else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                char when[32] = "-";
                const time_t t = static_cast<time_t>(r.at);
                tm lt;
                if (r.at > 0 && localtime_s(&lt, &t) == 0) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &lt);
                ImGui::TextUnformatted(when);
            }
            ImGui::EndTable();
        }
    } else {
        ck::hint(p, "Load a map to see its runs.");
    }
    ck::section(p, "All maps");
    if (!g_allMapsLoaded) loadAllMaps();
    if (g_allMaps.empty()) {
        ck::hint(p, "Nothing yet.");
    } else if (ImGui::BeginTable("##maps", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, 180.0f * s))) {
        ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("PB", ImGuiTableColumnFlags_WidthFixed, 90.0f * s);
        ImGui::TableSetupColumn("Runs", ImGuiTableColumnFlags_WidthFixed, 60.0f * s);
        ImGui::TableSetupColumn("Finishes", ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const MapRow& m : g_allMaps) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmtTime(m.pb).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", m.attempts);
            ImGui::TableNextColumn();
            ImGui::Text("%d", m.finishes);
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------- replay picker (Settings)
std::wstring replaysFolder() {
    const HorizonGameInfo* gi = g_host->game_info();
    if (!gi) return std::wstring();
    if ((gi->game & HORIZON_GAME_FOREVER) && gi->user_folder && *gi->user_folder) return horizon::wide(gi->user_folder) + L"Tracks\\Replays\\";
    return horizon::wide(gi->game_folder ? gi->game_folder : "") + L"GameData\\Tracks\\Replays\\";
}

void scanReplays(std::wstring folder, std::string uid) {
    std::vector<ReplayEntry> found;
    std::vector<std::wstring> dirs{folder};
    size_t files = 0;
    while (!dirs.empty() && files < 4000) {
        const std::wstring d = dirs.back();
        dirs.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (dirs.size() < 200) dirs.push_back(d + name + L"\\");
                continue;
            }
            if (name.size() < 11 || _wcsicmp(name.c_str() + name.size() - 11, L".replay.gbx") != 0) continue;
            files++;
            ck::Replay r;
            std::string err;
            if (ck::readReplayHeader(d + name, r, err) && r.mapUid == uid)
                found.push_back(ReplayEntry{d + name, ck::stripCodes(r.driver), horizon::utf8(name), r.raceTime});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(found.begin(), found.end(), [](const ReplayEntry& a, const ReplayEntry& b) { return a.time < b.time; });
    std::lock_guard<std::mutex> lock(g_pickMutex);
    g_picked = std::move(found);
    g_scanning = false;
}

void startScan() {
    if (g_scanning || !g_uidKnown) return;
    if (g_scanThread.joinable()) g_scanThread.join();
    g_scanning = true;
    g_scanUid = g_stats.uid;
    g_scanThread = std::thread(scanReplays, replaysFolder(), g_scanUid);
}

} // namespace

// ---------------------------------------------------------------- exports
extern "C" {

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) {
    static const HorizonPluginInfo info = {HORIZON_ABI,
                                           "wr-hunter",
                                           "WR Hunter",
                                           "0.1.0",
                                           HORIZON_GAME_ALL,
                                           HORIZON_CAP_MENU | HORIZON_CAP_OVERLAY | HORIZON_CAP_GAME_EVENTS,
                                           HORIZON_FLAG_ONLINE_OK | HORIZON_FLAG_LIVE_TOGGLE,
                                           "cheatoskar",
                                           "Split times against your best and the TMX record, with gold sectors and history.",
                                           HORIZON_ABI_MINOR};
    return &info;
}

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    if (!host || host->abi != HORIZON_ABI) return 1;
    g_host = host;
    // a fresh start (the DLL stays loaded when the plugin is switched off and on, or between test runs)
    g_ids = ck::IdTable();
    g_idsTried = false;
    g_uidOffset = -1;
    g_haveMap = false;
    g_mapName.clear();
    g_stats = wrh::MapStats();
    g_dirty = false;
    g_live.clear();
    g_runState = 0;
    g_nowMs = -1;
    g_target = wrh::Comparison();
    g_targetStatus.clear();
    g_allMapsLoaded = false;
    g_enabled = true;
    g_ui = horizon_imgui_attach(host);
    if (!g_ui) host->log(host->plugin, HORIZON_LOG_WARN, "Dear ImGui version differs from the host: no widget");
    g_forever = ck::isForever(host);
    const bool reflect = ck::liveReflectInit(host);
    std::string missing;
    const ck::Layout L = ck::resolveLayout(g_forever ? ck::Engine::Forever : ck::Engine::Legacy, &ck::liveOffset, &missing);
    g_reader.reset(new ck::Reader(L));
    host->log(host->plugin, HORIZON_LOG_INFO, "times from %s; reflection %s; %s",
              g_forever ? "the player's checkpoint list (read only, once a frame)" : "the host's checkpoint events", reflect ? "ready" : "not available",
              missing.c_str());
    host->subscribe(host->plugin, HORIZON_EVENT_BIT(HORIZON_EVENT_MAP_LOADED) | HORIZON_EVENT_BIT(HORIZON_EVENT_RACE_START) |
                                      HORIZON_EVENT_BIT(HORIZON_EVENT_CHECKPOINT) | HORIZON_EVENT_BIT(HORIZON_EVENT_FINISH) |
                                      HORIZON_EVENT_BIT(HORIZON_EVENT_RESPAWN) | HORIZON_EVENT_BIT(HORIZON_EVENT_MENU_ENTERED));
    if (ck::abiAtLeast(host, 2) && host->register_panel) {
        HorizonPanelDef d = {};
        d.size = sizeof(d);
        d.key = "splits";
        d.title = "Splits";
        d.icon = HZ_ICON_TIMER;
        d.flags = HORIZON_PANEL_HUD;
        d.draw = &drawSplits;
        d.default_x = 0.70f; // the detailed table is about 480 px wide
        d.default_y = 0.22f;
        host->register_panel(host->plugin, &d);
        d.key = "history";
        d.title = "History";
        d.icon = HZ_ICON_SCROLL_TEXT;
        d.flags = HORIZON_PANEL_MENU_PAGE;
        d.draw = &drawHistory;
        d.default_x = 0.30f;
        d.default_y = 0.20f;
        d.default_w = 0.40f;
        d.default_h = 0.55f;
        host->register_panel(host->plugin, &d);
    }
    g_generation++;
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    save();
    g_generation++;
    if (g_scanThread.joinable()) g_scanThread.join();
    g_reader.reset();
    g_host = nullptr;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_SetEnabled(int32_t enabled) {
    g_enabled = enabled != 0;
    if (!g_enabled) save();
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_OnEvent(const HorizonEvent* e) {
    if (!g_host || !e || !g_enabled) return;
    switch (e->type) {
    case HORIZON_EVENT_MAP_LOADED: loadMap(e); break;
    case HORIZON_EVENT_RACE_START: startRun(); break;
    case HORIZON_EVENT_CHECKPOINT:
        if (!g_forever) onCheckpointEvent(e->time_ms);
        break;
    case HORIZON_EVENT_RESPAWN: g_respawns = e->respawns; break;
    case HORIZON_EVENT_FINISH:
        if (e->is_local_player) {
            if (g_forever) pollForever(); // splits up to the finish first
            // events of the finish line itself (the hook may fire there too) are not a checkpoint
            else if (g_runState == 1 && !g_live.empty() && g_cpEvents.justTaken(clockMs())) g_live.pop_back();
            finishRun(e->time_ms, e->respawns >= 0 ? e->respawns : g_respawns);
        }
        break;
    case HORIZON_EVENT_MENU_ENTERED:
        save();
        g_runState = 0;
        g_live.clear();
        g_cpEvents.clear();
        break;
    default: break;
    }
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawOverlay(void) {
    if (!g_host || !g_enabled) return;
    pollForever();
    pollGhost();
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawMenu(void) {
    if (!g_ui || !g_host) return;
    const ck::Palette p = ck::palette(g_host);
    ck::section(p, "Layout");
    int layout = setting("Layout", 0);
    if (ImGui::RadioButton("Compact", layout == 0)) setSetting("Layout", 0);
    ImGui::SameLine();
    if (ImGui::RadioButton("Detailed", layout == 1)) setSetting("Layout", 1);
    bool sob = setting("ShowSob", 1) != 0, resets = setting("ShowResets", 1) != 0, toast = setting("PbToast", 1) != 0;
    if (ImGui::Checkbox("Sum of best", &sob)) setSetting("ShowSob", sob ? 1 : 0);
    ImGui::SameLine();
    if (ImGui::Checkbox("Runs and resets", &resets)) setSetting("ShowResets", resets ? 1 : 0);
    ImGui::SameLine();
    if (ImGui::Checkbox("PB note", &toast)) setSetting("PbToast", toast ? 1 : 0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A short note in the corner after a new personal best.");

    ck::section(p, "Compare with");
    const int target = setting("Target", kTargetTmx);
    const char* names[] = {"Personal best only", "TMX record", "Ghost in the race", "Replay file"};
    for (int i = 0; i < 4; i++) {
        if (i == kTargetGhost && !g_forever) continue;
        if (ImGui::RadioButton(names[i], target == i)) {
            setSetting("Target", i);
            refreshTarget();
        }
        if (i == kTargetTmx && ImGui::IsItemHovered()) ImGui::SetTooltip("The record replay from TMX, through TM Horizon.");
        if (i == kTargetGhost && ImGui::IsItemHovered()) ImGui::SetTooltip("The fastest ghost the race shows (a medal, a loaded replay).");
    }
    if (!g_targetStatus.empty()) ck::hint(p, g_targetStatus.c_str());
    else if (!g_target.splits.empty())
        ImGui::Text("%s: %s, %d splits", g_target.name.c_str(), fmtTime(g_target.time).c_str(), static_cast<int>(g_target.splits.size()));
    if (target == kTargetTmx && g_uidKnown && ImGui::Button("Reload")) {
        DeleteFileW((dataFolder() + L"records\\" + horizon::wide(wrh::fileKey(g_stats.uid)) + L".Replay.Gbx").c_str());
        refreshTarget();
    }
    if (target == kTargetFile) {
        if (!g_uidKnown) {
            ck::hint(p, "The map's UID is unknown, so replays can't be matched.");
        } else {
            if (ImGui::Button(g_scanning ? "Looking..." : "Find replays")) startScan();
            std::lock_guard<std::mutex> lock(g_pickMutex);
            const std::string chosen = ck::getString(g_host, "TargetFile", "");
            for (size_t i = 0; i < g_picked.size() && i < 30; i++) {
                const ReplayEntry& r = g_picked[i];
                const std::string label = fmtTime(r.time) + "  " + (r.driver.empty() ? r.file : r.driver) + "##rp" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), horizon::utf8(r.path) == chosen)) {
                    g_host->set_string(g_host->plugin, "TargetFile", horizon::utf8(r.path).c_str());
                    refreshTarget();
                }
            }
            if (!g_scanning && g_picked.empty()) ck::hint(p, "No replay of this map found yet.");
        }
    }

    ck::section(p, "This map");
    ImGui::Text("%s", g_haveMap ? (g_mapName.empty() ? "(no name)" : g_mapName.c_str()) : "No map loaded");
    if (g_haveMap) {
        ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
        ImGui::Text("%s  |  times from %s", g_uidKnown ? "kept by UID" : "kept by name (UID unknown)",
                    g_forever ? (g_reader && g_reader->run().cpsFromList ? "the game's checkpoint list" : "the checkpoint counter") : "the host's events");
        ImGui::PopStyleColor();
        static bool confirm = false;
        if (!confirm && ImGui::Button("Reset map stats")) confirm = true;
        if (confirm) {
            ImGui::TextUnformatted("Delete PB, golds and history of this map?");
            if (ImGui::Button("Delete")) {
                const std::string key = g_stats.key, uid = g_stats.uid, name = g_stats.name;
                g_stats = wrh::MapStats();
                g_stats.key = key;
                g_stats.uid = uid;
                g_stats.name = name;
                g_dirty = true;
                save();
                confirm = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep")) confirm = false;
        }
    }
    ck::hint(p, "Split times only: no checkpoint positions, nothing changes the run. Allowed online.");
}

// Test hook (tests\wr_hunter_fakehost.cpp, the preview): "state" -> a one-line summary.
HORIZON_EXPORT int32_t __cdecl HorizonClient_TestHook(const char* cmd, char* out, int32_t outSize) {
    if (!cmd || !g_host) return -1;
    std::string reply;
    if (!strcmp(cmd, "state")) {
        reply = "map=" + g_stats.key + " uid=" + (g_uidKnown ? "1" : "0") + " live=" + std::to_string(g_live.size()) + " run=" + std::to_string(g_runState) +
                " pb=" + std::to_string(g_stats.pbTime()) + " sob=" + std::to_string(wrh::sumOfBest(g_stats)) + " attempts=" +
                std::to_string(g_stats.attempts) + " resets=" + std::to_string(g_stats.resets) + " finishes=" + std::to_string(g_stats.finishes) +
                " target=" + std::to_string(g_target.time) + " targetSplits=" + std::to_string(g_target.splits.size()) + " now=" + std::to_string(g_nowMs) +
                " pbSplits=" + std::to_string(g_stats.pb.size()) + " rows=" + std::to_string(g_rowsShown) + " splits=";
        for (size_t i = 0; i < g_live.size(); i++) reply += (i ? "," : "") + std::to_string(g_live[i]);
    } else if (!strncmp(cmd, "clock ", 6)) {
        g_testClock = static_cast<uint64_t>(strtoull(cmd + 6, nullptr, 10));
        reply = "ok";
    } else if (!strncmp(cmd, "target ", 7)) {
        setSetting("Target", atoi(cmd + 7));
        refreshTarget();
        reply = "ok";
    } else if (!strncmp(cmd, "layout ", 7)) {
        setSetting("Layout", atoi(cmd + 7));
        reply = "ok";
    } else {
        return -1;
    }
    if (out && outSize > 0) {
        strncpy_s(out, static_cast<size_t>(outSize), reply.c_str(), _TRUNCATE);
    }
    return static_cast<int32_t>(reply.size());
}

} // extern "C"
