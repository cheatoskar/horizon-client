#include "gbx.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <windows.h>

namespace lr {

namespace {

struct Reader {
    const unsigned char* p;
    size_t n;
    size_t pos = 0;
    bool bad = false;

    Reader(const unsigned char* d, size_t sz) : p(d), n(sz) {}
    bool has(size_t k) const { return !bad && pos + k <= n; }
    uint32_t u32() {
        if (!has(4)) { bad = true; return 0; }
        uint32_t v = static_cast<uint32_t>(p[pos]) | static_cast<uint32_t>(p[pos + 1]) << 8 |
                     static_cast<uint32_t>(p[pos + 2]) << 16 | static_cast<uint32_t>(p[pos + 3]) << 24;
        pos += 4;
        return v;
    }
    unsigned u8() {
        if (!has(1)) { bad = true; return 0; }
        return p[pos++];
    }
    std::string str() {
        uint32_t len = u32();
        if (bad || len > 4096 || !has(len)) { bad = true; return std::string(); }
        std::string s(reinterpret_cast<const char*>(p + pos), len);
        pos += len;
        return s;
    }
};

int timeFromU32(uint32_t v) { return (v == 0xFFFFFFFFu || v > 0x7FFFFFFFu) ? 0 : static_cast<int>(v); }

bool timesSane(int bronze, int silver, int gold, int author) {
    // unvalidated maps may have an author time of 0 / -1, but the medal steps must be ordered
    if (bronze <= 0 || silver <= 0 || gold <= 0) return false;
    if (!(gold <= silver && silver <= bronze)) return false;
    return author <= 0 || author <= gold;
}

// 0x...3002: u8 version, u32 (0), bronze, silver, gold, author, cost, ...
bool parseTimesChunk(const unsigned char* d, size_t sz, GbxMapHeader& h) {
    Reader r(d, sz);
    r.u8();
    r.u32();
    int bronze = timeFromU32(r.u32()), silver = timeFromU32(r.u32()), gold = timeFromU32(r.u32());
    int author = timeFromU32(r.u32());
    int cost = static_cast<int>(r.u32());
    if (r.bad || !timesSane(bronze, silver, gold, author)) return false;
    h.bronzeMs = bronze; h.silverMs = silver; h.goldMs = gold; h.authorMs = author;
    h.costCoppers = cost;
    h.timesFromBinary = true;
    return true;
}

// Lookback string: u32 index; 0xFFFFFFFF = empty; flag bits set with zero index = a new string
// follows (and is remembered); otherwise a 1-based reference to an earlier string.
struct Lookback {
    std::vector<std::string> seen;
    bool started = false;
    std::string read(Reader& r) {
        if (!started) { r.u32(); started = true; }  // lookback version (3)
        uint32_t idx = r.u32();
        if (r.bad || idx == 0xFFFFFFFFu) return std::string();
        uint32_t low = idx & 0x3FFFFFFFu;
        if ((idx & 0xC0000000u) != 0 && low == 0) {
            std::string s = r.str();
            seen.push_back(s);
            return s;
        }
        if (low >= 1 && low <= seen.size()) return seen[low - 1];
        r.bad = true;
        return std::string();
    }
};

// 0x...3003: u8 version, ident (uid, collection, author as lookback strings), name (plain string)
bool parseInfoChunk(const unsigned char* d, size_t sz, GbxMapHeader& h) {
    Reader r(d, sz);
    r.u8();
    Lookback lb;
    std::string uid = lb.read(r);
    std::string coll = lb.read(r);
    std::string author = lb.read(r);
    std::string name = r.str();
    if (r.bad || uid.empty() || (name.empty() && author.empty())) return false;
    h.uid = uid; h.environment = coll; h.author = author; h.name = name;
    h.nameFromBinary = true;
    return true;
}

// 0x...3007: u32 version; version >= 1: u32 jpeg size, "<Thumbnail.jpg>", jpeg, "</Thumbnail.jpg>",
// "<Comments>", string, "</Comments>"
bool parseThumbChunk(const unsigned char* d, size_t sz, GbxMapHeader& h) {
    Reader r(d, sz);
    uint32_t ver = r.u32();
    if (r.bad || ver == 0) return false;
    uint32_t jpgSize = r.u32();
    static const char kOpen[] = "<Thumbnail.jpg>";
    const size_t openLen = sizeof kOpen - 1;
    if (r.bad || !r.has(openLen) || std::memcmp(d + r.pos, kOpen, openLen) != 0) return false;
    r.pos += openLen;
    if (jpgSize > sz - r.pos) return false;
    h.thumbnailJpeg.assign(reinterpret_cast<const char*>(d + r.pos), jpgSize);
    r.pos += jpgSize;
    static const char kClose[] = "</Thumbnail.jpg><Comments>";
    const size_t closeLen = sizeof kClose - 1;
    if (r.has(closeLen) && std::memcmp(d + r.pos, kClose, closeLen) == 0) {
        r.pos += closeLen;
        std::string c = r.str();
        if (!r.bad) h.comments = c;
    }
    return !h.thumbnailJpeg.empty();
}

// attribute value of the first <tag ... attr="..."> in xml; false when missing
bool xmlAttr(const std::string& xml, const char* tag, const char* attr, std::string& out) {
    std::string open = std::string("<") + tag;
    size_t t = xml.find(open);
    while (t != std::string::npos) {
        char after = t + open.size() < xml.size() ? xml[t + open.size()] : '\0';
        if (after == ' ' || after == '/' || after == '>') break;
        t = xml.find(open, t + 1);
    }
    if (t == std::string::npos) return false;
    size_t end = xml.find('>', t);
    if (end == std::string::npos) return false;
    std::string key = std::string(" ") + attr + "=\"";
    size_t a = xml.find(key, t);
    if (a == std::string::npos || a > end) return false;
    a += key.size();
    size_t q = xml.find('"', a);
    if (q == std::string::npos) return false;
    out = xml.substr(a, q - a);
    return true;
}

int xmlInt(const std::string& xml, const char* tag, const char* attr) {
    std::string v;
    if (!xmlAttr(xml, tag, attr, v) || v.empty()) return 0;
    long long x = std::strtoll(v.c_str(), nullptr, 10);
    return (x <= 0 || x > 0x7FFFFFFF) ? 0 : static_cast<int>(x);
}

void parseXml(const std::string& xml, GbxMapHeader& h) {
    std::string v;
    if (!h.timesFromBinary) {
        int b = xmlInt(xml, "times", "bronze"), s = xmlInt(xml, "times", "silver"), g = xmlInt(xml, "times", "gold");
        h.bronzeMs = b; h.silverMs = s; h.goldMs = g;
        h.authorMs = xmlInt(xml, "times", "authortime");
    }
    if (!h.nameFromBinary) {
        if (xmlAttr(xml, "ident", "uid", v)) h.uid = v;
        if (xmlAttr(xml, "ident", "name", v)) h.name = percentDecode(v);
        if (xmlAttr(xml, "ident", "author", v)) h.author = percentDecode(v);
    }
    if (h.environment.empty() && xmlAttr(xml, "desc", "envir", v)) h.environment = v;
    if (xmlAttr(xml, "desc", "mood", v)) h.mood = v;
    if (xmlAttr(xml, "desc", "type", v)) h.mapType = v;
    h.nbLaps = xmlInt(xml, "desc", "nblaps");
    if (h.costCoppers == 0) h.costCoppers = xmlInt(xml, "desc", "price");
}

} // namespace

std::string percentDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            o += static_cast<char>(std::strtol(hex, nullptr, 16));
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string stripTmCodes(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '$') { o += s[i]; continue; }
        if (i + 1 >= s.size()) break;
        char c = s[i + 1];
        if (c == '$') { o += '$'; ++i; continue; }
        auto hexd = [](char x) { return std::isxdigit(static_cast<unsigned char>(x)) != 0; };
        if (hexd(c)) {
            // $rgb: up to three hex digits
            size_t j = i + 1;
            for (int k = 0; k < 3 && j < s.size() && hexd(s[j]); ++k) ++j;
            i = j - 1;
        } else {
            ++i;  // single-letter code ($w, $o, $i, $s, $n, $z, ...)
        }
    }
    return o;
}

GbxMapHeader parseChallengeHeader(const unsigned char* data, size_t size) {
    GbxMapHeader h;
    auto fail = [&h](const char* m) { h.error = m; return h; };
    if (size < 21 || std::memcmp(data, "GBX", 3) != 0) return fail("not a GBX file");
    Reader r(data, size);
    r.pos = 3;
    unsigned ver = r.u8();
    ver |= r.u8() << 8;
    h.gbxVersion = static_cast<int>(ver);
    if (ver < 3 || ver > 6) return fail("unsupported GBX version");
    unsigned fmt = r.u8();
    if (fmt != 'B') return fail("GBX text format is not supported");
    if (ver >= 4) { r.u8(); r.u8(); r.u8(); }  // ref table / body compression, unknown flag
    else { r.u8(); r.u8(); }
    h.classId = r.u32();
    if ((h.classId & 0xFFF) != 0) return fail("not a map file (class id)");
    if (ver >= 6) {
        uint32_t userSize = r.u32();
        if (r.bad || userSize > size - r.pos) return fail("truncated GBX header");
    }
    uint32_t nChunks = r.u32();
    if (r.bad || nChunks == 0 || nChunks > 64) return fail("bad header chunk count");
    struct Ent { uint32_t id, size; };
    std::vector<Ent> ents;
    for (uint32_t i = 0; i < nChunks; ++i) {
        Ent e;
        e.id = r.u32();
        e.size = r.u32() & 0x7FFFFFFFu;
        ents.push_back(e);
    }
    if (r.bad) return fail("truncated GBX header table");
    std::string xml;
    bool haveTimes = false, haveInfo = false;
    for (const Ent& e : ents) {
        if (e.size > size - r.pos) return fail("truncated GBX header chunk");
        const unsigned char* d = data + r.pos;
        // chunk number is in the low 12 bits; the class part differs per game family
        uint32_t num = e.id & 0xFFF;
        if (num == 0x002) haveTimes = parseTimesChunk(d, e.size, h);
        else if (num == 0x003) haveInfo = parseInfoChunk(d, e.size, h);
        else if (num == 0x007) parseThumbChunk(d, e.size, h);
        else if (num == 0x005) {
            Reader xr(d, e.size);
            xml = xr.str();
        }
        r.pos += e.size;
    }
    if (!xml.empty()) parseXml(xml, h);
    (void)haveTimes; (void)haveInfo;
    if (h.uid.empty() && h.name.empty()) return fail("no map identity found in the header");
    h.ok = true;
    return h;
}

GbxMapHeader readChallengeHeaderFile(const std::string& path) {
    GbxMapHeader h;
    FILE* fp = nullptr;
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, wpath, 1024) > 0) _wfopen_s(&fp, wpath, L"rb");
    else fopen_s(&fp, path.c_str(), "rb");
    if (!fp) { h.error = "cannot open " + path; return h; }
    // the header is always at the start of the file; 256 KiB is far more than needed
    std::vector<unsigned char> buf(256 * 1024);
    size_t n = std::fread(buf.data(), 1, buf.size(), fp);
    std::fclose(fp);
    return parseChallengeHeader(buf.data(), n);
}

} // namespace lr
