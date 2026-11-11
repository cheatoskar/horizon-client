// The TM Horizon bridge (docs/client-bridge.md), the parts without sockets or threads: HTTP answer
// parsing, the per-plugin rate limit, the events JSON, the play_map checks. Unit tested in tests/unit_tests.cpp.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/json.h"

namespace horizon {
namespace bridge {

constexpr uint16_t kFirstPort = 27331;
constexpr int kPortCount = 5;
constexpr size_t kMaxBody = 16u << 20;

struct HttpAnswer {
    int status = -1;             // -1: no answer (connection failed / timeout)
    std::string contentType;
    std::string body;
    int retryAfterS = 0;
};

// Splits a full HTTP/1.1 answer (headers + body, Connection: close). False when it is not one.
inline bool parseAnswer(const std::string& raw, HttpAnswer* out) {
    const size_t end = raw.find("\r\n\r\n");
    if (end == std::string::npos || raw.compare(0, 5, "HTTP/") != 0) return false;
    const size_t sp = raw.find(' ');
    if (sp == std::string::npos || sp > end) return false;
    out->status = atoi(raw.c_str() + sp + 1);
    if (out->status < 100 || out->status > 599) return false;
    size_t at = raw.find("\r\n") + 2;
    long long length = -1;
    while (at < end) {
        const size_t eol = raw.find("\r\n", at);
        const std::string line = raw.substr(at, eol - at);
        at = eol + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon), v = line.substr(colon + 1);
        while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
        for (char& c : k) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        if (k == "content-type") out->contentType = v.substr(0, v.find(';'));
        else if (k == "content-length") length = atoll(v.c_str());
        else if (k == "retry-after") out->retryAfterS = atoi(v.c_str());
    }
    out->body = raw.substr(end + 4);
    if (length >= 0 && static_cast<size_t>(length) < out->body.size()) out->body.resize(static_cast<size_t>(length));
    return length < 0 || out->body.size() == static_cast<size_t>(length);
}

// Token bucket: `perSecond` tokens a second, at most `burst`.
class RateLimit {
public:
    RateLimit(double perSecond = 4.0, double burst = 8.0) : rate_(perSecond), burst_(burst), tokens_(burst) {}
    bool take(uint64_t nowMs) {
        if (last_) {
            tokens_ += static_cast<double>(nowMs - last_) / 1000.0 * rate_;
            if (tokens_ > burst_) tokens_ = burst_;
        }
        last_ = nowMs;
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }

private:
    double rate_, burst_, tokens_;
    uint64_t last_ = 0;
};

struct Event {
    long long id = 0;
    long long at = 0; // ms since 1970 (TM Horizon's clock, same PC)
    std::string kind, level, title, text;
    std::string game, path; // play_map: target site code, map path relative to the game's Tracks folder
};

// {"ok":true,"last":N,"events":[...]}: the events and the new cursor. False on bad JSON.
inline bool parseEvents(const std::string& body, std::vector<Event>* events, long long* last) {
    Json j;
    std::string err;
    if (!JsonParser::parse(body, j, err) || j.type != Json::Object) return false;
    if (const Json* l = j.get("last")) *last = l->asInt(*last);
    if (const Json* a = j.get("events")) {
        if (a->type != Json::Array) return false;
        for (const Json& e : a->arr) {
            Event ev;
            if (const Json* v = e.get("id")) ev.id = v->asInt(0);
            if (const Json* v = e.get("kind")) ev.kind = v->asString();
            if (const Json* v = e.get("level")) ev.level = v->asString();
            if (const Json* v = e.get("title")) ev.title = v->asString();
            if (const Json* v = e.get("text")) ev.text = v->asString();
            if (const Json* v = e.get("at")) ev.at = v->asInt(0);
            if (const Json* v = e.get("game")) ev.game = v->asString();
            if (const Json* v = e.get("path")) ev.path = v->asString();
            if (ev.id > *last) *last = ev.id;
            if (ev.title.empty() && ev.kind != "play_map") continue;
            events->push_back(ev);
        }
    }
    return true;
}

// The pairing answer: "ok" (key set), "pending", "denied", "" (not understood).
inline std::string parsePair(const std::string& body, std::string* key) {
    Json j;
    std::string err;
    if (!JsonParser::parse(body, j, err) || j.type != Json::Object) return std::string();
    if (const Json* k = j.get("key")) {
        if (k->type == Json::String && k->str.size() >= 16 && k->str.size() <= 128) {
            *key = k->str;
            return "ok";
        }
    }
    if (const Json* s = j.get("status")) return s->asString();
    return std::string();
}

// Toast kind for an event level.
inline uint32_t toastKind(const std::string& level) {
    if (level == "ok") return 1;
    if (level == "warn") return 2;
    if (level == "error") return 3;
    return 0;
}

// A path a plugin may send: "/v1/...", printable ASCII, no spaces, at most 1 KB.
inline bool validPath(const char* p) {
    if (!p || p[0] != '/' || p[1] != 'v' || p[2] != '1' || p[3] != '/') return false;
    size_t n = 0;
    for (; p[n]; n++)
        if (p[n] <= 32 || p[n] >= 127 || n >= 1024) return false;
    // the bridge's own paths are the host's
    return std::string(p).compare(0, 11, "/v1/client/") != 0;
}

// The host's site code for a game bit (docs/client-bridge.md).
inline const char* siteCode(uint32_t game) {
    switch (game) {
    case 0x01u: return "tmn";
    case 0x02u: return "tms";
    case 0x04u: return "tmo";
    case 0x08u: return "tmnf";
    case 0x10u: return "tmuf";
    default: return "";
    }
}

// ---- play_map (TM Horizon asks the game to open a map file)

constexpr const char* kCaps = "play_map";
constexpr long long kPlayMapMaxAgeMs = 120000; // older requests (e.g. after a reconnect) are dropped

// Games whose host advertises and runs play_map: ESWC and Sunrise (gamectl). Forever is never driven from here.
inline bool playMapGame(uint32_t game) { return game == 0x01u || game == 0x02u; }

// The long-poll path: the first call (no cursor) and the cursor calls, with the capability flag when set.
inline std::string eventsPath(bool haveCursor, long long last, bool playMap) {
    std::string p = "/v1/client/events";
    if (haveCursor) p += "?since=" + std::to_string(last) + "&wait=20";
    if (playMap) p += std::string(haveCursor ? "&" : "?") + "caps=" + kCaps;
    return p;
}

// A play_map event meant for this host (several games can be paired at once).
inline bool playMapFor(const Event& e, const char* site) {
    return e.kind == "play_map" && site && *site && e.game == site;
}

// at 0 = unknown (kept). A request from the future (clock change) is kept too.
inline bool playMapFresh(long long at, long long nowMs) { return at <= 0 || nowMs - at <= kPlayMapMaxAgeMs; }

// The map path of a play_map event, relative to the game's Tracks folder ('/' or '\'), checked and with
// backslashes; "" when refused: absolute, drive, UNC or stream (':'), "." / ".." / empty parts, parts ending
// in '.' or ' ', reserved device names, control or reserved characters, not a .Challenge.Gbx.
inline std::string mapRelPath(const std::string& in) {
    if (in.empty() || in.size() > 400) return std::string();
    std::string p = in;
    for (char& c : p) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || u == 127 || strchr(":*?\"<>|", c)) return std::string();
        if (c == '/') c = '\\';
    }
    if (p[0] == '\\') return std::string();
    static const char kSuffix[] = ".challenge.gbx";
    const size_t sl = sizeof(kSuffix) - 1;
    const size_t file = p.rfind('\\') == std::string::npos ? 0 : p.rfind('\\') + 1;
    if (p.size() - file <= sl) return std::string(); // a name before the suffix
    for (size_t i = 0; i < sl; i++) {
        char c = p[p.size() - sl + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if (c != kSuffix[i]) return std::string();
    }
    size_t at = 0;
    while (at <= p.size()) {
        size_t end = p.find('\\', at);
        if (end == std::string::npos) end = p.size();
        const std::string part = p.substr(at, end - at);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return std::string();
        std::string stem = part.substr(0, part.find('.'));
        for (char& c : stem) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
        while (!stem.empty() && stem.back() == ' ') stem.pop_back();
        static const char* const kDevices[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
        for (const char* d : kDevices)
            if (stem == d) return std::string();
        if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '0' && stem[3] <= '9')
            return std::string();
        at = end + 1;
    }
    return p;
}

} // namespace bridge
} // namespace horizon
