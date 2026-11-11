// Unit tests of the TMX Browser logic (no ImGui, no host). Plain main(), PASS/FAIL lines, exit code = failures.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tmx_logic.h"

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

    // ---- queries
    {
        Form f;
        f.name = "a b&c";
        f.author = "Wirtual";
        f.tagMask = (1u << 0) | (1u << 7);
        f.difficulty = 2;
        f.length = 2; // 30-60 s
        f.awards = 10;
        const std::string q = searchPath("tmnf", f, 2);
        check(q == "/v1/tmx/search?game=tmnf&name=a%20b%26c&author=Wirtual&tags=0,7&difficulty=2&lengthmin=30000&lengthmax=60000&awards=10&page=2", "search path with every filter");
        check(searchPath("tmuf", Form(), 0) == "/v1/tmx/search?game=tmuf&page=1", "empty form, page clamped to 1");
        Form g;
        g.length = 1;
        check(searchPath("tmn", g, 1).find("lengthmax=30000") != std::string::npos && searchPath("tmn", g, 1).find("lengthmin") == std::string::npos, "length < 30 s has only a maximum");
        g.length = 4;
        check(searchPath("tmn", g, 1).find("lengthmin=120000") != std::string::npos && searchPath("tmn", g, 1).find("lengthmax") == std::string::npos, "length > 2 min has only a minimum");
        const std::string r = randomPath("tms", f);
        check(r.find("name=") == std::string::npos && r.find("tags=0,7") != std::string::npos && r.compare(0, 24, "/v1/tmx/random?game=tms&") == 0, "random path keeps filters, drops text");
        check(thumbPath("tmnf", 12) == "/v1/tmx/thumb?game=tmnf&id=12" && mapPath("tmuf", 7) == "/v1/tmx/map?game=tmuf&id=7", "thumb and map paths");
        check(trackUrl("tmnf", 1000) == "https://tmnf.exchange/trackshow/1000", "track URL");
        check(tagSummary((1u << 2) | (1u << 8)) == "Maze, SpeedTech", "tag summary");
    }

    // ---- parsing
    {
        const std::string body =
            "{\"ok\":true,\"site\":\"tmnf.exchange\",\"page\":1,\"more\":true,\"results\":["
            "{\"id\":2233,\"name\":\"$o$f00A01-Race\",\"author\":\"Nadeo\",\"awards\":357,\"tags\":[\"Race\",\"Tech\"],\"difficulty\":\"Beginner\","
            "\"authorTime\":24540,\"environment\":\"Stadium\",\"uploaded\":\"2008-04-12T00:00:00Z\",\"thumbnail\":true,\"uid\":\"abc\"},"
            "{\"id\":5,\"name\":\"\",\"author\":\"x\",\"thumbnail\":false},{\"nope\":1}]}";
        SearchResult r;
        std::string err;
        const bool ok = parseSearch(body, r, err);
        check(ok && r.tracks.size() == 2 && r.more && r.page == 1, "search answer parsed (bad entry skipped)");
        check(ok && r.tracks[0].name == "A01-Race" && r.tracks[0].awards == 357 && r.tracks[0].authorTime == 24540 && r.tracks[0].tags.size() == 2 &&
                  r.tracks[0].difficulty == "Beginner" && r.tracks[0].uid == "abc",
              "track fields, $-codes stripped");
        check(ok && r.tracks[1].name == "Map 5" && !r.tracks[1].thumbnail && r.tracks[1].authorTime == -1, "defaults for missing fields");
        SearchResult bad;
        check(!parseSearch("{\"ok\":false,\"error\":\"rate\"}", bad, err) && err == "rate", "error answer reports its text");
        check(!parseSearch("not json", bad, err), "garbage rejected");
        Track t;
        bool found = true;
        check(parseRandom("{\"ok\":true,\"result\":null}", t, found, err) && !found, "random: null result");
        check(parseRandom("{\"ok\":true,\"result\":{\"id\":9,\"name\":\"R\"}}", t, found, err) && found && t.id == 9, "random: result");
    }

    // ---- messages
    {
        check(bridgeErrorText(200).empty(), "no error text for 200");
        check(bridgeErrorText(-3) == "Too many requests, wait a moment" && bridgeErrorText(429) == bridgeErrorText(-3), "rate limit text");
        check(bridgeErrorText(404) == "Not found on TMX" && bridgeErrorText(500).find("500") != std::string::npos, "http error texts");
        check(playResultText(-3) == "Playing is off while online" && playResultText(-2) == "Go back to the main menu to play" && playResultText(0) == "Map started", "play result texts");
        check(playResultText(-1).find("not supported") != std::string::npos, "play result: not supported");
        check(bridgeStateText(1) == "Start TM Horizon to search TMX" && bridgeStateText(2) == "Press Allow in TM Horizon" && bridgeStateText(3) == "Connected to TM Horizon", "bridge state texts");
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
        const std::string js = favouritesToJson(favs);
        std::vector<FavEntry> back;
        check(parseFavourites(js, back) && back.size() == 2, "favourites round trip: count");
        check(back.size() == 2 && back[0].t.name == favs[0].t.name && back[0].t.tags == favs[0].t.tags && back[0].t.authorTime == 31337 && back[0].game == "tmnf" &&
                  back[0].added == "2026-11-11" && back[0].t.difficulty == "Expert" && back[1].t.id == 99 && back[1].game == "tmuf",
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

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
