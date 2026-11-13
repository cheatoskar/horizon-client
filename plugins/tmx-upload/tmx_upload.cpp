// TMX Upload - a Horizon Host plugin: offers new replays to uploaders (TM Horizon, the browser extension)
// over the local bridge protocol of the 100% TMX mod: HTTP on 127.0.0.1:27311-27315, /v1/ping, /v1/pair,
// /v1/hello, /v1/next, /v1/file, /v1/result. docs/plugins/tmx-upload.md has the rules and the differences.
//
// Threads
//   game thread   OnEvent, DrawMenu, DrawOverlay, SetEnabled, Init, Shutdown, every host call.
//   bridge thread started by start(): probes the ports, binds, accepts; one short-lived thread per client.
//                 It never calls the host (log lines are queued, settings are written through
//                 run_on_game_thread) and shares state with the game thread under g_mu.
#define HORIZON_PLUGIN_BUILD
#define _CRT_RAND_S
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "horizon_imgui.h"
#include "horizon_plugin.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr char kVersion[] = "0.1.0";
constexpr char kId[] = "tmx-upload";
constexpr int kBasePort = 27311; // same five ports as the 100% TMX mod and the browser extension
constexpr int kPortTries = 5;
constexpr size_t kMaxEntries = 20;
constexpr size_t kMaxResults = 10;
constexpr size_t kMaxTokens = 8;
constexpr double kPairWindow = 180.0;  // seconds an unanswered request (or an Allow nobody collected) stands
constexpr double kDenyCooldown = 60.0; // seconds after "No" before the same app can ask again
constexpr DWORD kSeenMs = 90000;       // a client counts as connected this long after its last request
constexpr int kMaxConns = 8;

const HorizonHostApi* g_host = nullptr;
bool g_imgui = false;
bool g_enabled = false;

enum class Mode { Off, Probing, Serving, Passive, NoPort };
enum class Pair { Idle, Asked, Approved };
enum Client { kClientHorizon = 0, kClientExtension = 1, kClientOther = 2, kClientCount = 3 };

struct Entry {
    std::string id, path, fileName, mapName, site;
    int timeMs = 0;
    bool verified = false;
    bool queued = false;  // waiting for an uploader's /v1/result
    bool offered = false; // handed out by /v1/next at least once
    bool tmxCopy = false;
    int finishSerial = 0;
};

struct ResultRec {
    time_t at = 0;
    std::string map, detail, link;
    int timeMs = 0;
    bool ok = false;
};

// Everything below is guarded by g_mu.
std::mutex g_mu;
Mode g_mode = Mode::Off;
int g_port = 0;
int g_foreignPort = 0;
std::vector<std::string> g_tokens;
Pair g_pair = Pair::Idle;
double g_pairAt = 0;
int g_pairWho = kClientOther;
double g_denyUntil = 0;
std::deque<Entry> g_entries; // newest first
std::deque<ResultRec> g_results; // newest first
DWORD g_seen[kClientCount] = {0, 0, 0};
std::string g_mapUid, g_mapName;
bool g_haveFinish = false;
int g_finishMs = -1;
int g_finishSerial = 0;
unsigned long long g_counter = 0;
std::string g_tag;
std::string g_site = "tmnf";
std::string g_note;
std::vector<std::string> g_logq;

std::atomic<bool> g_running{false};
std::atomic<DWORD> g_recheckMs{30000};
std::thread g_thread;
HANDLE g_stop = nullptr;
std::mutex g_connMu;

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- small helpers ---------------
void qlog(const char* fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_logq.size() < 200) g_logq.push_back(buf);
}

void flushLog() { // game thread
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        lines.swap(g_logq);
    }
    if (!g_host || !g_host->log) return;
    for (const std::string& l : lines) g_host->log(g_host->plugin, HORIZON_LOG_INFO, "%s", l.c_str());
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    return out;
}

bool fileExists(const std::string& path) {
    const DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool readFile(const std::string& path, std::string& out, size_t limit = static_cast<size_t>(64) << 20) {
    FILE* f = _wfopen(widen(path).c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
        if (out.size() > limit) break;
    }
    fclose(f);
    return true;
}

// The map's UID sits in the replay's header as plain text.
bool fileMentions(const std::string& path, const std::string& uid) {
    if (uid.empty()) return false;
    FILE* f = _wfopen(widen(path).c_str(), L"rb");
    if (!f) return false;
    std::string head(64 * 1024, '\0');
    const size_t n = fread(&head[0], 1, head.size(), f);
    fclose(f);
    head.resize(n);
    return head.find(uid) != std::string::npos;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWithNoCase(const std::string& s, const char* suffix) {
    const std::string a = lower(s), b = lower(suffix);
    return a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0;
}

std::string baseName(const std::string& p) {
    const size_t at = p.find_last_of("\\/");
    return at == std::string::npos ? p : p.substr(at + 1);
}

std::string randomHex(int digits) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < digits; i++) {
        unsigned int v = 0;
        if (rand_s(&v) != 0) v = static_cast<unsigned int>(GetTickCount()) + static_cast<unsigned int>(i);
        out.push_back(kHex[v & 0xF]);
    }
    return out;
}

std::string jsonQuote(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char b[8];
            _snprintf_s(b, sizeof(b), _TRUNCATE, "\\u%04x", c);
            out += b;
        } else out.push_back(ch);
    }
    return out + "\"";
}

// Flat JSON reading: enough for {"id":"..","ok":true,"detail":"..","link":".."}.
bool jsonValuePos(const std::string& s, const char* key, size_t& pos) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t at = 0;
    while ((at = s.find(pat, at)) != std::string::npos) {
        size_t p = at + pat.size();
        while (p < s.size() && isspace(static_cast<unsigned char>(s[p]))) p++;
        if (p < s.size() && s[p] == ':') {
            p++;
            while (p < s.size() && isspace(static_cast<unsigned char>(s[p]))) p++;
            pos = p;
            return true;
        }
        at += pat.size();
    }
    return false;
}

void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string jsonString(const std::string& s, const char* key) {
    size_t p;
    if (!jsonValuePos(s, key, p) || p >= s.size() || s[p] != '"') return "";
    std::string out;
    for (p++; p < s.size() && s[p] != '"'; p++) {
        if (s[p] != '\\' || p + 1 >= s.size()) {
            out.push_back(s[p]);
            continue;
        }
        const char e = s[++p];
        if (e == 'n') out.push_back('\n');
        else if (e == 't') out.push_back('\t');
        else if (e == 'r') out.push_back('\r');
        else if (e == 'u' && p + 4 < s.size()) {
            appendUtf8(out, static_cast<unsigned>(strtoul(s.substr(p + 1, 4).c_str(), nullptr, 16)));
            p += 4;
        } else out.push_back(e);
    }
    return out;
}

bool jsonBool(const std::string& s, const char* key) {
    size_t p;
    return jsonValuePos(s, key, p) && s.compare(p, 4, "true") == 0;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(strtoul(s.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') out.push_back(' ');
        else out.push_back(s[i]);
    }
    return out;
}

std::string queryValue(const std::string& query, const std::string& key) {
    size_t at = 0;
    while (at < query.size()) {
        const size_t end = query.find('&', at);
        const std::string pair = query.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) return percentDecode(pair.substr(eq + 1));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return "";
}

std::string timeText(int ms) {
    if (ms <= 0) return "";
    char b[32];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%d:%02d.%02d", ms / 60000, (ms / 1000) % 60, (ms % 1000) / 10);
    return b;
}

const char* clientName(int c) {
    return c == kClientHorizon ? "TM Horizon" : c == kClientExtension ? "browser extension" : "another app";
}

// Who is asking: the browser extension sends an extension Origin, TM Horizon's HTTP client (ureq) a user agent.
int classify(const std::string& origin, const std::string& ua) {
    const std::string o = lower(origin), u = lower(ua);
    if (o.rfind("chrome-extension://", 0) == 0 || o.rfind("moz-extension://", 0) == 0 || o.rfind("safari-web-extension://", 0) == 0)
        return kClientExtension;
    if (u.find("ureq") != std::string::npos || u.find("horizon") != std::string::npos || u.find("tauri") != std::string::npos)
        return kClientHorizon;
    return kClientOther;
}

// ---------------------------------------------------------------- state (call with g_mu held) ----
void lapsePair(double now) {
    if ((g_pair == Pair::Asked || g_pair == Pair::Approved) && now - g_pairAt > kPairWindow) g_pair = Pair::Idle;
}

std::string joinTokens() {
    std::string s;
    for (const std::string& t : g_tokens) s += (s.empty() ? "" : ",") + t;
    return s;
}

bool hasToken(const std::string& key) {
    if (key.empty()) return false;
    for (const std::string& t : g_tokens)
        if (t == key) return true;
    return false;
}

std::string newId() { return std::to_string(++g_counter) + "-" + g_tag; }

// ---------------------------------------------------------------- settings (game thread) -----
void HORIZON_CALL saveTokensCb(void*) {
    if (!g_host) return;
    std::string joined;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        joined = joinTokens();
    }
    g_host->set_string(g_host->plugin, "Tokens", joined.c_str());
}

void loadTokens() {
    char buf[1024] = "";
    g_host->get_string(g_host->plugin, "Tokens", "", buf, sizeof(buf));
    std::lock_guard<std::mutex> lk(g_mu);
    g_tokens.clear();
    std::string cur;
    for (const char* p = buf;; p++) {
        if (*p == ',' || *p == 0) {
            if (cur.size() >= 16 && g_tokens.size() < kMaxTokens) g_tokens.push_back(cur);
            cur.clear();
            if (!*p) break;
        } else cur.push_back(*p);
    }
}

// ---------------------------------------------------------------- the HTTP bridge --------------
struct Req {
    std::string method, path, query, key, host, origin, ua, body;
};
struct Resp {
    int status = 200;
    std::string type = "application/json";
    std::string body;
};

Resp jsonResp(int status, const std::string& body) {
    Resp r;
    r.status = status;
    r.body = body;
    return r;
}

std::string httpText(const Resp& r) {
    const char* reason = r.status == 200 ? "OK" : r.status == 400 ? "Bad Request" : r.status == 401 ? "Unauthorized"
                         : r.status == 403 ? "Forbidden" : r.status == 404 ? "Not Found" : "Error";
    std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " + reason + "\r\n";
    head += "Content-Type: " + r.type + "\r\n";
    head += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    // No Access-Control-Allow-Origin, like the mod: the extension reaches us through a host permission, a
    // web page in a tab has no business reading this.
    head += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    return head + r.body;
}

std::string describe(const Entry& e) {
    return std::string("{\"id\":") + jsonQuote(e.id) + ",\"site\":" + jsonQuote(e.site) + ",\"trackId\":0,\"mapName\":" +
           jsonQuote(e.mapName) + ",\"fileName\":" + jsonQuote(e.fileName) + ",\"timeMs\":" + std::to_string(e.timeMs > 0 ? e.timeMs : 0) +
           ",\"verified\":" + (e.verified ? "true" : "false") + "}";
}

Resp handle(const Req& rq) {
    const int who = classify(rq.origin, rq.ua);
    if (rq.method == "GET" && rq.path == "/v1/ping")
        return jsonResp(200, std::string("{\"ok\":true,\"mod\":") + jsonQuote(std::string("horizon-tmx-upload ") + kVersion) +
                                 ",\"bridge\":\"horizon-host\"}");

    if (rq.method == "POST" && rq.path == "/v1/pair") {
        std::lock_guard<std::mutex> lk(g_mu);
        const double now = nowSeconds();
        lapsePair(now);
        if (g_pair == Pair::Approved) {
            const std::string token = randomHex(32);
            g_tokens.push_back(token);
            while (g_tokens.size() > kMaxTokens) g_tokens.erase(g_tokens.begin());
            g_pair = Pair::Idle;
            g_logq.push_back(std::string("pairing: handed a key to ") + clientName(g_pairWho));
            if (g_host && g_host->run_on_game_thread) g_host->run_on_game_thread(g_host->plugin, saveTokensCb, nullptr);
            return jsonResp(200, std::string("{\"ok\":true,\"key\":") + jsonQuote(token) + "}");
        }
        if (now < g_denyUntil) return jsonResp(200, "{\"ok\":false,\"status\":\"denied\"}");
        if (g_pair != Pair::Asked) {
            g_pair = Pair::Asked;
            g_pairWho = who;
            g_logq.push_back(std::string("pairing: ") + clientName(who) + " asks to upload replays");
        }
        g_pairAt = now; // a client that keeps asking keeps the prompt alive, like the mod
        return jsonResp(200, "{\"ok\":false,\"status\":\"pending\"}");
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!hasToken(rq.key))
            return jsonResp(401, "{\"ok\":false,\"error\":\"pair this app first: allow it in the game\"}");
        g_seen[who] = GetTickCount() | 1; // never 0
    }

    if (rq.method == "GET" && rq.path == "/v1/hello") {
        std::string site;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            site = g_site;
        }
        // "linked" is the 100% TMX account link of the mod; there is none here, so the answer is yes.
        return jsonResp(200, std::string("{\"ok\":true,\"mod\":") + jsonQuote(std::string("horizon-tmx-upload ") + kVersion) +
                                 ",\"game\":" + jsonQuote(site) + ",\"linked\":true}");
    }

    if (rq.method == "GET" && rq.path == "/v1/next") {
        int wait = atoi(queryValue(rq.query, "wait").c_str());
        wait = std::max(0, std::min(wait, 55));
        const double deadline = nowSeconds() + wait;
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(g_mu);
                for (Entry& e : g_entries) { // newest first
                    if (!e.queued) continue;
                    e.offered = true;
                    return jsonResp(200, "{\"ok\":true,\"upload\":" + describe(e) + "}");
                }
            }
            if (!g_running || nowSeconds() >= deadline) break;
            if (WaitForSingleObject(g_stop, 200) == WAIT_OBJECT_0) break;
        }
        return jsonResp(200, "{\"ok\":true,\"upload\":null}");
    }

    if (rq.method == "GET" && rq.path == "/v1/file") {
        const std::string id = queryValue(rq.query, "id");
        std::string file;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (const Entry& e : g_entries)
                if (e.queued && e.id == id) {
                    file = e.path;
                    break;
                }
        }
        if (file.empty()) return jsonResp(404, "{\"ok\":false,\"error\":\"no such replay\"}");
        Resp r;
        if (!readFile(file, r.body) || r.body.empty()) return jsonResp(404, "{\"ok\":false,\"error\":\"the file has gone\"}");
        r.type = "application/octet-stream";
        return r;
    }

    if (rq.method == "POST" && rq.path == "/v1/result") {
        const std::string id = jsonString(rq.body, "id");
        const bool ok = jsonBool(rq.body, "ok");
        const std::string detail = jsonString(rq.body, "detail");
        std::string link = jsonString(rq.body, "link");
        if (link.empty()) link = jsonString(rq.body, "url");
        std::lock_guard<std::mutex> lk(g_mu);
        for (Entry& e : g_entries) {
            if (!e.queued || e.id != id) continue;
            e.queued = false;
            ResultRec r;
            r.at = time(nullptr);
            r.map = e.mapName;
            r.timeMs = e.timeMs;
            r.ok = ok;
            r.detail = !detail.empty() ? detail : (ok ? "Uploaded" : "Refused");
            r.link = link;
            g_results.push_front(r);
            while (g_results.size() > kMaxResults) g_results.pop_back();
            g_logq.push_back(std::string("result: ") + (ok ? "ok" : "not ok") + " - " + r.detail + " (" + e.fileName + ")");
            return jsonResp(200, "{\"ok\":true}");
        }
        return jsonResp(404, "{\"ok\":false,\"error\":\"no such replay\"}");
    }

    return jsonResp(404, "{\"ok\":false,\"error\":\"unknown request\"}");
}

struct Conn {
    SOCKET s = INVALID_SOCKET;
    std::atomic<bool> done{false};
    std::thread t;
};

void serveClient(SOCKET client) {
    DWORD timeout = 15000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string request;
    char buffer[4096];
    size_t headerEnd = std::string::npos;
    while (request.size() < 32 * 1024) {
        const int got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        request.append(buffer, static_cast<size_t>(got));
        headerEnd = request.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
    }
    if (headerEnd == std::string::npos) return;

    const std::string head = request.substr(0, headerEnd);
    Req rq;
    rq.body = request.substr(headerEnd + 4);
    const size_t sp1 = head.find(' ');
    const size_t sp2 = head.find(' ', sp1 + 1);
    std::string target;
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
        rq.method = head.substr(0, sp1);
        target = head.substr(sp1 + 1, sp2 - sp1 - 1);
    }
    size_t contentLength = 0;
    size_t at = head.find("\r\n");
    while (at != std::string::npos) {
        const size_t end = head.find("\r\n", at + 2);
        const std::string line = head.substr(at + 2, end == std::string::npos ? std::string::npos : end - at - 2);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = lower(line.substr(0, colon));
            std::string value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
            if (name == "x-tmx-key") rq.key = value;
            else if (name == "host") rq.host = value;
            else if (name == "origin") rq.origin = value;
            else if (name == "user-agent") rq.ua = value;
            else if (name == "content-length") contentLength = static_cast<size_t>(atoi(value.c_str()));
        }
        at = end;
    }
    while (rq.body.size() < contentLength && rq.body.size() < 1024 * 1024) {
        const int got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        rq.body.append(buffer, static_cast<size_t>(got));
    }
    const size_t mark = target.find('?');
    rq.path = mark == std::string::npos ? target : target.substr(0, mark);
    rq.query = mark == std::string::npos ? "" : target.substr(mark + 1);

    // A web page can rebind its own domain to 127.0.0.1 and read our answers; such a request still carries the
    // page's domain in Host (same check as the mod).
    const std::string host = lower(rq.host);
    const bool localHost = host.rfind("127.0.0.1:", 0) == 0 || host.rfind("localhost:", 0) == 0 || host == "127.0.0.1" || host == "localhost";
    const Resp resp = localHost ? handle(rq) : jsonResp(403, "{\"ok\":false,\"error\":\"only the extension may talk to the mod\"}");
    const std::string answer = httpText(resp);
    size_t sent = 0;
    while (sent < answer.size()) {
        const int wrote = send(client, answer.data() + sent, static_cast<int>(answer.size() - sent), 0);
        if (wrote <= 0) break;
        sent += static_cast<size_t>(wrote);
    }
    shutdown(client, SD_BOTH);
}

void connMain(Conn* c) {
    serveClient(c->s);
    {
        std::lock_guard<std::mutex> lk(g_connMu);
        closesocket(c->s);
        c->s = INVALID_SOCKET;
    }
    c->done = true;
}

// GET /v1/ping on one port: does a bridge (the mod's, or anything answering like one) live there?
bool pingPort(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(port));
    connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    fd_set ex;
    FD_ZERO(&ex);
    FD_SET(s, &ex);
    timeval tv = {0, 500000};
    const int n = select(0, nullptr, &w, &ex, &tv);
    if (n <= 0 || !FD_ISSET(s, &w)) {
        closesocket(s);
        return false;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    DWORD to = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    const std::string req = "GET /v1/ping HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nUser-Agent: horizon-tmx-upload\r\nConnection: close\r\n\r\n";
    send(s, req.data(), static_cast<int>(req.size()), 0);
    std::string answer;
    char buf[1024];
    while (answer.size() < 4096) {
        const int got = recv(s, buf, sizeof(buf), 0);
        if (got <= 0) break;
        answer.append(buf, static_cast<size_t>(got));
    }
    closesocket(s);
    std::string compact;
    for (char c : answer)
        if (c != ' ') compact.push_back(c);
    return compact.rfind("HTTP/1.1200", 0) == 0 && compact.find("\"ok\":true") != std::string::npos && compact.find("\"mod\"") != std::string::npos;
}

int probeForeign() {
    for (int i = 0; i < kPortTries && g_running; i++)
        if (pingPort(kBasePort + i)) return kBasePort + i;
    return 0;
}

SOCKET bindFirst(int& port) {
    for (int i = 0; i < kPortTries; i++) {
        SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (l == INVALID_SOCKET) return INVALID_SOCKET;
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only: this hands out files from the player's disk
        a.sin_port = htons(static_cast<u_short>(kBasePort + i));
        if (bind(l, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(l, 4) == 0) {
            port = kBasePort + i;
            return l;
        }
        closesocket(l);
    }
    return INVALID_SOCKET;
}

void setMode(Mode m, int port, int foreign) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_mode = m;
    g_port = port;
    g_foreignPort = foreign;
}

void serveLoop(SOCKET l) {
    std::list<std::unique_ptr<Conn>> conns;
    auto reap = [&conns]() {
        for (auto it = conns.begin(); it != conns.end();) {
            if ((*it)->done) {
                (*it)->t.join();
                it = conns.erase(it);
            } else ++it;
        }
    };
    while (g_running) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(l, &rs);
        timeval tv = {0, 200000};
        const int n = select(0, &rs, nullptr, nullptr, &tv);
        reap();
        if (n == SOCKET_ERROR) break;
        if (n == 0) continue;
        SOCKET c = accept(l, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        if (static_cast<int>(conns.size()) >= kMaxConns) {
            closesocket(c);
            continue;
        }
        std::unique_ptr<Conn> cn(new Conn());
        cn->s = c;
        Conn* raw = cn.get();
        conns.push_back(std::move(cn));
        raw->t = std::thread(connMain, raw);
    }
    {
        std::lock_guard<std::mutex> lk(g_connMu);
        for (auto& c : conns)
            if (c->s != INVALID_SOCKET) shutdown(c->s, SD_BOTH); // wakes a blocked recv
    }
    for (auto& c : conns) c->t.join();
}

bool waitStop(DWORD ms) { return WaitForSingleObject(g_stop, ms) == WAIT_OBJECT_0; }

void supervisor() {
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        qlog("bridge: winsock would not start");
        setMode(Mode::NoPort, 0, 0);
        return;
    }
    int lastForeign = 0;
    bool first = true;
    while (g_running) {
        if (first) setMode(Mode::Probing, 0, 0); // later checks keep showing the old state while they run
        first = false;
        const int foreign = probeForeign();
        if (!g_running) break;
        if (foreign) {
            if (foreign != lastForeign) qlog("bridge: another mod answers on port %d, staying passive", foreign);
            lastForeign = foreign;
            setMode(Mode::Passive, 0, foreign);
            if (waitStop(g_recheckMs)) break;
            continue;
        }
        lastForeign = 0;
        int port = 0;
        SOCKET l = bindFirst(port);
        if (l == INVALID_SOCKET) {
            qlog("bridge: no free port in %d..%d", kBasePort, kBasePort + kPortTries - 1);
            setMode(Mode::NoPort, 0, 0);
            if (waitStop(g_recheckMs)) break;
            continue;
        }
        qlog("bridge: listening on 127.0.0.1:%d", port);
        setMode(Mode::Serving, port, 0);
        serveLoop(l);
        closesocket(l);
        qlog("bridge: stopped");
        if (g_running && waitStop(1000)) break;
    }
    setMode(Mode::Off, 0, 0);
    WSACleanup();
}

void startBridge() {
    if (g_thread.joinable()) return;
    if (!g_stop) g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(g_stop);
    g_running = true;
    g_thread = std::thread(supervisor);
}

void stopBridge() {
    if (!g_thread.joinable()) return;
    g_running = false;
    SetEvent(g_stop);
    g_thread.join();
    setMode(Mode::Off, 0, 0);
}

// ---------------------------------------------------------------- replays (game thread) --------
void setNote(const std::string& n) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_note = n;
}

// Rule: a replay file the host announces (REPLAY_SAVED) is queued when the local player has finished the map
// that is loaded now (FINISH with is_local_player after the last MAP_LOADED / RACE_START) and the file is a
// *.Replay.Gbx that exists. Replays seen while watching (play mode replay) are never queued.
void enqueue(const std::string& path) {
    std::string note;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const bool tmx = endsWithNoCase(path, " TMX.Replay.Gbx");
        for (auto it = g_entries.begin(); it != g_entries.end();) {
            if (it->path == path) {
                it = g_entries.erase(it); // saved again: offer it again
            } else if (tmx && it->finishSerial == g_finishSerial && !it->tmxCopy && it->queued) {
                it = g_entries.erase(it); // the local-ghost copy replaces the full multi-ghost file of that finish
            } else ++it;
        }
        if (!tmx)
            for (const Entry& e : g_entries)
                if (e.finishSerial == g_finishSerial && e.tmxCopy) return; // the TMX copy of this finish is already there
        Entry e;
        e.id = newId();
        e.path = path;
        e.fileName = baseName(path);
        e.mapName = g_mapName;
        e.site = g_site;
        e.timeMs = g_finishMs;
        e.verified = fileMentions(path, g_mapUid);
        e.queued = true;
        e.tmxCopy = tmx;
        e.finishSerial = g_finishSerial;
        g_entries.push_front(e);
        while (g_entries.size() > kMaxEntries) g_entries.pop_back();
        g_logq.push_back("queued " + e.fileName + (e.verified ? " (map uid confirmed)" : " (map uid not confirmed)"));
        note = "New replay ready: " + e.fileName;
    }
    setNote(note);
}

void uploadLast() {
    bool needSave = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        needSave = g_haveFinish && (g_entries.empty() || g_entries.front().finishSerial != g_finishSerial);
    }
    // A finish the queue does not hold yet: ask the host to save its replay (the file then arrives as REPLAY_SAVED).
    if (needSave && g_host && g_host->abi_minor >= 1 && g_host->size >= offsetof(HorizonHostApi, save_replay) + sizeof(void*) &&
        g_host->save_replay) {
        const HorizonResult r = g_host->save_replay(g_host->plugin, "");
        if (r == HORIZON_OK) {
            setNote("Saving the replay...");
            return;
        }
        qlog("save_replay returned %d, offering the newest queued replay instead", static_cast<int>(r));
    }
    std::string note;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_entries.empty()) note = "No replay yet.";
        else {
            Entry& e = g_entries.front();
            if (!fileExists(e.path)) note = "The replay file is gone.";
            else {
                e.id = newId();
                e.queued = true;
                e.offered = false;
                note = "Offered again: " + e.fileName;
            }
        }
    }
    setNote(note);
}

// ---------------------------------------------------------------- UI ---------------------------
struct Snap {
    Mode mode = Mode::Off;
    int port = 0, foreign = 0;
    size_t tokens = 0;
    Pair pair = Pair::Idle;
    int pairWho = kClientOther;
    std::vector<Entry> entries;
    std::vector<ResultRec> results;
    bool seen[kClientCount] = {false, false, false};
    std::string note;
};

Snap snapshot() {
    Snap s;
    std::lock_guard<std::mutex> lk(g_mu);
    lapsePair(nowSeconds());
    s.mode = g_mode;
    s.port = g_port;
    s.foreign = g_foreignPort;
    s.tokens = g_tokens.size();
    s.pair = g_pair;
    s.pairWho = g_pairWho;
    s.entries.assign(g_entries.begin(), g_entries.end());
    s.results.assign(g_results.begin(), g_results.end());
    const DWORD now = GetTickCount();
    for (int i = 0; i < kClientCount; i++) s.seen[i] = g_seen[i] != 0 && now - g_seen[i] < kSeenMs;
    s.note = g_note;
    return s;
}

void answerPair(bool allow) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const double now = nowSeconds();
        if (allow) {
            if (g_pair == Pair::Asked) {
                g_pair = Pair::Approved;
                g_pairAt = now;
                g_denyUntil = 0;
            }
        } else {
            g_pair = Pair::Idle;
            g_denyUntil = now + kDenyCooldown;
        }
        g_logq.push_back(allow ? "pairing: allowed" : "pairing: refused");
    }
}

void forgetPaired() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_tokens.clear();
        g_pair = Pair::Idle;
    }
    saveTokensCb(nullptr);
}

ImVec4 col(const HorizonColor* c, ImVec4 fallback) { return c ? horizon_color(*c) : fallback; }

void drawPairPrompt(const Snap& s) {
    ImGui::Text("%s wants to upload replays.", clientName(s.pairWho));
    if (ImGui::Button("Allow")) answerPair(true);
    ImGui::SameLine();
    if (ImGui::Button("No")) answerPair(false);
}

std::string connectedText(const Snap& s) {
    std::string t;
    for (int i = 0; i < kClientCount; i++)
        if (s.seen[i]) t += (t.empty() ? "" : ", ") + std::string(clientName(i));
    return t;
}

// The one status line: "Uploader: waiting (port 27311)", "Uploader: connected (TM Horizon)", "Uploader: off" ...
// 0 = muted, 1 = ok, 2 = warn.
std::string statusLine(const Snap& s, int* tone) {
    char b[160];
    *tone = 0;
    switch (s.mode) {
    case Mode::Serving: {
        const std::string who = connectedText(s);
        if (!who.empty()) {
            *tone = 1;
            _snprintf_s(b, sizeof(b), _TRUNCATE, "Uploader: connected (%s)", who.c_str());
        } else {
            _snprintf_s(b, sizeof(b), _TRUNCATE, "Uploader: waiting (port %d)", s.port);
        }
        return b;
    }
    case Mode::Passive:
        *tone = 2;
        _snprintf_s(b, sizeof(b), _TRUNCATE, "Uploader: other mod (port %d)", s.foreign);
        return b;
    case Mode::NoPort: *tone = 2; return "Uploader: no free port";
    case Mode::Probing: return "Uploader: starting";
    default: return "Uploader: off";
    }
}

} // namespace

// ================================================================= plugin exports ================
extern "C" {

static const HorizonPluginInfo kInfo = {
    HORIZON_ABI,
    kId,
    "TMX Upload",
    kVersion,
    HORIZON_GAME_ALL,
    HORIZON_CAP_MENU | HORIZON_CAP_OVERLAY | HORIZON_CAP_GAME_EVENTS,
    HORIZON_FLAG_ONLINE_OK | HORIZON_FLAG_LIVE_TOGGLE,
    "cheatoskar",
    "Lets the TMX uploader pick up your new replays.",
    HORIZON_ABI_MINOR,
};

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) { return &kInfo; }

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    if (!host || host->abi != HORIZON_ABI) return 1;
    g_host = host;
    g_imgui = horizon_imgui_attach(host);
    if (!g_imgui) host->log(host->plugin, HORIZON_LOG_WARN, "ImGui attach failed, no menu page");
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_entries.clear();
        g_results.clear();
        g_pair = Pair::Idle;
        g_denyUntil = 0;
        g_haveFinish = false;
        g_finishMs = -1;
        g_finishSerial = 0;
        g_mapUid.clear();
        g_mapName.clear();
        g_note.clear();
        g_logq.clear();
        for (DWORD& d : g_seen) d = 0;
        g_tag = randomHex(6);
        const HorizonGameInfo* gi = host->game_info ? host->game_info() : nullptr;
        const uint32_t game = gi ? gi->game : HORIZON_GAME_TMNF;
        // The mod's site codes; the launcher maps them to the exchange.
        g_site = game == HORIZON_GAME_ESWC ? "tmn" : game == HORIZON_GAME_SUNRISE ? "tms" : game == HORIZON_GAME_ORIGINAL ? "tmo"
                 : game == HORIZON_GAME_TMUF ? "tmuf" : "tmnf";
    }
    loadTokens();
    host->subscribe(host->plugin, HORIZON_EVENT_BIT(HORIZON_EVENT_MAP_LOADED) | HORIZON_EVENT_BIT(HORIZON_EVENT_RACE_START) |
                                      HORIZON_EVENT_BIT(HORIZON_EVENT_FINISH) | HORIZON_EVENT_BIT(HORIZON_EVENT_REPLAY_SAVED));
    g_enabled = true;
    startBridge();
    host->log(host->plugin, HORIZON_LOG_INFO, "---- tmx-upload %s start (site code %s) ----", kVersion, g_site.c_str());
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    stopBridge();
    flushLog();
    if (g_stop) {
        CloseHandle(g_stop);
        g_stop = nullptr;
    }
    g_enabled = false;
    g_host = nullptr;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_SetEnabled(int32_t enabled) {
    g_enabled = enabled != 0;
    if (g_enabled) startBridge();
    else {
        stopBridge();
        flushLog();
    }
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_OnEvent(const HorizonEvent* e) {
    if (!e || !g_enabled || !g_host) return;
    flushLog();
    switch (e->type) {
    case HORIZON_EVENT_MAP_LOADED: {
        std::lock_guard<std::mutex> lk(g_mu);
        g_haveFinish = false;
        g_finishMs = -1;
        g_mapUid = (e->map && e->map->uid) ? e->map->uid : "";
        g_mapName = (e->map && e->map->name) ? e->map->name : "";
        break;
    }
    case HORIZON_EVENT_RACE_START: {
        std::lock_guard<std::mutex> lk(g_mu);
        g_haveFinish = false;
        g_finishMs = -1;
        break;
    }
    case HORIZON_EVENT_FINISH: {
        if (!e->is_local_player) break;
        std::lock_guard<std::mutex> lk(g_mu);
        g_haveFinish = true;
        g_finishMs = e->time_ms;
        g_finishSerial++;
        break;
    }
    case HORIZON_EVENT_REPLAY_SAVED: {
        if (e->size < offsetof(HorizonEvent, path) + sizeof(e->path) || !e->path || !*e->path) break;
        const std::string path = e->path;
        bool finished;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            finished = g_haveFinish;
        }
        if (e->play_mode == HORIZON_MODE_REPLAY || !finished) {
            g_host->log(g_host->plugin, HORIZON_LOG_DEBUG, "replay %s not queued: no local finish in this map", path.c_str());
            break;
        }
        if (!endsWithNoCase(path, ".Replay.Gbx") || !fileExists(path)) break;
        enqueue(path);
        break;
    }
    default: break;
    }
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawMenu(void) {
    if (!g_imgui || !g_host) return;
    flushLog();
    const Snap s = snapshot();
    const HorizonStyle* st = g_host->style ? g_host->style() : nullptr;
    const ImVec4 okc = col(st ? &st->ok : nullptr, ImVec4(0.3f, 0.8f, 0.4f, 1.0f));
    const ImVec4 warnc = col(st ? &st->warn : nullptr, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
    const ImVec4 badc = col(st ? &st->bad : nullptr, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
    const ImVec4 mutedc = col(st ? &st->muted : nullptr, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));

    int tone = 0;
    const std::string status = g_enabled ? statusLine(s, &tone) : std::string("Uploader: off");
    ImGui::TextColored(tone == 1 ? okc : tone == 2 ? warnc : mutedc, "%s", status.c_str());
    if (s.mode == Mode::Passive && ImGui::IsItemHovered()) ImGui::SetTooltip("Checked again every 30 s.");
    if (s.mode == Mode::NoPort && ImGui::IsItemHovered()) ImGui::SetTooltip("Ports 27311-27315 are taken.");
    ImGui::Spacing();

    if (s.pair == Pair::Asked) {
        ImGui::PushStyleColor(ImGuiCol_Text, warnc);
        drawPairPrompt(s);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    if (ImGui::Button("Upload last")) uploadLast();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Offer the newest replay again.");
    ImGui::SameLine();
    if (ImGui::Button("Forget apps")) forgetPaired();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paired: %d. They ask again next time.", static_cast<int>(s.tokens));
    if (!s.note.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(mutedc, "%s", s.note.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    int waiting = 0;
    for (const Entry& e : s.entries)
        if (e.queued) waiting++;
    ImGui::Text("Queue: %d", waiting);
    ImGui::Separator();
    ImGui::TextUnformatted("Results");
    if (s.results.empty()) ImGui::TextColored(mutedc, "None");
    for (const ResultRec& r : s.results) {
        char when[16] = "";
        tm lt = {};
        if (localtime_s(&lt, &r.at) == 0) strftime(when, sizeof(when), "%H:%M", &lt);
        ImGui::TextColored(r.ok ? okc : badc, "%s", r.ok ? "OK" : "Failed");
        ImGui::SameLine();
        ImGui::Text("%s  %s  %s", when, r.map.empty() ? "?" : r.map.c_str(), timeText(r.timeMs).c_str());
        // small windows: detail and link wrap instead of running off the right edge
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(mutedc, "%s", r.link.empty() ? r.detail.c_str() : (r.detail + "  " + r.link).c_str());
        ImGui::PopTextWrapPos();
    }
}

// The pairing question when the menu is closed. The host routes clicks to ImGui while a window is hovered.
HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawOverlay(void) {
    if (!g_imgui || !g_enabled || !g_host) return;
    flushLog();
    if (g_host->menu_open && g_host->menu_open()) return;
    const Snap s = snapshot();
    if (s.pair != Pair::Asked) return;
    const ImGuiIO& io = ImGui::GetIO();
    const HorizonStyle* st = g_host->style ? g_host->style() : nullptr;
    const float scale = (st && st->scale > 0.0f) ? st->scale : 1.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 48.0f * scale), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##tmx-upload-pair", nullptr, flags)) drawPairPrompt(s);
    ImGui::End();
}

// Test hook for tests\fake_host.cpp (nothing in the game calls it).
//   op 1: answer the pairing prompt (arg 1 = Allow, 0 = No)    op 2: out = "<mode>:<port>:<foreign port>"
//   op 3: re-check interval in ms (arg)                         op 4: out = "<pair state>:<tokens>:<results>:<queued>"
//   op 5: forget the "No" cool-down                             op 6: Upload last
HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_TestHook(int32_t op, int32_t arg, char* out, int32_t outSize) {
    if (out && outSize > 0) out[0] = 0;
    switch (op) {
    case 1: answerPair(arg != 0); return 0;
    case 2:
    case 4: {
        const Snap s = snapshot();
        char b[96];
        if (op == 2) {
            const char* m = s.mode == Mode::Serving ? "serving" : s.mode == Mode::Passive ? "passive" : s.mode == Mode::NoPort ? "noport"
                            : s.mode == Mode::Probing ? "probing" : "off";
            _snprintf_s(b, sizeof(b), _TRUNCATE, "%s:%d:%d", m, s.port, s.foreign);
        } else {
            int queued = 0;
            for (const Entry& e : s.entries) queued += e.queued ? 1 : 0;
            _snprintf_s(b, sizeof(b), _TRUNCATE, "%s:%d:%d:%d", s.pair == Pair::Asked ? "asked" : s.pair == Pair::Approved ? "approved" : "idle",
                        static_cast<int>(s.tokens), static_cast<int>(s.results.size()), queued);
        }
        if (out && outSize > 0) strncpy_s(out, static_cast<size_t>(outSize), b, _TRUNCATE);
        return 0;
    }
    case 3: g_recheckMs = static_cast<DWORD>(arg); return 0;
    case 5: {
        std::lock_guard<std::mutex> lk(g_mu);
        g_denyUntil = 0;
        return 0;
    }
    case 6: uploadLast(); return 0;
    default: return -1;
    }
}

} // extern "C"
