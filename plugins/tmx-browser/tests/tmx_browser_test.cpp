// Unit tests of the TMX Browser logic (no ImGui, no host). Plain main(), PASS/FAIL lines, exit code = failures.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tmx_logic.h"
#include "tmx_net.h"

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* name) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) g_pass++;
    else g_fail++;
}

static std::string bmp(int w, int h) {
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
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            char* px = &f[54 + static_cast<size_t>(y) * row + static_cast<size_t>(x) * 3];
            px[0] = static_cast<char>(200); // B
            px[1] = static_cast<char>(100); // G
            px[2] = static_cast<char>(50);  // R
        }
    return f;
}

int main() {
    using namespace tmx;

    // ---- TMX API URLs
    {
        Form f;
        f.name = "a b&c";
        f.author = "Wirtual";
        f.tagMask = (1u << 0) | (1u << 7);
        f.difficulty = 2;
        f.length = 2; // 30-60 s
        f.awards = 10;
        const std::string q = apiSearchUrl("tmnf", f, 20, 0);
        check(q.compare(0, 41, "https://tmnf.exchange/api/tracks?fields=T") == 0 && q.find("HasThumbnail%2CImages") != std::string::npos, "search URL: host and fields");
        check(q.find("&count=20&inunlimiter=0&name=a%20b%26c&author=Wirtual&tag=0,7&difficulty=2&authortimemin=30000&authortimemax=60000") != std::string::npos &&
                  q.find("after=") == std::string::npos && q.find("awards") == std::string::npos,
              "search URL with every filter (awards stay with the plugin)");
        check(apiSearchUrl("tmo", Form(), 20, 263172).find("https://original.tm-exchange.com/api/tracks?") == 0 &&
                  apiSearchUrl("tmo", Form(), 20, 263172).find("&after=263172") != std::string::npos,
              "Original: original.tm-exchange.com, page cursor");
        Form g;
        g.length = 1;
        check(apiSearchUrl("tmn", g, 20, 0).find("authortimemax=30000") != std::string::npos && apiSearchUrl("tmn", g, 20, 0).find("authortimemin") == std::string::npos,
              "length < 30 s has only a maximum");
        g.length = 4;
        check(apiSearchUrl("tmn", g, 20, 0).find("authortimemin=120000") != std::string::npos && apiSearchUrl("tmn", g, 20, 0).find("authortimemax") == std::string::npos,
              "length > 2 min has only a minimum");
        const std::string r = apiRandomUrl("tms", f);
        check(r.find("name=") == std::string::npos && r.find("author=") == std::string::npos && r.find("tag=0,7") != std::string::npos && r.find("count=100") != std::string::npos &&
                  r.compare(0, 31, "https://sunrise.tm-exchange.com") == 0,
              "random URL keeps filters, drops text");
        Track t;
        t.id = 12;
        t.thumbKind = 1;
        check(imageUrl("tmuf", t) == "https://tmuf.exchange/trackshow/12/thumbnail", "Forever picture: the map's thumbnail");
        t.thumbKind = 2;
        check(imageUrl("tmo", t) == "https://original.tm-exchange.com/trackshow/12/image/1", "old site picture: the first screenshot");
        check(gbxUrl("tmn", 7) == "https://nations.tm-exchange.com/trackgbx/7" && playUrl("tmnf", 7) == "https://tmnf.exchange/trackplay/7", "map and play URLs");
        check(trackUrl("tmnf", 1000) == "https://tmnf.exchange/trackshow/1000", "track URL");
        check(tagSummary((1u << 2) | (1u << 8)) == "Maze, SpeedTech", "tag summary");
        check(knownSite("tmo") && !knownSite("tm2") && siteForever("tmuf") && !siteForever("tmn"), "site codes");
        check(allowedHost("TMNF.exchange") && allowedHost("original.tm-exchange.com") && !allowedHost("evil.example") && !allowedHost("tmnf.exchange.evil.example"), "host allow list");
    }

    // ---- TMX API answers
    {
        const std::string body =
            "{\"More\":true,\"Results\":["
            "{\"TrackId\":2233,\"TrackName\":\"$o$f00A01-Race\",\"Authors\":[{\"User\":{\"Name\":\"Nadeo\"}}],\"Uploader\":{\"Name\":\"up\"},\"Awards\":357,\"Tags\":[0,7],"
            "\"Difficulty\":0,\"AuthorTime\":24540,\"Environment\":7,\"HasThumbnail\":true,\"Images\":[],\"UId\":\"abc\",\"UploadedAt\":\"2008-04-12T00:00:00\"},"
            "{\"TrackId\":5,\"TrackName\":\"\",\"Uploader\":{\"Name\":\"x\"},\"Awards\":0,\"HasThumbnail\":false,\"Images\":[]},"
            "{\"TrackId\":6,\"TrackName\":\"Shot\",\"Awards\":12,\"HasThumbnail\":false,\"Images\":[{\"Width\":320,\"Height\":240}]},{\"nope\":1}]}";
        std::vector<Track> v;
        bool more = false;
        int last = 0;
        std::string err;
        const bool ok = parseApiTracks(body, 0, v, more, last, err);
        check(ok && v.size() == 3 && more && last == 6, "API answer parsed (bad entry skipped), cursor = last id");
        check(ok && v[0].name == "A01-Race" && v[0].author == "Nadeo" && v[0].awards == 357 && v[0].authorTime == 24540 && v[0].tags.size() == 2 && v[0].tags[1] == "Tech" &&
                  v[0].difficulty == "Beginner" && v[0].environment == "Stadium" && v[0].uid == "abc",
              "track fields, $-codes stripped, ids to names");
        check(ok && v[0].thumbKind == 1 && v[0].thumbnail, "HasThumbnail: the map's own thumbnail");
        check(ok && v[1].name == "Map 5" && v[1].author == "x" && v[1].thumbKind == -1 && !v[1].thumbnail && v[1].authorTime == -1, "no picture at all (Original), uploader as author");
        check(ok && v[2].thumbKind == 2 && v[2].thumbnail, "old sites: a screenshot counts as the picture");
        check(parseApiTracks(body, 10, v, more, last, err) && v.size() == 2 && last == 6, "awards filter drops maps, cursor stays the last id");
        check(!parseApiTracks("not json", 0, v, more, last, err) && !err.empty(), "garbage rejected");
        check(!parseApiTracks("{\"error\":1}", 0, v, more, last, err), "answer without Results rejected");
    }

    // ---- links, map files, messages
    {
        check(tmtpLinkOk("tmtp:///:tmx:download:tmnf?token=fkDh%2fdHt%3d%3d") && tmtpLinkOk("tmtp:///:tmx:download:tmu?token=MQ%3d"), "TMX play links accepted");
        check(!tmtpLinkOk("tmtp:///:evil") && !tmtpLinkOk("https://tmnf.exchange/") && !tmtpLinkOk("tmtp:///:tmx:download:tmnf?token=a b") &&
                  !tmtpLinkOk("tmtp:///:tmx:download:tmnf?\"x") && !tmtpLinkOk(std::string("tmtp:///:tmx:download:") + std::string(1100, 'a')),
              "other links refused");
        std::string err;
        std::string gbx = "GBX";
        gbx += '\x06';
        gbx += '\x00';
        gbx += "BUCR";
        gbx += std::string("\x00\x30\x04\x03", 4);
        gbx += "rest of the file";
        check(checkMapFile(gbx.data(), gbx.size(), err), "challenge (0x03043000) accepted");
        std::string old = "GBX";
        old += '\x03';
        old += '\x00';
        old += "BUC";
        old += std::string("\x00\x30\x00\x24", 4);
        old += "old challenge data";
        check(checkMapFile(old.data(), old.size(), err), "old challenge (version 3, 0x24003000) accepted");
        std::string replay = gbx;
        replay.replace(9, 4, std::string("\x00\x30\x09\x03", 4));
        check(!checkMapFile(replay.data(), replay.size(), err) && err == "Not a map file", "a replay is not a map");
        check(!checkMapFile("<html><body>", 12, err) && !checkMapFile(nullptr, 0, err), "html and nothing refused");
        std::string big(kMaxMapBytes + 1, '\0');
        memcpy(&big[0], gbx.data(), gbx.size());
        check(!checkMapFile(big.data(), big.size(), err) && err == "Map file too large", "size limit");
        check(netErrorText(200).empty() && netErrorText(429) == "Too many requests" && netErrorText(404) == "Not found on TMX" && netErrorText(0) == "No connection to TMX" &&
                  netErrorText(503) == "TMX error 503",
              "network error texts");
        check(playResultText(-3) == "Leave the server first" && playResultText(-2) == "Main menu only" && playResultText(0) == "Map started", "play result texts");
        check(!isPlaceholderImage("abc", 3) && fnv1a("a", 1) == 0xe40c292cu, "placeholder check and FNV-1a");
        check(thumbCacheName("tmo", 42) == "tmo-42.img", "cache file name");
    }

    // ---- file names and folders
    {
        check(sanitizeFileName("a/b\\c:d*e?f\"g<h>i|j") == "a b c d e f g h i j", "forbidden characters replaced");
        check(sanitizeFileName("$o$fffNice $$ Map") == "Nice $ Map", "$-codes stripped, $$ kept as one $");
        check(sanitizeFileName("  ..  ") == "map" && sanitizeFileName("") == "map", "empty name becomes map");
        check(sanitizeFileName(std::string(200, 'x')).size() == 80, "limited to 80 characters");
        std::string u;
        for (int i = 0; i < 100; i++) u += "\xC3\xA4"; // 100 x a-umlaut
        const std::string cut = sanitizeFileName(u);
        check(cut.size() == 160, "cut by characters, not bytes");
        check(sanitizeFileName("trailing dot. ") == "trailing dot", "no trailing dots or spaces");
        check(mapFileName("Paradise Lost", 1000) == "Paradise Lost (1000).Challenge.Gbx", "map file name");
        check(mapsFolder(true, "C:\\Docs\\TrackMania\\", "C:\\Game\\") == "C:\\Docs\\TrackMania\\Tracks\\Challenges\\", "Forever maps folder");
        check(mapsFolder(true, "", "C:\\Game\\") == "C:\\Game\\GameData\\Tracks\\Challenges\\", "Forever without user folder falls back");
        check(mapsFolder(false, "C:\\Docs\\", "C:\\Game\\") == "C:\\Game\\GameData\\Tracks\\Challenges\\", "legacy maps folder");
        check(downloadFolder(false, "", "C:\\Game") == "C:\\Game\\GameData\\Tracks\\Challenges\\Downloaded\\_Horizon\\", "download folder (missing slash added)");

        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        const std::string base = std::string(tmp) + "tmxb_unit_" + std::to_string(GetCurrentProcessId()) + "\\a\\b\\";
        const std::string file = base + "x\xC3\xA4.bin";
        check(createDirs(file) && writeFile(file, "hello", 5), "createDirs + writeFile (UTF-8 name)");
        std::string back;
        check(readFile(file, back) && back == "hello", "readFile");
        check(!readFile(base + "missing.bin", back), "readFile of a missing file fails");
        std::string cmd = "rd /s /q \"" + std::string(tmp) + "tmxb_unit_" + std::to_string(GetCurrentProcessId()) + "\"";
        system(cmd.c_str());
    }

    // ---- favourites
    {
        std::vector<FavEntry> favs(2);
        favs[0].t.id = 12;
        favs[0].t.name = "Quote \" and \\ backslash \xC3\xA4";
        favs[0].t.author = "me";
        favs[0].t.awards = 5;
        favs[0].t.authorTime = 31337;
        favs[0].t.tags = {"Race", "Tech"};
        favs[0].t.difficulty = "Expert";
        favs[0].game = "tmnf";
        favs[0].added = "2026-11-11";
        favs[1].t.id = 99;
        favs[1].t.name = "Second";
        favs[1].game = "tmuf";
        favs[1].t.thumbKind = -1;
        favs[1].t.thumbnail = false;
        const std::string js = favouritesToJson(favs);
        std::vector<FavEntry> back;
        check(parseFavourites(js, back) && back.size() == 2, "favourites round trip: count");
        check(back.size() == 2 && back[0].t.name == favs[0].t.name && back[0].t.tags == favs[0].t.tags && back[0].t.authorTime == 31337 && back[0].game == "tmnf" &&
                  back[0].added == "2026-11-11" && back[0].t.difficulty == "Expert" && back[1].t.id == 99 && back[1].game == "tmuf" &&
                  back[1].t.thumbKind == -1 && !back[1].t.thumbnail && back[0].t.thumbKind == 0 && back[0].t.thumbnail,
              "favourites round trip: fields");
        check(!parseFavourites("{broken", back) && back.empty(), "broken favourites file rejected");
        check(parseFavourites("{\"favourites\":[]}", back) && back.empty(), "empty favourites");
    }

    // ---- LRU
    {
        Lru<int> lru(3);
        std::vector<int> ev;
        lru.put(1, 10, &ev);
        lru.put(2, 20, &ev);
        lru.put(3, 30, &ev);
        check(ev.empty() && lru.size() == 3, "LRU fills up to capacity");
        lru.get(1); // 1 is now the freshest, 2 the oldest
        lru.put(4, 40, &ev);
        check(ev.size() == 1 && ev[0] == 20 && lru.peek(2) == nullptr && lru.peek(1) && lru.peek(4), "LRU evicts the least recently used");
        ev.clear();
        lru.put(1, 11, &ev);
        check(ev.size() == 1 && ev[0] == 10 && lru.size() == 3 && *lru.peek(1) == 11, "LRU replacing a key hands back the old value");
        ev.clear();
        lru.clear(&ev);
        check(ev.size() == 3 && lru.size() == 0, "LRU clear hands back everything");
        Lru<int> big(48);
        for (int i = 0; i < 200; i++) big.put(i, i, nullptr);
        check(big.size() == 48 && big.peek(199) && !big.peek(151) && big.peek(152), "LRU of 48 keeps the newest 48");
    }

    // ---- thumbnails
    {
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        const std::string wide = bmp(600, 300);
        check(decodeThumb(wide.data(), wide.size(), px, w, h, 256) && w == 256 && h == 128 && px.size() == 256u * 128u * 4u, "BMP 600x300 shrunk to 256x128");
        check(px.size() >= 4 && px[0] == 50 && px[1] == 100 && px[2] == 200 && px[3] == 255, "pixels are RGBA");
        const std::string small = bmp(40, 20);
        check(decodeThumb(small.data(), small.size(), px, w, h, 256) && w == 40 && h == 20, "small image kept as is");
        check(!decodeThumb("not an image at all", 19, px, w, h, 256), "garbage is not decoded");
        check(!decodeThumb(nullptr, 0, px, w, h, 256), "null input is not decoded");
    }

    // ---- thumbnail cache prune
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        const std::string dir = std::string(tmp) + "tmxb_cache_" + std::to_string(GetCurrentProcessId()) + "\\";
        for (int i = 0; i < 10; i++) {
            const std::string f = dir + "tmo-" + std::to_string(i) + ".img";
            createDirs(f);
            writeFile(f, "x", 1);
            HANDLE h = CreateFileA(f.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            FILETIME ft = {static_cast<DWORD>(1000000 * (i + 1)), 30000000};
            SetFileTime(h, nullptr, nullptr, &ft);
            CloseHandle(h);
        }
        pruneCache(dir, 4);
        int left = 0;
        std::string b;
        for (int i = 0; i < 10; i++) left += readFile(dir + "tmo-" + std::to_string(i) + ".img", b) ? 1 : 0;
        check(left == 4 && readFile(dir + "tmo-9.img", b) && !readFile(dir + "tmo-0.img", b), "cache prune keeps the newest files");
        system(("rd /s /q \"" + dir + "\"").c_str());
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
