// The TM Horizon bridge (ABI minor 3, docs/client-bridge.md): one loopback HTTP connection from the
// game to the TM Horizon launcher on this PC. Two threads of the host:
//   - control: finds the launcher (ports 27331-27335), pairs (the player presses Allow in TM Horizon),
//     then long-polls /v1/client/events and turns them into toasts;
//   - requests: sends the plugins' bridge_request calls one after the other.
// Answers go back to the plugins on the game thread (bridgeDeliver, end of a frame). The only thing that
// touches the game is a play_map event (ESWC/Sunrise): checked here, started from bridgeDeliver with the same
// rules as the plugins' play_map_file. The key is kept in host.json ("bridge.key"), written from the game thread.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "host/bridge.h"

#include <bcrypt.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "imgui.h"
#include "horizon_imgui.h"
#include "common/util.h"
#include "host/bridge_logic.h"
#include "host/plugins.h"
#include "host/toasts.h"

namespace horizon {
namespace {

struct Job {
    Plugin* plugin = nullptr;
    std::string method, path, body;
    HorizonBridgeCallback cb = nullptr;
    void* user = nullptr;
    bridge::HttpAnswer answer;
    int error = 0; // HORIZON_BRIDGE_ERR_*
};

std::mutex g_lock;
std::condition_variable g_cv;
std::deque<Job> g_queue, g_done;
std::thread g_control, g_worker;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_enabled{true};
std::atomic<int> g_state{HORIZON_BRIDGE_SEARCHING};
std::atomic<uint16_t> g_port{0};
std::string g_key;          // under g_lock
bool g_keyDirty = false;    // under g_lock: the game thread writes it to host.json
std::string g_instance;
std::string g_pairBody;
bool g_started = false;
std::atomic<SOCKET> g_longPoll{INVALID_SOCKET};
std::vector<std::pair<Plugin*, bridge::RateLimit>> g_limits; // game thread only
std::string g_site;         // this game's site code ("tmn", ...)
bool g_playMap = false;     // play_map advertised and run (ESWC, Sunrise)
struct PlayRequest {
    long long id = 0;
    std::string title, rel; // rel: checked, relative to GameData\Tracks, backslashes
};
std::deque<PlayRequest> g_play; // under g_lock; only the newest is kept

long long nowEpochMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const unsigned long long t = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<long long>(t / 10000ull) - 11644473600000ll;
}

void toastPlay(uint32_t kind, const std::string& title, const char* text) {
    HorizonToast t = {};
    t.size = sizeof(t);
    t.kind = kind;
    t.title = title.c_str();
    t.text = text;
    t.icon = HZ_ICON_PLAY;
    toastsPush("TM Horizon", t);
}

// Control thread: a play_map event. Checks what needs no game, queues the rest for the game thread.
void onPlayMap(const bridge::Event& e) {
    if (!bridge::playMapFor(e, g_site.c_str())) {
        logLine(HORIZON_LOG_INFO, "host", "bridge: play_map %lld for \"%s\" ignored (this game is \"%s\")", e.id, e.game.c_str(), g_site.c_str());
        return;
    }
    if (!g_playMap) {
        logLine(HORIZON_LOG_INFO, "host", "bridge: play_map %lld ignored: not run on this game", e.id);
        return;
    }
    if (!bridge::playMapFresh(e.at, nowEpochMs())) {
        logLine(HORIZON_LOG_INFO, "host", "bridge: play_map %lld ignored: older than %lld s", e.id, bridge::kPlayMapMaxAgeMs / 1000);
        return;
    }
    const std::string rel = bridge::mapRelPath(e.path);
    const std::string title = e.title.empty() ? std::string("Map") : e.title;
    if (rel.empty()) {
        logLine(HORIZON_LOG_WARN, "host", "bridge: play_map %lld refused: bad path \"%s\"", e.id, e.path.c_str());
        toastPlay(HORIZON_TOAST_WARN, title, "Could not open: bad map path.");
        return;
    }
    logLine(HORIZON_LOG_INFO, "host", "bridge: play_map %lld \"%s\" (%s) queued", e.id, title.c_str(), rel.c_str());
    std::lock_guard<std::mutex> lock(g_lock);
    g_play.clear();
    g_play.push_back(PlayRequest{e.id, title, rel});
}

// Game thread: the queued play_map, same rules as play_map_file (main menu, not online).
void runPlayMap(const PlayRequest& r) {
    const char* why = nullptr;
    HorizonResult res = HORIZON_ERR_FAILED;
    const std::wstring root = state().game.exeFolder + L"GameData\\Tracks\\";
    wchar_t rootFull[MAX_PATH] = {}, full[MAX_PATH] = {};
    const std::wstring joined = root + wide(r.rel);
    const DWORD nr = GetFullPathNameW(root.c_str(), MAX_PATH, rootFull, nullptr);
    const DWORD nf = GetFullPathNameW(joined.c_str(), MAX_PATH, full, nullptr);
    const DWORD attr = nf && nf < MAX_PATH ? GetFileAttributesW(full) : INVALID_FILE_ATTRIBUTES;
    if (!nr || nr >= MAX_PATH || !nf || nf >= MAX_PATH || _wcsnicmp(full, rootFull, wcslen(rootFull)) != 0) {
        why = "Could not open: bad map path.";
    } else if (attr == INVALID_FILE_ATTRIBUTES || (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        why = "Could not open: map file not found.";
    } else if (isOnlineNow() || runGate()) {
        res = HORIZON_ERR_BLOCKED_ONLINE;
        why = "Leave the online game first.";
    } else {
        res = game().playMapFile(utf8(full).c_str(), 0);
        if (res == HORIZON_ERR_WRONG_STATE) why = "Go to the main menu first.";
        else if (res == HORIZON_ERR_BLOCKED_ONLINE) why = "Leave the online game first.";
        else if (res == HORIZON_ERR_NOT_SUPPORTED) why = "Not supported in this game.";
        else if (res != HORIZON_OK) why = "Could not open the map.";
    }
    logLine(why ? HORIZON_LOG_WARN : HORIZON_LOG_INFO, "host", "bridge: play_map %lld %s -> %d%s%s", r.id, utf8(full).c_str(), res,
            why ? ", " : "", why ? why : "");
    if (why) toastPlay(HORIZON_TOAST_WARN, r.title, why);
    else toastPlay(HORIZON_TOAST_OK, r.title, "Loading map.");
}

std::string currentKey() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_key;
}
void setKey(const std::string& k) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_key == k) return;
    g_key = k;
    g_keyDirty = true;
}

// One request, Connection: close. track: the socket is published so bridgeStop can break a long poll.
bridge::HttpAnswer call(uint16_t port, const char* method, const std::string& path, const std::string& key, const std::string& body,
                        int timeoutMs, bool track) {
    bridge::HttpAnswer a;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return a;
    DWORD to = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (track) g_longPoll = s;
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (track) g_longPoll = INVALID_SOCKET;
        closesocket(s);
        return a;
    }
    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                      "\r\nConnection: close\r\nUser-Agent: HorizonHost/" + kHostVersion + "\r\n";
    if (!key.empty()) req += "X-Horizon-Key: " + key + "\r\n";
    if (!body.empty() || strcmp(method, "POST") == 0)
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    size_t sent = 0;
    while (sent < req.size()) {
        const int n = send(s, req.data() + sent, static_cast<int>(req.size() - sent), 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    std::string raw;
    if (sent == req.size()) {
        char buf[16384];
        for (;;) {
            const int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, static_cast<size_t>(n));
            if (raw.size() > bridge::kMaxBody + 65536) {
                raw.clear();
                a.status = -4;
                break;
            }
        }
    }
    if (track) g_longPoll = INVALID_SOCKET;
    closesocket(s);
    if (a.status == -4) return a;
    bridge::HttpAnswer parsed;
    if (bridge::parseAnswer(raw, &parsed)) return parsed;
    return a;
}

uint16_t findLauncher() {
    for (int i = 0; i < bridge::kPortCount && !g_stop; i++) {
        const uint16_t port = static_cast<uint16_t>(bridge::kFirstPort + i);
        const bridge::HttpAnswer a = call(port, "GET", "/v1/client/ping", "", "", 1500, false);
        if (a.status == 200 && a.body.find("\"tm-horizon\"") != std::string::npos) return port;
    }
    return 0;
}

void sleepFor(int ms) {
    std::unique_lock<std::mutex> lock(g_lock);
    g_cv.wait_for(lock, std::chrono::milliseconds(ms), [] { return g_stop.load(); });
}

void controlLoop() {
    long long last = -1;
    bool haveCursor = false;
    bool announced = false;
    while (!g_stop) {
        if (!g_enabled) {
            g_state = HORIZON_BRIDGE_OFF;
            g_port = 0;
            sleepFor(500);
            continue;
        }
        if (!g_port) {
            if (g_state != HORIZON_BRIDGE_DENIED) g_state = HORIZON_BRIDGE_SEARCHING;
            g_port = findLauncher();
            if (!g_port) {
                sleepFor(10000);
                continue;
            }
            logLine(HORIZON_LOG_INFO, "host", "bridge: TM Horizon answers on 127.0.0.1:%u", g_port.load());
        }
        std::string key = currentKey();
        if (key.empty()) {
            const bridge::HttpAnswer a = call(g_port, "POST", "/v1/client/pair", "", g_pairBody, 5000, false);
            if (a.status != 200) {
                g_port = 0;
                sleepFor(3000);
                continue;
            }
            std::string k;
            const std::string st = bridge::parsePair(a.body, &k);
            if (st == "ok") {
                setKey(k);
                logLine(HORIZON_LOG_INFO, "host", "bridge: paired with TM Horizon");
                continue;
            }
            if (st == "denied") {
                if (g_state != HORIZON_BRIDGE_DENIED) logLine(HORIZON_LOG_INFO, "host", "bridge: TM Horizon said No; asking again in a minute");
                g_state = HORIZON_BRIDGE_DENIED;
                sleepFor(60000);
                continue;
            }
            g_state = HORIZON_BRIDGE_PAIRING;
            sleepFor(2000);
            continue;
        }
        g_state = HORIZON_BRIDGE_CONNECTED;
        if (!announced) {
            announced = true;
            logLine(HORIZON_LOG_INFO, "host", "bridge: connected");
        }
        const std::string path = bridge::eventsPath(haveCursor, last, g_playMap);
        const bridge::HttpAnswer a = call(g_port, "GET", path, key, "", 30000, true);
        if (g_stop) break;
        if (a.status == 401) {
            logLine(HORIZON_LOG_INFO, "host", "bridge: the key is no longer valid, pairing again");
            setKey("");
            announced = false;
            continue;
        }
        if (a.status != 200) {
            // TM Horizon closed (or restarted on another port): find it again
            g_port = 0;
            announced = false;
            sleepFor(2000);
            continue;
        }
        std::vector<bridge::Event> events;
        long long cursor = last;
        if (!bridge::parseEvents(a.body, &events, &cursor)) {
            sleepFor(5000);
            continue;
        }
        if (haveCursor) {
            for (const bridge::Event& e : events) {
                if (e.kind == "play_map") {
                    onPlayMap(e);
                    continue;
                }
                if (e.title.empty()) continue;
                HorizonToast t = {};
                t.size = sizeof(t);
                t.kind = bridge::toastKind(e.level);
                t.title = e.title.c_str();
                t.text = e.text.c_str();
                t.icon = e.kind == "upload" ? HZ_ICON_CLOUD_UPLOAD : e.kind == "mod" ? HZ_ICON_DOWNLOAD : e.kind == "server" ? HZ_ICON_WIFI : 0u;
                toastsPush("TM Horizon", t);
            }
        }
        last = cursor;
        haveCursor = true;
    }
}

void workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_lock);
            g_cv.wait(lock, [] { return g_stop.load() || !g_queue.empty(); });
            if (g_stop) return;
            job = std::move(g_queue.front());
            g_queue.pop_front();
        }
        const std::string key = currentKey();
        if (g_state != HORIZON_BRIDGE_CONNECTED || key.empty() || !g_port) {
            job.error = HORIZON_BRIDGE_ERR_NOT_CONNECTED;
        } else {
            job.answer = call(g_port, job.method.c_str(), job.path, key, job.body, 45000, false);
            if (job.answer.status == -4) job.error = HORIZON_BRIDGE_ERR_TOO_LARGE;
            else if (job.answer.status < 0) job.error = HORIZON_BRIDGE_ERR_TIMEOUT;
            else if (job.answer.status == 401) setKey("");
        }
        std::lock_guard<std::mutex> lock(g_lock);
        g_done.push_back(std::move(job));
    }
}

bool sehBridgeCb(HorizonBridgeCallback cb, void* user, int32_t status, const char* ct, const void* body, uint32_t len) {
    __try {
        cb(user, status, ct, body, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

size_t openOf(Plugin* p) {
    size_t n = 0;
    for (const Job& j : g_queue) n += j.plugin == p ? 1 : 0;
    for (const Job& j : g_done) n += j.plugin == p ? 1 : 0;
    return n;
}

} // namespace

void bridgeStart() {
    if (g_started) return;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        HWARN("bridge: WSAStartup failed, no connection to TM Horizon");
        return;
    }
    g_enabled = hostSettingInt("bridge.enabled", 1) != 0;
    g_key = hostSetting("bridge.key", "");
    g_instance = hostSetting("bridge.instance", "");
    if (g_instance.size() != 16) {
        unsigned v[2] = {GetCurrentProcessId(), static_cast<unsigned>(GetTickCount64())};
        BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(v), sizeof(v), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        char buf[17];
        snprintf(buf, sizeof(buf), "%08x%08x", v[0], v[1]);
        g_instance = buf;
        hostSetSetting("bridge.instance", g_instance);
    }
    const HostState& s = state();
    g_pairBody = std::string("{\"client\":\"horizon-host\",\"version\":\"") + kHostVersion + "\",\"game\":\"" + bridge::siteCode(s.game.game) +
                 "\",\"gameName\":\"" + s.game.name + "\",\"instance\":\"" + g_instance + "\"}";
    g_site = bridge::siteCode(s.game.game);
    g_playMap = bridge::playMapGame(s.game.game);
    g_stop = false;
    g_control = std::thread(controlLoop);
    g_worker = std::thread(workerLoop);
    g_started = true;
    HLOG("bridge: %s (TM Horizon on 127.0.0.1:%u-%u)%s%s", g_enabled ? "on" : "off (host.json bridge.enabled=0)", bridge::kFirstPort,
         bridge::kFirstPort + bridge::kPortCount - 1, g_key.empty() ? "" : ", paired before", g_playMap ? ", play_map on" : "");
}

void bridgeStop() {
    if (!g_started) return;
    g_stop = true;
    g_cv.notify_all();
    const SOCKET s = g_longPoll.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) closesocket(s);
    if (g_control.joinable()) g_control.join();
    if (g_worker.joinable()) g_worker.join();
    std::lock_guard<std::mutex> lock(g_lock);
    g_queue.clear();
    g_done.clear();
    g_play.clear();
    g_started = false;
}

int bridgeState() { return g_started ? g_state.load() : HORIZON_BRIDGE_OFF; }

std::string bridgeStateText() {
    switch (bridgeState()) {
    case HORIZON_BRIDGE_OFF: return "Off";
    case HORIZON_BRIDGE_SEARCHING: return "TM Horizon is not running.";
    case HORIZON_BRIDGE_PAIRING: return "Press Allow in TM Horizon.";
    case HORIZON_BRIDGE_CONNECTED: return "Connected to TM Horizon.";
    case HORIZON_BRIDGE_DENIED: return "TM Horizon said No. Asks again in a minute.";
    default: return "";
    }
}

bool bridgePaired() { return !currentKey().empty(); }

void bridgeSetEnabled(bool on) {
    g_enabled = on;
    hostSetSettingInt("bridge.enabled", on ? 1 : 0);
    HLOG("menu: TM Horizon connection %s", on ? "on" : "off");
    g_cv.notify_all();
    if (!on) {
        const SOCKET s = g_longPoll.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) closesocket(s);
    }
}

void bridgeForget() {
    setKey("");
    HLOG("menu: TM Horizon pairing forgotten");
    const SOCKET s = g_longPoll.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) closesocket(s);
}

HorizonResult bridgeRequest(Plugin* p, const char* method, const char* path, const void* body, uint32_t len, HorizonBridgeCallback cb,
                            void* user) {
    if (!p || !cb || !method || (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) || !bridge::validPath(path) ||
        (len && !body) || len > (1u << 20))
        return HORIZON_ERR_INVALID_ARG;
    if (!g_started || !g_enabled) return HORIZON_ERR_NOT_SUPPORTED;
    bridge::RateLimit* rl = nullptr;
    for (auto& e : g_limits)
        if (e.first == p) rl = &e.second;
    if (!rl) {
        g_limits.emplace_back(p, bridge::RateLimit(4.0, 8.0));
        rl = &g_limits.back().second;
    }
    Job j;
    j.plugin = p;
    j.method = method;
    j.path = path;
    if (len) j.body.assign(static_cast<const char*>(body), len);
    j.cb = cb;
    j.user = user;
    std::lock_guard<std::mutex> lock(g_lock);
    if (openOf(p) >= 8) return HORIZON_ERR_BUSY;
    if (!rl->take(GetTickCount64())) {
        j.error = HORIZON_BRIDGE_ERR_RATE_LIMIT;
        g_done.push_back(std::move(j));
        return HORIZON_OK;
    }
    g_queue.push_back(std::move(j));
    g_cv.notify_all();
    return HORIZON_OK;
}

void bridgeDeliver() {
    std::deque<Job> done;
    std::deque<PlayRequest> play;
    bool persist = false;
    std::string key;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        done.swap(g_done);
        play.swap(g_play);
        if (g_keyDirty) {
            g_keyDirty = false;
            persist = true;
            key = g_key;
        }
    }
    if (persist) hostSetSetting("bridge.key", key);
    for (const PlayRequest& r : play) runPlayMap(r);
    for (Job& j : done) {
        Plugin* p = j.plugin;
        if (!p || !p->running) continue;
        int32_t status = j.error ? j.error : j.answer.status;
        if (!p->enabled || pluginGated(p)) status = HORIZON_BRIDGE_ERR_CANCELLED;
        const bool withBody = status >= 100;
        if (!sehBridgeCb(j.cb, j.user, status, withBody ? j.answer.contentType.c_str() : "", withBody ? j.answer.body.data() : nullptr,
                         withBody ? static_cast<uint32_t>(j.answer.body.size()) : 0u))
            pluginCrashed(p, "a bridge callback");
    }
}

void bridgeDropPlugin(Plugin* p) {
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto it = g_queue.begin(); it != g_queue.end();) it = it->plugin == p ? g_queue.erase(it) : it + 1;
    for (auto it = g_done.begin(); it != g_done.end();) it = it->plugin == p ? g_done.erase(it) : it + 1;
}

} // namespace horizon
