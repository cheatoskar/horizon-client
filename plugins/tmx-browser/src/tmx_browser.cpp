// TMX Browser - a Horizon Host plugin: search the TrackMania Exchange from inside the game, look at the map
// cards, and download or play a map. Online data comes from TM Horizon over the host's bridge (the game
// process never goes online). The pure logic (queries, parsing, favourites, file names) is in tmx_logic.cpp.
//
// Threads: everything runs on the game thread (panels draw there, bridge callbacks come back there).
// Late answers: every request has an entry in g_pending and the generation it was sent in. Shutdown and
// switching the plugin off clear g_pending and bump g_gen, so nothing can touch freed state afterwards.
#define HORIZON_PLUGIN_BUILD
#define IMGUI_DEFINE_MATH_OPERATORS
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "imgui.h"
#include "horizon_imgui.h"
#include "horizon_plugin.h"
#include "ckui.h"
#include "tmx_logic.h"

using tmx::Track;

namespace {

constexpr char kVersion[] = "0.1.0";
constexpr size_t kMaxTextures = 48;
constexpr int kMaxThumbsInFlight = 3;

const HorizonHostApi* g_host = nullptr;
bool g_imgui = false;
bool g_enabled = false;
unsigned g_gen = 1;
std::string g_site;
bool g_forever = true;

// ---------------------------------------------------------------- requests
enum Kind { kSearch, kRandom, kThumb, kMap };
struct Pending {
    Kind kind = kSearch;
    unsigned gen = 0;
    int id = 0, page = 1;
    bool play = false;
    unsigned serial = 0;
    std::string name;
};
std::unordered_map<uintptr_t, Pending> g_pending;
uintptr_t g_nextReq = 1;

// ---------------------------------------------------------------- state
struct View {
    bool detail = false;
    Track t;
};
struct Browse {
    tmx::Form form, searched; // what the user edits / what the last search used
    std::vector<Track> results;
    int page = 0;
    bool more = false, loading = false, hasSearched = false;
    std::string err;
    char nameBuf[128] = "";
    char authorBuf[64] = "";
};
Browse g_b;
View g_view[2];
int g_tab = 0; // test hook: the panel the next play/save/fav acts on first
unsigned g_serial = 0;
bool g_large = false;

struct Msg {
    std::string text;
    int kind = 0; // 0 info, 1 ok, 2 warn
    ULONGLONG at = 0;
} g_msg;
bool g_mapBusy = false;
std::string g_lastFile;
std::vector<tmx::FavEntry> g_favs; // newest first

struct Thumb {
    uint64_t tex = 0;
    int w = 0, h = 0;
};
tmx::Lru<Thumb> g_thumbs(kMaxTextures);
std::unordered_set<int> g_noThumb, g_thumbInflight;
std::vector<int> g_wanted;
std::vector<uint64_t> g_deferred;
int g_frame = -1;
ULONGLONG g_thumbPause = 0;
int g_created = 0, g_released = 0;

// ---------------------------------------------------------------- small helpers
void logf(int level, const char* fmt, ...) {
    if (!g_host || !g_host->log) return;
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_host->log(g_host->plugin, level, "%s", buf);
}

void setMsg(const std::string& text, int kind) {
    g_msg.text = text;
    g_msg.kind = kind;
    g_msg.at = GetTickCount64();
}

bool connected() { return g_host && g_host->bridge_state && g_host->bridge_state() == HORIZON_BRIDGE_CONNECTED; }

std::string bridgeNeed(bool forMap) {
    const int s = g_host && g_host->bridge_state ? g_host->bridge_state() : HORIZON_BRIDGE_OFF;
    if (s == HORIZON_BRIDGE_SEARCHING && forMap) return "Start TM Horizon to download maps";
    return tmx::bridgeStateText(s);
}

// "" when Play may run now.
std::string playBlock() {
    if (!g_host) return "Not ready";
    if (g_host->is_online && g_host->is_online()) return "Playing is off while online";
    if (g_host->actions_allowed && !g_host->actions_allowed(g_host->plugin)) return "Playing is off during a run";
    if (g_host->get_game_state && g_host->get_game_state() != HORIZON_STATE_MAIN_MENU) return "Go back to the main menu to play";
    if (!connected()) return bridgeNeed(true);
    return "";
}
std::string saveBlock() { return connected() ? std::string() : bridgeNeed(true); }

void releaseTex(uint64_t tex) {
    if (tex && g_host && g_host->release_texture) {
        g_host->release_texture(g_host->plugin, tex);
        g_released++;
    }
}
void releaseDeferred() {
    for (uint64_t t : g_deferred) releaseTex(t);
    g_deferred.clear();
}
void releaseAllTextures() {
    std::vector<Thumb> all;
    g_thumbs.clear(&all);
    for (const Thumb& t : all) releaseTex(t.tex);
    releaseDeferred();
    g_thumbInflight.clear();
    g_noThumb.clear();
    g_wanted.clear();
}

// ---------------------------------------------------------------- settings, favourites
std::string dataFile(const char* name) { return std::string(g_host->data_folder(g_host->plugin)) + name; }

void saveForm() {
    const tmx::Form& f = g_b.form;
    g_host->set_string(g_host->plugin, "SearchName", f.name.c_str());
    g_host->set_string(g_host->plugin, "SearchAuthor", f.author.c_str());
    g_host->set_int(g_host->plugin, "SearchTags", static_cast<int32_t>(f.tagMask));
    g_host->set_int(g_host->plugin, "SearchDiff", f.difficulty);
    g_host->set_int(g_host->plugin, "SearchLen", f.length);
    g_host->set_int(g_host->plugin, "SearchAwards", f.awards);
}

void syncBuffers() {
    snprintf(g_b.nameBuf, sizeof(g_b.nameBuf), "%s", g_b.form.name.c_str());
    snprintf(g_b.authorBuf, sizeof(g_b.authorBuf), "%s", g_b.form.author.c_str());
}

void loadSettings() {
    tmx::Form& f = g_b.form;
    f = tmx::Form();
    f.name = ck::getString(g_host, "SearchName", "");
    f.author = ck::getString(g_host, "SearchAuthor", "");
    f.tagMask = static_cast<uint32_t>(g_host->get_int(g_host->plugin, "SearchTags", 0));
    f.difficulty = std::max(-1, std::min(3, static_cast<int>(g_host->get_int(g_host->plugin, "SearchDiff", -1))));
    f.length = std::max(0, std::min(tmx::kLengthCount - 1, static_cast<int>(g_host->get_int(g_host->plugin, "SearchLen", 0))));
    f.awards = std::max(0, std::min(9999, static_cast<int>(g_host->get_int(g_host->plugin, "SearchAwards", 0))));
    g_large = g_host->get_int(g_host->plugin, "GridLarge", 0) != 0;
    syncBuffers();
}

void saveFavs() {
    const std::string text = tmx::favouritesToJson(g_favs);
    if (!tmx::writeFile(dataFile("favourites.json"), text.data(), text.size())) logf(HORIZON_LOG_WARN, "could not write favourites.json");
}

void loadFavs() {
    g_favs.clear();
    std::string text;
    if (!tmx::readFile(dataFile("favourites.json"), text)) return;
    if (!tmx::parseFavourites(text, g_favs)) {
        logf(HORIZON_LOG_WARN, "favourites.json is not readable, starting empty");
        g_favs.clear();
    }
}

bool isFav(int id) {
    for (const tmx::FavEntry& f : g_favs)
        if (f.t.id == id && f.game == g_site) return true;
    return false;
}

void toggleFav(const Track& t) {
    for (size_t i = 0; i < g_favs.size(); i++)
        if (g_favs[i].t.id == t.id && g_favs[i].game == g_site) {
            g_favs.erase(g_favs.begin() + static_cast<long>(i));
            saveFavs();
            return;
        }
    tmx::FavEntry e;
    e.t = t;
    e.game = g_site;
    char d[16] = "";
    const time_t now = time(nullptr);
    struct tm lt;
    if (localtime_s(&lt, &now) == 0) strftime(d, sizeof(d), "%Y-%m-%d", &lt);
    e.added = d;
    g_favs.insert(g_favs.begin(), std::move(e));
    saveFavs();
}

std::vector<const Track*> favTracks() {
    std::vector<const Track*> v;
    for (const tmx::FavEntry& f : g_favs)
        if (f.game == g_site) v.push_back(&f.t);
    return v;
}

// ---------------------------------------------------------------- bridge
void HORIZON_CALL onBridge(void* user, int32_t status, const char* contentType, const void* body, uint32_t len);

bool bridgeSend(Pending p, const std::string& path, std::string* why) {
    if (!g_host || !g_host->bridge_request) return false;
    const uintptr_t key = g_nextReq++;
    p.gen = g_gen;
    g_pending[key] = p;
    const HorizonResult r = g_host->bridge_request(g_host->plugin, "GET", path.c_str(), nullptr, 0, &onBridge, reinterpret_cast<void*>(key));
    if (r == HORIZON_OK) return true;
    g_pending.erase(key);
    if (why) *why = r == HORIZON_ERR_BUSY ? "Too many requests, wait a moment" : r == HORIZON_ERR_NOT_SUPPORTED ? "The TM Horizon bridge is off" : "Could not send the request";
    return false;
}

void doSearch(int page) {
    if (!connected()) {
        g_b.err = tmx::bridgeStateText(g_host->bridge_state());
        return;
    }
    if (page <= 1) {
        g_b.searched = g_b.form;
        page = 1;
    }
    Pending p;
    p.kind = kSearch;
    p.page = page;
    p.serial = ++g_serial;
    std::string why;
    g_b.err.clear();
    g_b.loading = true;
    if (!bridgeSend(p, tmx::searchPath(g_site, g_b.searched, page), &why)) {
        g_b.loading = false;
        g_b.err = why;
    }
}

void doRandom() {
    if (!connected()) {
        g_b.err = tmx::bridgeStateText(g_host->bridge_state());
        return;
    }
    Pending p;
    p.kind = kRandom;
    p.serial = ++g_serial;
    std::string why;
    g_b.err.clear();
    g_b.loading = true;
    if (!bridgeSend(p, tmx::randomPath(g_site, g_b.form), &why)) {
        g_b.loading = false;
        g_b.err = why;
    }
}

// Download a map; play = also start it. All gates are checked here, the buttons only mirror them.
void doMap(const Track& t, bool play) {
    const std::string block = play ? playBlock() : saveBlock();
    if (!block.empty()) {
        setMsg(block, 2);
        return;
    }
    if (g_mapBusy) {
        setMsg("Busy, one map at a time", 2);
        return;
    }
    Pending p;
    p.kind = kMap;
    p.id = t.id;
    p.play = play;
    p.name = t.name;
    std::string why;
    g_mapBusy = true;
    setMsg(play ? "Downloading..." : "Saving...", 0);
    if (!bridgeSend(p, tmx::mapPath(g_site, t.id), &why)) {
        g_mapBusy = false;
        setMsg(why, 2);
    }
}

void handleMap(const Pending& p, int32_t status, const void* body, uint32_t len) {
    g_mapBusy = false;
    const std::string err = tmx::bridgeErrorText(status);
    if (!err.empty()) {
        if (status != HORIZON_BRIDGE_ERR_CANCELLED) setMsg(err, 2);
        return;
    }
    if (!body || len < 4 || memcmp(body, "GBX", 3) != 0) {
        setMsg("TMX did not send a map file", 2);
        return;
    }
    const HorizonGameInfo* gi = g_host->game_info();
    const std::string dir = tmx::downloadFolder(g_forever, gi && gi->user_folder ? gi->user_folder : "", gi && gi->game_folder ? gi->game_folder : "");
    const std::string path = dir + tmx::mapFileName(p.name, p.id);
    if (!tmx::createDirs(path) || !tmx::writeFile(path, body, len)) {
        setMsg("Could not write the map file", 2);
        logf(HORIZON_LOG_WARN, "could not write %s", path.c_str());
        return;
    }
    g_lastFile = path;
    if (!p.play) {
        setMsg("Saved as " + tmx::mapFileName(p.name, p.id), 1);
        ck::toast(g_host, HORIZON_TOAST_OK, "Map saved", p.name.c_str());
        return;
    }
    const std::string block = playBlock(); // the game may have changed state while the map downloaded
    if (!block.empty()) {
        setMsg("Saved. " + block, 2);
        ck::toast(g_host, HORIZON_TOAST_WARN, "Map saved", block.c_str());
        return;
    }
    const HorizonResult r = g_host->play_map_file(g_host->plugin, path.c_str(), 0);
    const std::string text = tmx::playResultText(r);
    if (r == HORIZON_OK) {
        setMsg(text, 1);
        ck::toast(g_host, HORIZON_TOAST_OK, "Map started", p.name.c_str());
    } else {
        setMsg(text, 2);
        ck::toast(g_host, HORIZON_TOAST_WARN, "Could not start the map", text.c_str());
    }
}

void handleThumb(const Pending& p, int32_t status, const void* body, uint32_t len) {
    g_thumbInflight.erase(p.id);
    if (status == HORIZON_BRIDGE_ERR_CANCELLED) return;
    if (status == HORIZON_BRIDGE_ERR_RATE_LIMIT || status == 429 || status == HORIZON_BRIDGE_ERR_TIMEOUT) {
        g_thumbPause = GetTickCount64() + 3000; // try again later, the card asks again
        return;
    }
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (status != 200 || !tmx::decodeThumb(body, len, rgba, w, h, 256)) {
        g_noThumb.insert(p.id);
        return;
    }
    const uint64_t tex = g_host->create_texture_rgba(g_host->plugin, static_cast<uint32_t>(w), static_cast<uint32_t>(h), rgba.data());
    if (!tex) {
        g_noThumb.insert(p.id);
        return;
    }
    g_created++;
    std::vector<Thumb> evicted;
    g_thumbs.put(p.id, Thumb{tex, w, h}, &evicted);
    for (const Thumb& e : evicted) g_deferred.push_back(e.tex); // may be in this frame's draw list: release next frame
}

void HORIZON_CALL onBridge(void* user, int32_t status, const char*, const void* body, uint32_t len) {
    auto it = g_pending.find(reinterpret_cast<uintptr_t>(user));
    if (it == g_pending.end()) return;
    const Pending p = std::move(it->second);
    g_pending.erase(it);
    if (p.gen != g_gen || !g_host || !g_enabled) return;
    const std::string bodyText = body && len ? std::string(static_cast<const char*>(body), len) : std::string();
    switch (p.kind) {
    case kThumb: handleThumb(p, status, body, len); break;
    case kMap: handleMap(p, status, body, len); break;
    case kSearch:
    case kRandom: {
        if (p.serial != g_serial) return; // a newer search superseded this one
        g_b.loading = false;
        std::string err = tmx::bridgeErrorText(status);
        if (!err.empty()) {
            if (status != HORIZON_BRIDGE_ERR_CANCELLED) g_b.err = err;
            return;
        }
        if (p.kind == kSearch) {
            tmx::SearchResult r;
            if (!tmx::parseSearch(bodyText, r, err)) {
                g_b.err = err;
                return;
            }
            if (p.page <= 1) g_b.results.clear();
            for (Track& t : r.tracks) {
                bool dup = false;
                for (const Track& o : g_b.results)
                    if (o.id == t.id) dup = true;
                if (!dup) g_b.results.push_back(std::move(t));
            }
            g_b.page = p.page;
            g_b.more = r.more;
            g_b.hasSearched = true;
        } else {
            Track t;
            bool found = false;
            if (!tmx::parseRandom(bodyText, t, found, err)) {
                g_b.err = err;
                return;
            }
            if (!found) {
                g_b.err = "No map matches these filters";
                return;
            }
            g_view[0].detail = true;
            g_view[0].t = std::move(t);
            g_msg.text.clear();
        }
        break;
    }
    }
}

// ---------------------------------------------------------------- thumbnails
void beginFrame() {
    const int f = ImGui::GetFrameCount();
    if (f == g_frame) return;
    g_frame = f;
    releaseDeferred();
    g_wanted.clear();
}

void wantThumb(const Track& t) {
    if (!t.thumbnail || g_noThumb.count(t.id) || g_thumbInflight.count(t.id) || g_thumbs.peek(t.id)) return;
    if (std::find(g_wanted.begin(), g_wanted.end(), t.id) == g_wanted.end()) g_wanted.push_back(t.id);
}

void pumpThumbs() {
    if (GetTickCount64() < g_thumbPause || !connected()) return;
    size_t i = 0;
    while (i < g_wanted.size() && static_cast<int>(g_thumbInflight.size()) < kMaxThumbsInFlight) {
        const int id = g_wanted[i++];
        if (g_thumbInflight.count(id) || g_thumbs.peek(id) || g_noThumb.count(id)) continue;
        Pending p;
        p.kind = kThumb;
        p.id = id;
        if (bridgeSend(p, tmx::thumbPath(g_site, id), nullptr)) g_thumbInflight.insert(id);
        else break; // busy or off: the cards ask again next frame
    }
    g_wanted.clear();
}

// ---------------------------------------------------------------- drawing helpers
float S() { return ck::uiScale(g_host); }

std::string fitText(ImFont* f, float fs, const std::string& text, float maxW) {
    if (ck::textSize(f, fs, text).x <= maxW) return text;
    std::string t = text;
    while (!t.empty()) {
        do t.pop_back();
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80);
        if (ck::textSize(f, fs, t + "...").x <= maxW) return t + "...";
    }
    return "...";
}

void drawStar(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool filled) {
    ImVec2 pts[10];
    for (int i = 0; i < 10; i++) {
        const float a = -1.5707963f + static_cast<float>(i) * 0.62831853f;
        const float rr = (i % 2 == 0) ? r : r * 0.42f;
        pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
    }
    if (filled) {
        for (int i = 0; i < 10; i++) dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], col);
        dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.0f);
    } else {
        dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, std::max(1.2f, r * 0.16f));
    }
}

ImVec4 difficultyColour(const ck::Palette& p, const std::string& d) {
    if (d == "Beginner") return p.ok;
    if (d == "Intermediate") return p.warn;
    if (d == "Expert") return p.accent2;
    if (d == "Lunatic") return p.bad;
    return p.text2;
}

float chipHeight(float fs, float s) { return fs * 0.82f + 5.0f * s; }

// Draws a rounded chip with the text in it; returns its width.
float drawChip(ImDrawList* dl, ImFont* f, float fs, ImVec2 pos, const std::string& text, ImU32 fill, ImU32 ink, float s, bool draw = true) {
    const float tfs = fs * 0.82f;
    const float padX = 6.0f * s;
    const ImVec2 ts = ck::textSize(f, tfs, text);
    const float w = ts.x + padX * 2.0f;
    const float h = chipHeight(fs, s);
    if (draw) {
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), fill, h * 0.5f);
        ck::text(dl, f, tfs, ImVec2(pos.x + padX, pos.y + (h - ts.y) * 0.5f), ink, text);
    }
    return w;
}

float cardHeight(float w, float fs, float s) {
    const float lh = fs * 1.2f;
    return w * 9.0f / 16.0f + 8.0f * s + lh + lh * 0.95f + 4.0f * s + lh + 5.0f * s + chipHeight(fs, s) + 8.0f * s;
}

void drawThumb(ImDrawList* dl, const ck::Palette& pal, const Track& t, ImVec2 p, ImVec2 size, float rounding, ImDrawFlags flags) {
    const Thumb* th = g_thumbs.get(t.id);
    if (th) {
        const float aspect = static_cast<float>(th->w) / static_cast<float>(th->h);
        const float target = size.x / size.y;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (aspect > target) {
            const float c = (1.0f - target / aspect) * 0.5f;
            uv0.x = c;
            uv1.x = 1.0f - c;
        } else {
            const float c = (1.0f - aspect / target) * 0.5f;
            uv0.y = c;
            uv1.y = 1.0f - c;
        }
        dl->AddImageRounded(static_cast<ImTextureID>(th->tex), p, p + size, uv0, uv1, IM_COL32_WHITE, rounding, flags);
        return;
    }
    dl->AddRectFilled(p, p + size, ck::u32(pal.bg2), rounding, flags);
    const bool none = !t.thumbnail || g_noThumb.count(t.id) != 0;
    const char* label = none ? "No preview" : "...";
    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const ImVec2 ts = ck::textSize(f, fs, label);
    ck::text(dl, f, fs, ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), ck::u32(pal.muted), label);
    wantThumb(t);
}

// One map card at the cursor position. Returns 1 when opened, 2 when the star was clicked, else 0.
int drawCard(const ck::Palette& pal, const Track& t, float w, float h) {
    const float s = S();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(w, h);
    if (!ImGui::IsRectVisible(p, p + size)) return 0;
    ImGui::PushID(t.id);
    ImGui::SetNextItemAllowOverlap();
    const bool open = ImGui::InvisibleButton("card", size);
    const bool hovered = ImGui::IsItemHovered();
    const float rad = 12.0f * s;
    dl->AddRectFilled(p, p + size, ck::u32(pal.surface), rad);
    const float thumbH = w * 9.0f / 16.0f;
    drawThumb(dl, pal, t, p, ImVec2(w, thumbH), rad, ImDrawFlags_RoundCornersTop);
    dl->AddRect(p, p + size, hovered ? ck::u32(pal.accent2, 0.9f) : IM_COL32(255, 255, 255, 22), rad, 0, hovered ? 1.6f : 1.0f);

    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float lh = fs * 1.2f;
    const float pad = 8.0f * s;
    float y = p.y + thumbH + pad;
    const float inner = w - pad * 2.0f;
    dl->PushClipRect(p, p + size, true);
    ck::text(dl, f, fs, ImVec2(p.x + pad, y), ck::u32(pal.text), fitText(f, fs, t.name, inner));
    y += lh;
    ck::text(dl, f, fs * 0.92f, ImVec2(p.x + pad, y), ck::u32(pal.muted), fitText(f, fs * 0.92f, t.author, inner));
    y += lh * 0.95f + 4.0f * s;
    // awards (star + count) left, author time right
    drawStar(dl, ImVec2(p.x + pad + fs * 0.4f, y + lh * 0.5f), fs * 0.42f, ck::u32(pal.gold), true);
    ck::text(dl, f, fs, ImVec2(p.x + pad + fs * 1.0f, y), ck::u32(pal.text2), std::to_string(t.awards));
    const std::string tm = ck::fmtTime(t.authorTime);
    const ImVec2 tms = ck::textSize(f, fs, tm);
    ck::text(dl, f, fs, ImVec2(p.x + w - pad - tms.x, y), ck::u32(pal.text2), tm);
    y += lh + 5.0f * s;
    // chips: difficulty, then styles while they fit
    float x = p.x + pad;
    const float xEnd = p.x + w - pad;
    if (!t.difficulty.empty()) {
        const ImVec4 dc = difficultyColour(pal, t.difficulty);
        x += drawChip(dl, f, fs, ImVec2(x, y), t.difficulty, ck::u32(dc, 0.20f), ck::u32(dc), s) + 4.0f * s;
    }
    for (const std::string& tag : t.tags) {
        const float cw = drawChip(dl, f, fs, ImVec2(x, y), tag, 0, 0, s, false);
        if (x + cw > xEnd) break;
        drawChip(dl, f, fs, ImVec2(x, y), tag, IM_COL32(255, 255, 255, 20), ck::u32(pal.text2), s);
        x += cw + 4.0f * s;
    }
    dl->PopClipRect();

    // favourite star on the thumbnail
    const float sb = 26.0f * s;
    const ImVec2 sp(p.x + w - sb - 4.0f * s, p.y + 4.0f * s);
    ImGui::SetCursorScreenPos(sp);
    int result = open ? 1 : 0;
    if (ImGui::InvisibleButton("fav", ImVec2(sb, sb))) result = 2;
    const bool starHot = ImGui::IsItemHovered();
    if (starHot) ImGui::SetTooltip("Favourite");
    const bool fav = isFav(t.id);
    const ImVec2 sc(sp.x + sb * 0.5f, sp.y + sb * 0.5f);
    dl->AddCircleFilled(sc, sb * 0.5f, IM_COL32(0, 0, 0, starHot ? 170 : 115));
    drawStar(dl, sc, sb * 0.30f, fav ? ck::u32(pal.accent2) : IM_COL32(255, 255, 255, starHot ? 255 : 210), fav);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(p);
    return result;
}

// Grid of cards. Returns the index to open (or -1); toggles favourites itself.
int drawGrid(const ck::Palette& pal, const std::vector<const Track*>& list) {
    const float s = S();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 10.0f * s;
    const float want = (g_large ? 260.0f : 176.0f) * s;
    const int cols = std::max(1, static_cast<int>((avail + gap) / (want + gap)));
    const float cw = (avail - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    const float ch = cardHeight(cw, ImGui::GetFontSize(), s);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    int opened = -1;
    for (size_t i = 0; i < list.size(); i++) {
        const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
        ImGui::SetCursorScreenPos(ImVec2(start.x + static_cast<float>(col) * (cw + gap), start.y + static_cast<float>(row) * (ch + gap)));
        const int r = drawCard(pal, *list[i], cw, ch);
        if (r == 1) opened = static_cast<int>(i);
        else if (r == 2) toggleFav(*list[i]);
    }
    const int rows = (static_cast<int>(list.size()) + cols - 1) / cols;
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(avail, static_cast<float>(rows) * (ch + gap)));
    return opened;
}

void gridSizeCombo(float width) {
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##grid", g_large ? "Large cards" : "Small cards")) {
        if (ImGui::Selectable("Small cards", !g_large) && g_large) {
            g_large = false;
            g_host->set_int(g_host->plugin, "GridLarge", 0);
        }
        if (ImGui::Selectable("Large cards", g_large) && !g_large) {
            g_large = true;
            g_host->set_int(g_host->plugin, "GridLarge", 1);
        }
        ImGui::EndCombo();
    }
}

void msgLine(const ck::Palette& pal) {
    if (g_msg.text.empty()) return;
    if (GetTickCount64() - g_msg.at > 20000 && g_msg.kind != 0) return;
    const ImVec4 c = g_msg.kind == 1 ? pal.ok : g_msg.kind == 2 ? pal.warn : pal.muted;
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", g_msg.text.c_str());
    ImGui::PopStyleColor();
}

void chipFlow(const ck::Palette& pal, const std::vector<std::string>& tags) {
    const float s = S();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float ch = chipHeight(fs, s), gap = 4.0f * s;
    float x = 0, y = 0;
    for (const std::string& t : tags) {
        const float w = drawChip(dl, f, fs, p0, t, 0, 0, s, false);
        if (x + w > avail && x > 0) {
            x = 0;
            y += ch + gap;
        }
        drawChip(dl, f, fs, ImVec2(p0.x + x, p0.y + y), t, IM_COL32(255, 255, 255, 20), ck::u32(pal.text2), s);
        x += w + gap;
    }
    ImGui::Dummy(ImVec2(avail, y + ch));
}

void infoRow(const ck::Palette& pal, const char* label, const std::string& value) {
    const float s = S();
    ImGui::PushStyleColor(ImGuiCol_Text, pal.muted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(96.0f * s);
    ImGui::TextUnformatted(value.c_str());
}

void drawDetail(const ck::Palette& pal, View& v) {
    const float s = S();
    Track& t = v.t;
    if (ImGui::Button("Back")) {
        v.detail = false;
        return;
    }
    ImGui::SameLine();
    ImGui::PushFont(ck::font(g_host, 1));
    ImGui::TextUnformatted(t.name.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2.0f * s));

    const float avail = ImGui::GetContentRegionAvail().x;
    const bool wide = avail > 640.0f * s;
    const float tw = wide ? std::min(avail * 0.5f, 520.0f * s) : avail;
    const float th = tw * 9.0f / 16.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::BeginGroup();
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(tw, th));
        drawThumb(dl, pal, t, p, ImVec2(tw, th), 12.0f * s, 0);
        dl->AddRect(p, p + ImVec2(tw, th), IM_COL32(255, 255, 255, 22), 12.0f * s);
    }
    ImGui::EndGroup();
    if (wide) ImGui::SameLine(0, 16.0f * s);
    else ImGui::Dummy(ImVec2(0, 6.0f * s));
    ImGui::BeginGroup();
    infoRow(pal, "Author", t.author);
    infoRow(pal, "Awards", std::to_string(t.awards));
    infoRow(pal, "Difficulty", t.difficulty.empty() ? std::string("-") : t.difficulty);
    infoRow(pal, "Author time", ck::fmtTime(t.authorTime));
    if (!t.environment.empty()) infoRow(pal, "Environment", t.environment);
    if (t.uploaded.size() >= 10) infoRow(pal, "Uploaded", t.uploaded.substr(0, 10));
    infoRow(pal, "Map ID", std::to_string(t.id));
    if (!t.tags.empty()) {
        ImGui::Dummy(ImVec2(0, 4.0f * s));
        chipFlow(pal, t.tags);
    }
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0, 8.0f * s));
    const std::string playOff = playBlock();
    const std::string saveOff = saveBlock();
    const bool busy = g_mapBusy;
    const HorizonStyle* hs = g_host->style ? g_host->style() : nullptr;
    const ImVec4 ink = hs ? ck::v4(hs->accent_ink) : ImVec4(0.1f, 0.04f, 0.01f, 1.0f);
    ImGui::BeginDisabled(!playOff.empty() || busy);
    ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal.accent2);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, pal.accent3);
    ImGui::PushStyleColor(ImGuiCol_Text, ink);
    if (ImGui::Button("Play", ImVec2(96.0f * s, 0))) doMap(t, true);
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!saveOff.empty() || busy);
    if (ImGui::Button("Save", ImVec2(80.0f * s, 0))) doMap(t, false);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Download without playing.");
    ImGui::SameLine();
    const bool fav = isFav(t.id);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float fs = ImGui::GetFontSize();
        const char* label = fav ? "Unfavourite" : "Favourite";
        const float bw = fs * 1.9f + ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::Button("##fav", ImVec2(bw, 0))) toggleFav(t);
        const float bh = ImGui::GetItemRectSize().y;
        drawStar(dl, ImVec2(p.x + fs * 1.0f, p.y + bh * 0.5f), fs * 0.5f, fav ? ck::u32(pal.accent2) : ck::u32(pal.text2), fav);
        ck::text(dl, ImGui::GetFont(), fs, ImVec2(p.x + fs * 1.9f, p.y + (bh - fs) * 0.5f), ck::u32(pal.text), label);
    }
    if (!playOff.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.warn);
        ImGui::TextWrapped("%s", playOff.c_str());
        ImGui::PopStyleColor();
    }
    msgLine(pal);

    ImGui::Dummy(ImVec2(0, 6.0f * s));
    ImGui::PushStyleColor(ImGuiCol_Text, pal.muted);
    ImGui::TextUnformatted("Open on TMX");
    ImGui::PopStyleColor();
    std::string url = tmx::trackUrl(g_site, t.id);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", url.c_str());
    ImGui::SetNextItemWidth(std::max(120.0f * s, std::min(avail - 80.0f * s, 520.0f * s)));
    ImGui::InputText("##url", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button("Copy")) ImGui::SetClipboardText(url.c_str());
}

void bridgeLine(const ck::Palette& pal) {
    const float s = S();
    const int st = g_host->bridge_state();
    const ImVec4 c = st == HORIZON_BRIDGE_CONNECTED ? pal.ok : st == HORIZON_BRIDGE_PAIRING ? pal.warn : pal.muted;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float fs = ImGui::GetFontSize();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 5.0f * s, p.y + fs * 0.5f + 1.0f), 4.0f * s, ck::u32(c));
    ImGui::Dummy(ImVec2(14.0f * s, fs));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, st == HORIZON_BRIDGE_CONNECTED ? pal.text2 : c);
    ImGui::TextUnformatted(tmx::bridgeStateText(st).c_str());
    ImGui::PopStyleColor();
}

void drawForm(const ck::Palette& pal) {
    const float s = S();
    tmx::Form& f = g_b.form;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    bool changed = false, go = false;

    // row 1: name, author
    const float w1 = (avail - gap) * 0.6f;
    ImGui::SetNextItemWidth(w1);
    if (ImGui::InputTextWithHint("##name", "Map name", g_b.nameBuf, sizeof(g_b.nameBuf), ImGuiInputTextFlags_EnterReturnsTrue)) go = true;
    if (ImGui::IsItemEdited() || go) {
        f.name = g_b.nameBuf;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(avail - w1 - gap);
    if (ImGui::InputTextWithHint("##author", "Author", g_b.authorBuf, sizeof(g_b.authorBuf), ImGuiInputTextFlags_EnterReturnsTrue)) go = true;
    if (ImGui::IsItemEdited() || go) {
        f.author = g_b.authorBuf;
        changed = true;
    }

    // row 2: style, difficulty, length, awards
    const float rest = avail - gap * 3.0f;
    {
        const float w = rest * 0.28f;
        const std::string sum = f.tagMask ? tmx::tagSummary(f.tagMask) : std::string("any");
        const std::string lbl = fitText(ImGui::GetFont(), ImGui::GetFontSize(), "Style: " + sum, w - 16.0f * s);
        if (ImGui::Button((lbl + "##style").c_str(), ImVec2(w, 0))) ImGui::OpenPopup("styles");
        if (ImGui::BeginPopup("styles")) {
            for (int i = 0; i < tmx::kTagCount; i++) {
                bool on = (f.tagMask >> tmx::kTags[i].id) & 1u;
                if (ImGui::Checkbox(tmx::kTags[i].name, &on)) {
                    f.tagMask = on ? (f.tagMask | (1u << tmx::kTags[i].id)) : (f.tagMask & ~(1u << tmx::kTags[i].id));
                    changed = true;
                }
                if (i % 3 != 2 && i + 1 < tmx::kTagCount) ImGui::SameLine(static_cast<float>(i % 3 + 1) * 130.0f * s);
            }
            if (ImGui::SmallButton("Clear")) {
                f.tagMask = 0;
                changed = true;
            }
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(rest * 0.24f);
    if (ImGui::BeginCombo("##diff", f.difficulty < 0 ? "Any difficulty" : tmx::kDifficulties[f.difficulty])) {
        if (ImGui::Selectable("Any difficulty", f.difficulty < 0)) {
            f.difficulty = -1;
            changed = true;
        }
        for (int i = 0; i < 4; i++)
            if (ImGui::Selectable(tmx::kDifficulties[i], f.difficulty == i)) {
                f.difficulty = i;
                changed = true;
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(rest * 0.24f);
    if (ImGui::BeginCombo("##len", tmx::kLengths[f.length].label)) {
        for (int i = 0; i < tmx::kLengthCount; i++)
            if (ImGui::Selectable(tmx::kLengths[i].label, f.length == i)) {
                f.length = i;
                changed = true;
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Author time.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(avail - (rest * 0.76f + gap * 3.0f));
    if (ImGui::DragInt("##awards", &f.awards, 0.25f, 0, 9999, "Awards: %d+")) changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimum number of awards.");

    // row 3: buttons
    const bool off = !connected();
    ImGui::BeginDisabled(off);
    if (ImGui::Button("Search", ImVec2(90.0f * s, 0))) go = true;
    ImGui::SameLine();
    if (ImGui::Button("Random", ImVec2(90.0f * s, 0))) {
        saveForm();
        doRandom();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("A random map that fits the filters.");
    ImGui::EndDisabled();
    ImGui::SameLine(avail - 130.0f * s);
    gridSizeCombo(130.0f * s);
    (void)pal;

    if (changed) saveForm();
    if (go) doSearch(1);
}

void drawResults(const ck::Palette& pal) {
    ImGui::Dummy(ImVec2(0, 4.0f * S()));
    if (!g_b.err.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.muted);
        ImGui::TextWrapped("%s", g_b.err.c_str());
        ImGui::PopStyleColor();
    }
    if (g_b.loading) ck::hint(pal, "Searching...");
    if (g_b.results.empty()) {
        if (!g_b.loading && g_b.hasSearched) ck::hint(pal, "No maps found.");
        else if (!g_b.loading && !g_b.hasSearched && g_b.err.empty()) ck::hint(pal, "Search TMX or try a random map.");
        return;
    }
    std::vector<const Track*> list;
    for (const Track& t : g_b.results) list.push_back(&t);
    const int open = drawGrid(pal, list);
    if (open >= 0) {
        g_view[0].detail = true;
        g_view[0].t = g_b.results[static_cast<size_t>(open)];
        g_msg.text.clear();
    }
    if (g_b.more) {
        ImGui::BeginDisabled(g_b.loading || !connected());
        if (ImGui::Button("More", ImVec2(110.0f * S(), 0))) doSearch(g_b.page + 1);
        ImGui::EndDisabled();
    }
}

void HORIZON_CALL drawBrowse(void*) {
    if (!g_enabled || !g_imgui || !g_host) return;
    beginFrame();
    const ck::Palette pal = ck::palette(g_host);
    bridgeLine(pal);
    ImGui::Dummy(ImVec2(0, 2.0f * S()));
    if (g_view[0].detail) {
        drawDetail(pal, g_view[0]);
    } else {
        drawForm(pal);
        drawResults(pal);
    }
    pumpThumbs();
}

void HORIZON_CALL drawFavs(void*) {
    if (!g_enabled || !g_imgui || !g_host) return;
    beginFrame();
    const ck::Palette pal = ck::palette(g_host);
    if (g_view[1].detail) {
        drawDetail(pal, g_view[1]);
    } else {
        const std::vector<const Track*> list = favTracks();
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, pal.muted);
        ImGui::Text("%d favourite%s", static_cast<int>(list.size()), list.size() == 1 ? "" : "s");
        ImGui::PopStyleColor();
        ImGui::SameLine(avail - 130.0f * S());
        gridSizeCombo(130.0f * S());
        ImGui::Dummy(ImVec2(0, 4.0f * S()));
        if (list.empty()) {
            ck::hint(pal, "No favourites yet. Click the star on a map.");
        } else {
            const int open = drawGrid(pal, list);
            if (open >= 0) {
                g_view[1].detail = true;
                g_view[1].t = *list[static_cast<size_t>(open)];
                g_msg.text.clear();
            }
        }
    }
    pumpThumbs();
}

void resetState() {
    g_pending.clear();
    g_gen++;
    g_b.results.clear();
    g_b.err.clear();
    g_b.loading = false;
    g_b.hasSearched = false;
    g_b.more = false;
    g_b.page = 0;
    g_view[0] = View();
    g_view[1] = View();
    g_mapBusy = false;
    g_msg = Msg();
    g_serial = 0;
    g_frame = -1;
    g_thumbPause = 0;
    releaseAllTextures();
}

// ---------------------------------------------------------------- test hook
std::string activeStateText() {
    char buf[900];
    const std::string pb = g_host ? playBlock() : std::string("no host");
    snprintf(buf, sizeof(buf),
             "game=%s bridge=%d results=%d more=%d page=%d loading=%d thumbs=%d inflight=%d noThumb=%d created=%d released=%d "
             "detail=%s:%d favs=%d tab=%s playblock=\"%s\" msg=\"%s\" err=\"%s\" file=\"%s\"",
             g_site.c_str(), g_host && g_host->bridge_state ? g_host->bridge_state() : -1, static_cast<int>(g_b.results.size()),
             g_b.more ? 1 : 0, g_b.page, g_b.loading ? 1 : 0, static_cast<int>(g_thumbs.size()), static_cast<int>(g_thumbInflight.size()),
             static_cast<int>(g_noThumb.size()), g_created, g_released, g_view[g_tab].detail ? (g_tab == 0 ? "browse" : "favourites") : "none",
             g_view[g_tab].detail ? g_view[g_tab].t.id : 0, static_cast<int>(favTracks().size()), g_tab == 0 ? "browse" : "favourites",
             pb.c_str(), g_msg.text.c_str(), g_b.err.c_str(), g_lastFile.c_str());
    return buf;
}

View* activeView() {
    if (g_view[g_tab].detail) return &g_view[g_tab];
    if (g_view[1 - g_tab].detail) return &g_view[1 - g_tab];
    return nullptr;
}

int hookOut(char* out, int32_t outSize, const std::string& text, int code) {
    if (out && outSize > 0) {
        const size_t n = std::min(text.size(), static_cast<size_t>(outSize - 1));
        memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return code;
}

} // namespace

// ---------------------------------------------------------------- exports -----------------------
static const HorizonPluginInfo kInfo = {
    HORIZON_ABI,
    "tmx-browser",
    "TMX Browser",
    kVersion,
    HORIZON_GAME_ALL,
    HORIZON_CAP_GAME_CONTROL,
    HORIZON_FLAG_ONLINE_OK | HORIZON_FLAG_LIVE_TOGGLE,
    "cheatoskar",
    "Search the TrackMania Exchange, preview maps and play them from the game.",
    HORIZON_ABI_MINOR,
};

extern "C" {

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) { return &kInfo; }

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    if (!host || host->abi != HORIZON_ABI) return 1;
    if (!ck::abiAtLeast(host, 3) || !host->register_panel || !host->bridge_request) {
        if (host->log) host->log(host->plugin, HORIZON_LOG_WARN, "needs a host with ABI 1.3 (panels and the bridge)");
        return 1;
    }
    g_host = host;
    g_imgui = horizon_imgui_attach(host);
    if (!g_imgui) {
        host->log(host->plugin, HORIZON_LOG_WARN, "ImGui attach failed, no panels");
        g_host = nullptr;
        return 1;
    }
    g_site = ck::siteCode(host);
    if (g_site.empty()) {
        g_host = nullptr;
        return 1;
    }
    g_forever = ck::isForever(host);
    resetState();
    g_enabled = true;
    g_lastFile.clear();
    g_created = g_released = 0;
    loadSettings();
    loadFavs();

    HorizonPanelDef d = {};
    d.size = sizeof(d);
    d.key = "browse";
    d.title = "Browse";
    d.icon = HZ_ICON_SEARCH;
    d.flags = HORIZON_PANEL_MENU_PAGE;
    d.draw = &drawBrowse;
    d.default_x = 0.15f;
    d.default_y = 0.10f;
    d.default_w = 0.70f;
    d.default_h = 0.75f;
    host->register_panel(host->plugin, &d);
    d.key = "favourites";
    d.title = "Favourites";
    d.icon = HZ_ICON_PIN;
    d.draw = &drawFavs;
    host->register_panel(host->plugin, &d);

    host->log(host->plugin, HORIZON_LOG_INFO, "---- tmx-browser %s start (site code %s, %d favourites) ----", kVersion, g_site.c_str(),
              static_cast<int>(g_favs.size()));
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    if (g_host) {
        if (g_host->unregister_panel) {
            g_host->unregister_panel(g_host->plugin, "browse");
            g_host->unregister_panel(g_host->plugin, "favourites");
        }
        g_enabled = false;
        resetState(); // releases every texture while g_host is still valid
    }
    g_host = nullptr;
    g_imgui = false;
    g_enabled = false;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_SetEnabled(int32_t enabled) {
    if (!g_host) return;
    if (enabled) {
        g_enabled = true;
    } else {
        g_enabled = false;
        resetState();
    }
}

// Test hook for the fake-host tests and the preview screenshots. cmd is one line, out gets a short state text.
//   search [text]   set the name filter (when given), run the search, page 1     more            next page
//   random          random map (opens its detail)         select N / favselect N   open result N / favourite N
//   back            close the detail of the active tab    tab browse|favourites    which panel play/save/fav act on first
//   play / save     download (and start) the open map     fav [N]                  toggle the open map / result N
//   form k=v ...    name author tags(3,5) diff len awards  grid small|large        card size
//   state           the state text only
HORIZON_EXPORT int32_t __cdecl HorizonClient_TestHook(const char* cmd, char* out, int32_t outSize) {
    if (!cmd) return hookOut(out, outSize, "error: no command", -1);
    std::string line = cmd;
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\n')) line.pop_back();
    const size_t sp = line.find(' ');
    const std::string c = line.substr(0, sp);
    if (!g_host && c != "state") return hookOut(out, outSize, "error: not running", -1); // "state" also works after Shutdown
    const std::string arg = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    const int n = atoi(arg.c_str());
    if (c == "state") {
        // nothing to do
    } else if (c == "search") {
        if (!arg.empty()) {
            g_b.form.name = arg;
            syncBuffers();
        }
        saveForm();
        doSearch(1);
    } else if (c == "more") {
        doSearch(g_b.page + 1);
    } else if (c == "random") {
        doRandom();
    } else if (c == "select" || c == "favselect") {
        if (c == "select") {
            if (n < 1 || n > static_cast<int>(g_b.results.size())) return hookOut(out, outSize, "error: no such result", -1);
            g_view[0].detail = true;
            g_view[0].t = g_b.results[static_cast<size_t>(n - 1)];
            g_tab = 0;
        } else {
            const std::vector<const Track*> fl = favTracks();
            if (n < 1 || n > static_cast<int>(fl.size())) return hookOut(out, outSize, "error: no such favourite", -1);
            g_view[1].detail = true;
            g_view[1].t = *fl[static_cast<size_t>(n - 1)];
            g_tab = 1;
        }
        g_msg.text.clear();
    } else if (c == "back") {
        g_view[g_tab].detail = false;
    } else if (c == "tab") {
        g_tab = arg == "favourites" || arg == "favs" ? 1 : 0;
    } else if (c == "play" || c == "save") {
        View* v = activeView();
        if (!v) return hookOut(out, outSize, "error: no map open", -1);
        doMap(v->t, c == "play");
    } else if (c == "fav") {
        if (n >= 1) {
            if (n > static_cast<int>(g_b.results.size())) return hookOut(out, outSize, "error: no such result", -1);
            toggleFav(g_b.results[static_cast<size_t>(n - 1)]);
        } else {
            View* v = activeView();
            if (!v) return hookOut(out, outSize, "error: no map open", -1);
            toggleFav(v->t);
        }
    } else if (c == "grid") {
        g_large = arg == "large";
        g_host->set_int(g_host->plugin, "GridLarge", g_large ? 1 : 0);
    } else if (c == "form") {
        tmx::Form& f = g_b.form;
        size_t i = 0;
        while (i < arg.size()) {
            size_t e = arg.find(' ', i);
            if (e == std::string::npos) e = arg.size();
            const std::string kv = arg.substr(i, e - i);
            i = e + 1;
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "name") f.name = v;
            else if (k == "author") f.author = v;
            else if (k == "diff") f.difficulty = std::max(-1, std::min(3, atoi(v.c_str())));
            else if (k == "len") f.length = std::max(0, std::min(tmx::kLengthCount - 1, atoi(v.c_str())));
            else if (k == "awards") f.awards = std::max(0, atoi(v.c_str()));
            else if (k == "tags") {
                f.tagMask = 0;
                size_t j = 0;
                while (j < v.size()) {
                    size_t ce = v.find(',', j);
                    if (ce == std::string::npos) ce = v.size();
                    const int id = atoi(v.substr(j, ce - j).c_str());
                    if (id >= 0 && id < 32) f.tagMask |= 1u << id;
                    j = ce + 1;
                }
            }
        }
        syncBuffers();
        saveForm();
    } else {
        return hookOut(out, outSize, "error: unknown command", -1);
    }
    return hookOut(out, outSize, activeStateText(), 0);
}

} // extern "C"
