// A fake TMX exchange for the TMX Browser tests and previews (no network): answers the TMX API v2 calls the
// plugin makes (search, random, pictures, map files, Forever play links) through HorizonClient_TestNet.
#pragma once

#include <stdint.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace faketmx {

typedef void(__cdecl* PutFn)(void* ctx, const char* type, const char* location, const void* body, int32_t len);
typedef int32_t(__cdecl* FetchFn)(const char* url, int32_t follow, PutFn put, void* ctx);
typedef int32_t(__cdecl* LinkFn)(const char* link);
typedef void(__cdecl* TestNetFn)(FetchFn fetch, LinkFn link);

struct State {
    std::vector<std::string> urls;  // every request
    std::vector<std::string> links; // tmtp links handed to the game
    int forceStatus = 0;            // != 0: every request answers this status
    bool linksWork = true;
};
inline State& state() {
    static State s;
    return s;
}

inline const char* const* names() {
    static const char* const n[] = {"Paradise Lost", "Speedway Echo", "Tech Garden", "Moonlit Dirt", "A01-Race Remake", "Coastline Drift",
                                    "Lunatic Ladder", "Short & Sweet", "Glass Highway", "Night Shift", "Bumper Cars", "Long Haul"};
    return n;
}

inline std::string param(const std::string& url, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t p = url.find("?" + k);
    if (p == std::string::npos) p = url.find("&" + k);
    if (p == std::string::npos) return "";
    p += k.size() + 1;
    const size_t e = url.find('&', p);
    return url.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

inline bool forever(const std::string& url) { return url.find("tmnf.exchange") != std::string::npos || url.find("tmuf.exchange") != std::string::npos; }

// Map i (0..23): id 1000+i. On the old sites map 1011 has no screenshot.
inline std::string trackJson(int i, bool fv) {
    static const char* const authors[] = {"Nadeo", "cheatoskar", "riolu", "Wirtual", "Hefest", "Spam", "eyebo", "Kappa"};
    const int id = 1000 + i;
    const bool images = !fv && id != 1011;
    char b[700];
    snprintf(b, sizeof(b),
             "{\"TrackId\":%d,\"TrackName\":\"$o%s\",\"Uploader\":{\"Name\":\"up\"},\"Authors\":[{\"User\":{\"Name\":\"%s\"}}],\"Awards\":%d,"
             "\"Tags\":[%d],\"Difficulty\":%d,\"AuthorTime\":%d,\"Environment\":7,\"HasThumbnail\":%s,\"Images\":[%s],\"UId\":\"FakeUid%020d\","
             "\"UploadedAt\":\"2025-0%d-1%dT12:00:00\"}",
             id, names()[i % 12], authors[i % 8], (i * 37) % 120, i % 8, i % 4, 25000 + i * 3170, fv ? "true" : "false",
             images ? "{\"Width\":480,\"Height\":240,\"HasHighQuality\":false}" : "", id, 1 + i % 9, i % 10);
    return b;
}

// A 24-bit BMP picture (stb_image reads it like the real JPEGs).
inline std::string bmp(int id) {
    const int w = 192, h = 108;
    const int row = (w * 3 + 3) & ~3;
    std::string f(54 + static_cast<size_t>(row) * h, '\0');
    auto p32 = [&](size_t at, uint32_t v) { memcpy(&f[at], &v, 4); };
    f[0] = 'B';
    f[1] = 'M';
    p32(2, static_cast<uint32_t>(f.size()));
    p32(10, 54);
    p32(14, 40);
    p32(18, static_cast<uint32_t>(w));
    p32(22, static_cast<uint32_t>(h));
    f[26] = 1;
    f[28] = 24;
    const unsigned hue = static_cast<unsigned>(id) * 2654435761u;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            char* px = &f[54 + static_cast<size_t>(y) * row + static_cast<size_t>(x) * 3];
            const bool sky = y > h / 2, road = !sky && std::abs(x - w / 2) < (h / 2 - y) / 2 + 6;
            px[0] = static_cast<char>(sky ? 230 : road ? 60 : 40);
            px[1] = static_cast<char>(sky ? 170 : road ? 60 : 120 + (hue >> 8) % 60);
            px[2] = static_cast<char>(sky ? 90 + (hue % 80) : road ? 66 : 50);
        }
    return f;
}

inline std::string challenge(int id) {
    std::string f = "GBX";
    f += '\x06';
    f += '\x00';
    f += "BUCR";
    const uint32_t cls = 0x03043000u;
    f.append(reinterpret_cast<const char*>(&cls), 4);
    f += " fake challenge " + std::to_string(id);
    return f;
}

inline int32_t __cdecl fetch(const char* urlC, int32_t follow, PutFn put, void* ctx) {
    const std::string url = urlC ? urlC : "";
    state().urls.push_back(url);
    if (state().forceStatus) {
        put(ctx, "text/html", "", "busy", 4);
        return state().forceStatus;
    }
    const bool fv = forever(url);
    if (url.find("/api/tracks?") != std::string::npos) {
        const int count = atoi(param(url, "count").c_str());
        const int after = atoi(param(url, "after").c_str());
        const std::string name = param(url, "name");
        std::string r;
        bool more = false;
        if (count >= 100) { // random: one map
            r = trackJson(7, fv);
        } else {
            const int base = after > 0 ? 12 : 0;
            more = after <= 0;
            int n = 0;
            for (int i = 0; i < 12; i++) {
                if (!name.empty() && name != "a" && std::string(names()[i]).find(name) == std::string::npos) continue;
                if (n++) r += ",";
                r += trackJson(i + base, fv);
            }
        }
        const std::string body = std::string("{\"More\":") + (more ? "true" : "false") + ",\"Results\":[" + r + "]}";
        put(ctx, "application/json", "", body.data(), static_cast<int32_t>(body.size()));
        return 200;
    }
    const size_t ts = url.find("/trackshow/");
    if (ts != std::string::npos) {
        const int id = atoi(url.c_str() + ts + 11);
        const std::string b = bmp(id);
        put(ctx, "image/bmp", "", b.data(), static_cast<int32_t>(b.size()));
        return 200;
    }
    const size_t tg = url.find("/trackgbx/");
    if (tg != std::string::npos) {
        const int id = atoi(url.c_str() + tg + 10);
        if (id == 1013) { // TMX answers a page instead of a map
            put(ctx, "text/html", "", "<html>", 6);
            return 200;
        }
        const std::string b = challenge(id);
        put(ctx, "application/octet-stream", "", b.data(), static_cast<int32_t>(b.size()));
        return 200;
    }
    const size_t tp = url.find("/trackplay/");
    if (tp != std::string::npos && fv && !follow) {
        const int id = atoi(url.c_str() + tp + 11);
        const std::string site = url.find("tmuf.exchange") != std::string::npos ? "tmu" : "tmnf";
        const std::string loc = "tmtp:///:tmx:download:" + site + "?token=tok" + std::to_string(id) + "%3d%3d";
        put(ctx, "text/html", loc.c_str(), "", 0);
        return 302;
    }
    put(ctx, "text/html", "", "not found", 9);
    return 404;
}

inline int32_t __cdecl link(const char* l) {
    state().links.push_back(l ? l : "");
    return state().linksWork ? 1 : 0;
}

inline int count(const char* part) {
    int n = 0;
    for (const std::string& u : state().urls)
        if (u.find(part) != std::string::npos) n++;
    return n;
}

} // namespace faketmx
