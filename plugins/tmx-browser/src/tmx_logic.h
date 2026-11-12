// TMX Browser: the logic without ImGui (query builder, result parsing, favourites, file names, folders, LRU).
#pragma once

#include <cstdint>
#include <iterator>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tmx {

struct Track {
    int id = 0;
    std::string name, author, difficulty, environment, uploaded, uid;
    int awards = 0;
    int authorTime = -1; // ms, -1 unknown
    std::vector<std::string> tags;
    bool thumbnail = true;
    // Which picture TMX has: 0 unknown (favourites saved by 0.1: ask image/1), 1 the map's own thumbnail
    // (Forever maps), 2 an uploaded screenshot (the only kind on Nations, Sunrise and Original), -1 none.
    int thumbKind = 0;
};

struct FavEntry {
    Track t;
    std::string game; // site code (tmnf ...)
    std::string added; // YYYY-MM-DD
};

// ---------------------------------------------------------------- search form
struct TagDef {
    int id;
    const char* name;
};
extern const TagDef kTags[];
extern const int kTagCount;
extern const char* const kDifficulties[4];

struct LengthPreset {
    const char* label;
    int minMs; // -1 none
    int maxMs; // -1 none
};
extern const LengthPreset kLengths[];
extern const int kLengthCount;

struct Form {
    std::string name, author;
    uint32_t tagMask = 0; // bit = TMX tag id
    int difficulty = -1;  // -1 any, 0..3
    int length = 0;       // index into kLengths, 0 = any
    int awards = 0;
};

std::string tagList(uint32_t mask); // "0,2,5"
std::string tagSummary(uint32_t mask); // "Race, Maze"
// Host name of the exchange for a site code, and the track page URL.
std::string siteHost(const std::string& site);
std::string trackUrl(const std::string& site, int id);

// ---------------------------------------------------------------- answers
struct SearchResult {
    bool more = false;
    int page = 1;
    std::vector<Track> tracks;
};
bool parseTrack(const void* jsonValue, Track& out); // horizon::Json* (favourites.json entries)
// One short line for a HorizonResult of play_map_file.
std::string playResultText(int result);

// ---------------------------------------------------------------- TMX API (direct, tmx_api.cpp)
// The plugin talks to the exchange of the running game itself (TMX API v2, the same calls TM Horizon makes).
constexpr size_t kMaxMapBytes = 8u << 20;   // a .Challenge.Gbx larger than this is refused
constexpr size_t kMaxThumbBytes = 3u << 20; // a picture larger than this is skipped
constexpr size_t kMaxJsonBytes = 2u << 20;
constexpr int kPageSize = 20;
bool knownSite(const std::string& site);
bool siteForever(const std::string& site); // tmnf, tmuf: maps carry their own thumbnail, Play goes through the game
// https://<host>/api/tracks?fields=...&count=..&inunlimiter=0[&name=..&author=..&tag=..&difficulty=..
// &authortimemin=..&authortimemax=..][&after=<last id of the page before>]. Awards are filtered here, not by TMX.
std::string apiSearchUrl(const std::string& site, const Form& f, int count, int afterId);
std::string apiRandomUrl(const std::string& site, const Form& f); // 100 maps with the filters (no name/author), one is picked
std::string imageUrl(const std::string& site, const Track& t);    // the small picture of a map
std::string gbxUrl(const std::string& site, int id);              // the map file
std::string playUrl(const std::string& site, int id);             // Forever: answers 302 to a tmtp:// link for the game
// TMX API v2 answer -> tracks with at least minAwards awards. lastId: the last TrackId of the page (also of
// filtered ones), the cursor for the next page. more: TMX has more.
bool parseApiTracks(const std::string& body, int minAwards, std::vector<Track>& out, bool& more, int& lastId, std::string& err);
bool parseApiTrack(const void* jsonValue, Track& out); // one element of "Results" (horizon::Json*)
// The Location of /trackplay/<id> on tmnf.exchange / tmuf.exchange: tmtp:///:tmx:download:<site>?token=...
bool tmtpLinkOk(const std::string& link);
// A downloaded map: size, "GBX" header, version 3..6 and the challenge class (0x03043000, or 0x24003000 on
// the older games). err gets a short line.
bool checkMapFile(const void* data, size_t len, std::string& err);
// Only these hosts are fetched (also after redirects).
bool allowedHost(const std::string& host);
uint32_t fnv1a(const void* data, size_t len);
// TMX's "no picture" image (the grey TMX logo, served for maps without a screenshot on the old sites).
bool isPlaceholderImage(const void* data, size_t len);
// One short line for an HTTP status or a network error (status <= 0).
std::string netErrorText(int status);
// Thumbnail cache file name: "<site>-<id>.img"
std::string thumbCacheName(const std::string& site, int id);

// ---------------------------------------------------------------- files
// No \/:*?"<>|, no $-codes, no control chars, trimmed, at most maxChars characters (UTF-8 aware).
std::string sanitizeFileName(const std::string& name, size_t maxChars = 80);
// "<safe name> (<id>).Challenge.Gbx"
std::string mapFileName(const std::string& name, int id);
// Folder the game lists its maps from, trailing backslash.
std::string mapsFolder(bool forever, const std::string& userFolder, const std::string& gameFolder);
// <maps folder>Downloaded\_Horizon\ (trailing backslash)
std::string downloadFolder(bool forever, const std::string& userFolder, const std::string& gameFolder);

bool createDirs(const std::string& pathUtf8); // creates every missing folder of a folder path or a file's folder
bool writeFile(const std::string& pathUtf8, const void* data, size_t len);
bool readFile(const std::string& pathUtf8, std::string& out);

// ---------------------------------------------------------------- favourites
std::string favouritesToJson(const std::vector<FavEntry>& favs);
bool parseFavourites(const std::string& text, std::vector<FavEntry>& out);

// ---------------------------------------------------------------- thumbnails
// Decodes any format stb_image reads and shrinks to at most maxWidth pixels wide. RGBA8, rows top to bottom.
bool decodeThumb(const void* data, size_t len, std::vector<uint8_t>& rgba, int& w, int& h, int maxWidth = 256);

// ---------------------------------------------------------------- LRU
// Keeps the most recently used values; put() hands back the ones that fell out so the owner can release them.
template <class V>
class Lru {
public:
    explicit Lru(size_t capacity) : cap_(capacity) {}
    V* get(int key) { // marks as used
        auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        items_.splice(items_.begin(), items_, it->second);
        return &items_.front().second;
    }
    const V* peek(int key) const {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second->second;
    }
    void put(int key, V value, std::vector<V>* evicted) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            if (evicted) evicted->push_back(std::move(it->second->second));
            items_.erase(it->second);
            map_.erase(it);
        }
        items_.emplace_front(key, std::move(value));
        map_[key] = items_.begin();
        while (items_.size() > cap_) {
            auto last = std::prev(items_.end());
            map_.erase(last->first);
            if (evicted) evicted->push_back(std::move(last->second));
            items_.pop_back();
        }
    }
    void clear(std::vector<V>* out) {
        if (out)
            for (auto& kv : items_) out->push_back(std::move(kv.second));
        items_.clear();
        map_.clear();
    }
    size_t size() const { return items_.size(); }
    size_t capacity() const { return cap_; }

private:
    size_t cap_;
    std::list<std::pair<int, V>> items_;
    std::unordered_map<int, typename std::list<std::pair<int, V>>::iterator> map_;
};

} // namespace tmx
