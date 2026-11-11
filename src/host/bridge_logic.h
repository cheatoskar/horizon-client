// The TM Horizon bridge (docs/client-bridge.md), the parts without sockets or threads: HTTP answer
// parsing, the per-plugin rate limit, the events JSON. Unit tested in tests/unit_tests.cpp.
#pragma once

#include <cstdint>
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
    std::string kind, level, title, text;
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
            if (ev.title.empty()) continue;
            if (ev.id > *last) *last = ev.id;
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

} // namespace bridge
} // namespace horizon
