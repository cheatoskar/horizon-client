// Statistics lines for TM Horizon (see stats.h). The map's uid comes from the host's map info when the
// map was started from a file; a map picked in the game's own menus has none there, so it is read from
// the engine's id table (plugins/client-common/mapident: plain reads, no game code is called).
#include "host/stats.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "host/host.h"
#include "host/plugins.h"
#include "host/stats_log.h"
#include "../../plugins/client-common/mapident.h" // shared with the client mods

namespace horizon {
namespace {

// Reads that never fault (the game's memory, read-only).
struct HostMem : ck::Mem {
    bool read(uintptr_t addr, void* out, size_t n) override {
        if (addr < 0x10000) return false;
        __try {
            memcpy(out, reinterpret_cast<const void*>(addr), n);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    bool isKind(uintptr_t, const char*) override { return true; }
};

std::wstring g_dir;
bool g_on = false;
stats::Recorder g_rec;
std::string g_uid, g_name; // the current map
// the id table, looked up once
bool g_idsTried = false;
ck::IdTable g_ids;
int g_uidOffset = -1;

std::mutex g_lock;
std::condition_variable g_wake;
std::deque<std::pair<std::string, std::string>> g_queue; // file name, text
bool g_stop = false;
std::thread g_worker;
int g_failures = 0;

void workerLoop() {
    std::unique_lock<std::mutex> lk(g_lock);
    for (;;) {
        g_wake.wait(lk, [] { return g_stop || !g_queue.empty(); });
        while (!g_queue.empty()) {
            const auto item = g_queue.front();
            g_queue.pop_front();
            lk.unlock();
            const bool ok = stats::append(g_dir, item.first, item.second);
            lk.lock();
            if (!ok && g_failures++ < 3) HWARN("stats: could not append to %s\\%s", utf8(g_dir).c_str(), item.first.c_str());
        }
        if (g_stop) return;
    }
}

std::string uidFromEngine(void* challenge) {
    if (!challenge) return std::string();
    HostMem m;
    if (!g_idsTried) {
        g_idsTried = true;
        const uintptr_t base = state().game.base;
        const size_t size = base ? ck::imageSize(m, base) : 0;
        if (!size || !ck::findIdTable(m, base, size, &g_ids)) HLOG("stats: map uids only for maps started from a file (no id table)");
        if (state().game.game & HORIZON_GAME_FOREVER) g_uidOffset = 0xdc; // TMNF/TMUF (100 TMX Bingo), else scanned
    }
    if (!g_ids.ok()) return std::string();
    return ck::challengeUid(m, g_ids, reinterpret_cast<uintptr_t>(challenge), &g_uidOffset);
}

} // namespace

bool statsActive() { return g_on; }

void statsStart() {
    const Profile& p = profile();
    if (!p.valid || p.statsDir.empty()) return;
    if (state().safeMode) {
        HLOG("stats: safe mode, nothing is written");
        return;
    }
    g_dir = wide(p.statsDir);
    g_stop = false;
    g_worker = std::thread(workerLoop);
    g_on = true;
    HLOG("stats: play events go to %s", p.statsDir.c_str());
}

void statsEvent(const HorizonEvent& e) {
    if (!g_on) return;
    if (e.type != HORIZON_EVENT_MAP_LOADED && e.type != HORIZON_EVENT_RACE_START && e.type != HORIZON_EVENT_RESPAWN && e.type != HORIZON_EVENT_FINISH)
        return;
    std::string author, env;
    if (e.type == HORIZON_EVENT_MAP_LOADED) {
        g_uid = e.map && e.map->uid ? e.map->uid : "";
        g_name = e.map && e.map->name ? e.map->name : "";
        if (g_uid.empty()) g_uid = uidFromEngine(e.engine_challenge);
        if (e.map && e.map->author) author = e.map->author;
        if (e.map && e.map->environment) env = e.map->environment;
    }
    const int64_t t = stats::nowMs();
    std::string line = g_rec.line(e, g_uid, g_name, author, env, t);
    if (line.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_queue.size() < 256) g_queue.emplace_back(stats::fileName(t), std::move(line));
    }
    g_wake.notify_one();
}

void statsStop() {
    if (!g_on) return;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_stop = true;
    }
    g_wake.notify_one();
    if (g_worker.joinable()) g_worker.join();
    g_on = false;
}

} // namespace horizon
