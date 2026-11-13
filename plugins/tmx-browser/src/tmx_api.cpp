// TMX Browser: the TMX API v2 calls the plugin makes itself (URLs, answer parsing, map file check).
// No network and no ImGui here, so the unit tests cover all of it.
#include "tmx_logic.h"

#include <cstring>

#include "ckui.h"
#include "common/json.h"

namespace tmx {

namespace {
const char* const kEnvironments[] = {"", "Snow", "Desert", "Rally", "Island", "Coast", "Bay", "Stadium"};
const char kFields[] = "TrackId,TrackName,Uploader.Name,Authors,Awards,Tags,Difficulty,AuthorTime,Environment,HasThumbnail,Images,UId,UploadedAt";

void addFilters(std::string& q, const Form& f, bool text) {
    if (text && !f.name.empty()) q += "&name=" + ck::urlEscape(f.name);
    if (text && !f.author.empty()) q += "&author=" + ck::urlEscape(f.author);
    if (f.tagMask) q += "&tag=" + tagList(f.tagMask);
    if (f.difficulty >= 0 && f.difficulty <= 3) q += "&difficulty=" + std::to_string(f.difficulty);
    if (f.length > 0 && f.length < kLengthCount) {
        const LengthPreset& l = kLengths[f.length];
        if (l.minMs >= 0) q += "&authortimemin=" + std::to_string(l.minMs);
        if (l.maxMs >= 0) q += "&authortimemax=" + std::to_string(l.maxMs);
    }
}

std::string base(const std::string& site) { return "https://" + siteHost(site); }
} // namespace

bool knownSite(const std::string& site) { return site == "tmn" || site == "tms" || site == "tmo" || site == "tmnf" || site == "tmuf"; }
bool siteForever(const std::string& site) { return site == "tmnf" || site == "tmuf"; }

std::string apiSearchUrl(const std::string& site, const Form& f, int count, int afterId) {
    std::string q = base(site) + "/api/tracks?fields=" + ck::urlEscape(kFields) + "&count=" + std::to_string(count < 1 ? 1 : count > 100 ? 100 : count) +
                    "&inunlimiter=0";
    addFilters(q, f, true);
    if (afterId > 0) q += "&after=" + std::to_string(afterId);
    return q;
}

std::string apiRandomUrl(const std::string& site, const Form& f) {
    std::string q = base(site) + "/api/tracks?fields=" + ck::urlEscape(kFields) + "&count=100&inunlimiter=0";
    addFilters(q, f, false);
    return q;
}

std::string imageUrl(const std::string& site, const Track& t) {
    // Forever maps: the map's own thumbnail (a few KB). Old sites: the first screenshot (image/1).
    if (t.thumbKind == 1) return base(site) + "/trackshow/" + std::to_string(t.id) + "/thumbnail";
    return base(site) + "/trackshow/" + std::to_string(t.id) + "/image/1";
}
std::string gbxUrl(const std::string& site, int id) { return base(site) + "/trackgbx/" + std::to_string(id); }
std::string playUrl(const std::string& site, int id) { return base(site) + "/trackplay/" + std::to_string(id); }

bool parseApiTrack(const void* jsonValue, Track& t) {
    const horizon::Json* j = static_cast<const horizon::Json*>(jsonValue);
    if (!j || j->type != horizon::Json::Object) return false;
    const horizon::Json* id = j->get("TrackId");
    if (!id || !id->isNumber() || id->asInt() <= 0) return false;
    t = Track();
    t.id = static_cast<int>(id->asInt());
    if (const horizon::Json* v = j->get("TrackName")) t.name = ck::stripCodes(v->asString());
    if (t.name.empty()) t.name = "Map " + std::to_string(t.id);
    if (const horizon::Json* a = j->get("Authors"))
        if (a->type == horizon::Json::Array && !a->arr.empty())
            if (const horizon::Json* u = a->arr[0].get("User"))
                if (const horizon::Json* n = u->get("Name")) t.author = n->asString();
    if (t.author.empty())
        if (const horizon::Json* u = j->get("Uploader"))
            if (const horizon::Json* n = u->get("Name")) t.author = n->asString();
    if (const horizon::Json* v = j->get("Awards")) t.awards = static_cast<int>(v->asInt(0));
    if (const horizon::Json* v = j->get("AuthorTime")) t.authorTime = static_cast<int>(v->asInt(-1));
    if (const horizon::Json* v = j->get("Environment"))
        if (v->isNumber() && v->asInt() > 0 && v->asInt() < 8) t.environment = kEnvironments[v->asInt()];
    if (const horizon::Json* v = j->get("UploadedAt")) t.uploaded = v->asString();
    if (const horizon::Json* v = j->get("UId")) t.uid = v->asString();
    if (const horizon::Json* v = j->get("Difficulty"))
        if (v->isNumber() && v->asInt() >= 0 && v->asInt() <= 3) t.difficulty = kDifficulties[v->asInt()];
    if (const horizon::Json* v = j->get("Tags"))
        if (v->type == horizon::Json::Array)
            for (const horizon::Json& e : v->arr)
                if (e.isNumber()) {
                    const long long tag = e.asInt();
                    for (int i = 0; i < kTagCount; i++)
                        if (kTags[i].id == tag) t.tags.push_back(kTags[i].name);
                }
    // HasThumbnail is the picture inside the map file: Forever maps have one. The old sites (Nations, Sunrise,
    // Original) always say false there; their only pictures are uploaded screenshots ("Images").
    const horizon::Json* ht = j->get("HasThumbnail");
    const horizon::Json* im = j->get("Images");
    const bool gbxThumb = ht && ht->type == horizon::Json::Bool && ht->b;
    const bool images = im && im->type == horizon::Json::Array && !im->arr.empty();
    t.thumbKind = gbxThumb ? 1 : images ? 2 : -1;
    t.thumbnail = t.thumbKind > 0;
    return true;
}

bool parseApiTracks(const std::string& body, int minAwards, std::vector<Track>& out, bool& more, int& lastId, std::string& err) {
    out.clear();
    more = false;
    lastId = 0;
    horizon::Json root;
    std::string perr;
    if (!horizon::JsonParser::parse(body, root, perr) || root.type != horizon::Json::Object) {
        err = "Bad answer from TMX";
        return false;
    }
    const horizon::Json* r = root.get("Results");
    if (!r || r->type != horizon::Json::Array) {
        err = "Bad answer from TMX";
        return false;
    }
    if (const horizon::Json* m = root.get("More")) more = m->type == horizon::Json::Bool && m->b;
    for (const horizon::Json& e : r->arr) {
        Track t;
        if (!parseApiTrack(&e, t)) continue;
        lastId = t.id;
        if (t.awards >= minAwards) out.push_back(std::move(t));
    }
    return true;
}

bool tmtpLinkOk(const std::string& link) {
    static const char kPrefix[] = "tmtp:///:tmx:download:";
    if (link.size() > 1024 || link.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return false;
    for (unsigned char c : link)
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\') return false;
    return true;
}

bool checkMapFile(const void* data, size_t len, std::string& err) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    if (!p || len < 16) {
        err = "Not a map file";
        return false;
    }
    if (len > kMaxMapBytes) {
        err = "Map file too large";
        return false;
    }
    if (memcmp(p, "GBX", 3) != 0) {
        err = "Not a map file";
        return false;
    }
    const unsigned version = p[3] | (p[4] << 8);
    if (version < 3 || version > 6) {
        err = "Unknown map format";
        return false;
    }
    const size_t at = 5 + (version >= 4 ? 4 : 3); // format bytes "BUCR" (3 of them before version 4)
    if (len < at + 4) {
        err = "Not a map file";
        return false;
    }
    uint32_t cls = 0;
    memcpy(&cls, p + at, 4);
    if (cls != 0x03043000u && cls != 0x24003000u) {
        err = "Not a map file";
        return false;
    }
    return true;
}

bool allowedHost(const std::string& host) {
    std::string h = host;
    for (char& c : h)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return h == "tmnf.exchange" || h == "tmuf.exchange" || h == "nations.tm-exchange.com" || h == "sunrise.tm-exchange.com" ||
           h == "original.tm-exchange.com";
}

uint32_t fnv1a(const void* data, size_t len) {
    uint32_t h = 0x811c9dc5u;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; i++) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

bool isPlaceholderImage(const void* data, size_t len) { return len == 342471 && fnv1a(data, len) == 0xa966f9d7u; }

std::string netErrorText(int status) {
    if (status >= 200 && status < 300) return "";
    if (status == 404) return "Not found on TMX";
    if (status == 429) return "Too many requests";
    if (status == -2) return "TMX timed out";
    if (status <= 0) return "No connection to TMX";
    return "TMX error " + std::to_string(status);
}

std::string thumbCacheName(const std::string& site, int id) { return site + "-" + std::to_string(id) + ".img"; }

} // namespace tmx
