#include "tmx_logic.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ckui.h"
#include "horizon_plugin.h"
#include "common/json.h"

namespace tmx {

const TagDef kTags[] = {{0, "Race"},          {1, "Stunt"},     {2, "Maze"},     {3, "Offroad"},       {4, "Multilap"},
                        {5, "FullSpeed"},     {6, "LOL"},       {7, "Tech"},     {8, "SpeedTech"},     {9, "RPG"},
                        {10, "PressForward"}, {11, "Trial"},    {12, "Grass"},   {13, "Story"},        {14, "Nascar"},
                        {15, "Speedfun"},     {16, "Endurance"}, {17, "Altered Nadeo"}, {18, "Transitional"}};
const int kTagCount = static_cast<int>(sizeof(kTags) / sizeof(kTags[0]));
const char* const kDifficulties[4] = {"Beginner", "Intermediate", "Expert", "Lunatic"};

const LengthPreset kLengths[] = {{"Any length", -1, -1}, {"< 30 s", -1, 30000}, {"30-60 s", 30000, 60000}, {"1-2 min", 60000, 120000}, {"> 2 min", 120000, -1}};
const int kLengthCount = static_cast<int>(sizeof(kLengths) / sizeof(kLengths[0]));

// ---------------------------------------------------------------- queries
std::string tagList(uint32_t mask) {
    std::string o;
    for (int i = 0; i < kTagCount; i++)
        if (mask & (1u << kTags[i].id)) {
            if (!o.empty()) o += ',';
            o += std::to_string(kTags[i].id);
        }
    return o;
}

std::string tagSummary(uint32_t mask) {
    std::string o;
    for (int i = 0; i < kTagCount; i++)
        if (mask & (1u << kTags[i].id)) {
            if (!o.empty()) o += ", ";
            o += kTags[i].name;
        }
    return o;
}

std::string siteHost(const std::string& site) {
    if (site == "tmn") return "nations.tm-exchange.com";
    if (site == "tms") return "sunrise.tm-exchange.com";
    if (site == "tmo") return "original.tm-exchange.com";
    if (site == "tmuf") return "tmuf.exchange";
    return "tmnf.exchange";
}
std::string trackUrl(const std::string& site, int id) { return "https://" + siteHost(site) + "/trackshow/" + std::to_string(id); }

// ---------------------------------------------------------------- answers
bool parseTrack(const void* jsonValue, Track& t) {
    const horizon::Json* j = static_cast<const horizon::Json*>(jsonValue);
    if (!j || j->type != horizon::Json::Object) return false;
    const horizon::Json* id = j->get("id");
    if (!id || !id->isNumber()) return false;
    t = Track();
    t.id = static_cast<int>(id->asInt());
    if (const horizon::Json* v = j->get("name")) t.name = ck::stripCodes(v->asString());
    if (t.name.empty()) t.name = "Map " + std::to_string(t.id);
    if (const horizon::Json* v = j->get("author")) t.author = v->asString();
    if (const horizon::Json* v = j->get("awards")) t.awards = static_cast<int>(v->asInt());
    if (const horizon::Json* v = j->get("authorTime")) t.authorTime = static_cast<int>(v->asInt(-1));
    if (const horizon::Json* v = j->get("environment")) t.environment = v->asString();
    if (const horizon::Json* v = j->get("uploaded")) t.uploaded = v->asString();
    if (const horizon::Json* v = j->get("uid")) t.uid = v->asString();
    if (const horizon::Json* v = j->get("thumbnail")) t.thumbnail = v->type != horizon::Json::Bool || v->b;
    if (const horizon::Json* v = j->get("thumb")) {
        t.thumbKind = static_cast<int>(v->asInt(0));
        if (t.thumbKind < -1 || t.thumbKind > 2) t.thumbKind = 0;
        t.thumbnail = t.thumbKind >= 0;
    }
    if (const horizon::Json* v = j->get("difficulty")) {
        if (v->type == horizon::Json::String) t.difficulty = v->str;
        else if (v->isNumber() && v->asInt() >= 0 && v->asInt() <= 3) t.difficulty = kDifficulties[v->asInt()];
    }
    if (const horizon::Json* v = j->get("tags"))
        if (v->type == horizon::Json::Array)
            for (const horizon::Json& e : v->arr) {
                if (e.type == horizon::Json::String) t.tags.push_back(e.str);
                else if (e.type == horizon::Json::Object)
                    if (const horizon::Json* n = e.get("name")) t.tags.push_back(n->asString());
            }
    return true;
}

std::string playResultText(int r) {
    switch (r) {
    case HORIZON_OK: return "Map started";
    case HORIZON_ERR_NOT_SUPPORTED: return "Not supported here";
    case HORIZON_ERR_WRONG_STATE: return "Main menu only";
    case HORIZON_ERR_BLOCKED_ONLINE: return "Leave the server first";
    case HORIZON_ERR_INVALID_ARG: return "Game refused the file";
    case HORIZON_ERR_BUSY: return "Busy, try again";
    default: return "Could not start the map";
    }
}

// ---------------------------------------------------------------- files
std::string sanitizeFileName(const std::string& name, size_t maxChars) {
    const std::string in = ck::stripCodes(name);
    std::string out;
    bool lastSpace = true;
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7f || strchr("\\/:*?\"<>|", c)) c = ' ';
        if (c == ' ') {
            if (lastSpace) continue;
            lastSpace = true;
        } else {
            lastSpace = false;
        }
        out += static_cast<char>(c);
    }
    // cut at maxChars characters without splitting a UTF-8 sequence
    size_t chars = 0, i = 0;
    while (i < out.size()) {
        if ((static_cast<unsigned char>(out[i]) & 0xC0) != 0x80) {
            if (chars == maxChars) break;
            chars++;
        }
        i++;
    }
    out.resize(i);
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    size_t s = 0;
    while (s < out.size() && (out[s] == ' ' || out[s] == '.')) s++;
    out.erase(0, s);
    return out.empty() ? std::string("map") : out;
}

std::string mapFileName(const std::string& name, int id) { return sanitizeFileName(name) + " (" + std::to_string(id) + ").Challenge.Gbx"; }

static std::string withSlash(std::string s) {
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s += '\\';
    return s;
}

std::string mapsFolder(bool forever, const std::string& userFolder, const std::string& gameFolder) {
    if (forever && !userFolder.empty()) return withSlash(userFolder) + "Tracks\\Challenges\\";
    return withSlash(gameFolder) + "GameData\\Tracks\\Challenges\\";
}

std::string downloadFolder(bool forever, const std::string& userFolder, const std::string& gameFolder) {
    return mapsFolder(forever, userFolder, gameFolder) + "Downloaded\\_Horizon\\";
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

bool createDirs(const std::string& pathUtf8) {
    std::wstring w = widen(pathUtf8);
    for (wchar_t& c : w)
        if (c == L'/') c = L'\\';
    // only the part up to the last separator is a folder
    const size_t last = w.find_last_of(L'\\');
    if (last == std::wstring::npos) return true;
    w.resize(last);
    for (size_t i = 1; i <= w.size(); i++) {
        if (i != w.size() && w[i] != L'\\') continue;
        const std::wstring part = w.substr(0, i);
        if (part.back() == L':' || part.back() == L'\\') continue; // drive or share root
        if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            const DWORD attr = GetFileAttributesW(part.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
        }
    }
    const DWORD attr = GetFileAttributesW(w.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool writeFile(const std::string& pathUtf8, const void* data, size_t len) {
    const std::wstring w = widen(pathUtf8);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = len == 0 || (WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr) && written == len);
    CloseHandle(h);
    return ok;
}

bool readFile(const std::string& pathUtf8, std::string& out) {
    const std::wstring w = widen(pathUtf8);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[16384];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(h);
    return true;
}

// ---------------------------------------------------------------- favourites
static horizon::Json jStr(const std::string& s) {
    horizon::Json j;
    j.type = horizon::Json::String;
    j.str = s;
    return j;
}
static horizon::Json jNum(double n) {
    horizon::Json j;
    j.type = horizon::Json::Number;
    j.num = n;
    return j;
}

std::string favouritesToJson(const std::vector<FavEntry>& favs) {
    horizon::Json root;
    root.type = horizon::Json::Object;
    root.obj.emplace_back("version", jNum(1));
    horizon::Json arr;
    arr.type = horizon::Json::Array;
    for (const FavEntry& f : favs) {
        horizon::Json o;
        o.type = horizon::Json::Object;
        o.obj.emplace_back("id", jNum(f.t.id));
        o.obj.emplace_back("game", jStr(f.game));
        o.obj.emplace_back("name", jStr(f.t.name));
        o.obj.emplace_back("author", jStr(f.t.author));
        o.obj.emplace_back("awards", jNum(f.t.awards));
        o.obj.emplace_back("authorTime", jNum(f.t.authorTime));
        o.obj.emplace_back("difficulty", jStr(f.t.difficulty));
        horizon::Json tags;
        tags.type = horizon::Json::Array;
        for (const std::string& t : f.t.tags) tags.arr.push_back(jStr(t));
        o.obj.emplace_back("tags", std::move(tags));
        o.obj.emplace_back("environment", jStr(f.t.environment));
        o.obj.emplace_back("uid", jStr(f.t.uid));
        o.obj.emplace_back("uploaded", jStr(f.t.uploaded));
        o.obj.emplace_back("thumb", jNum(f.t.thumbKind));
        o.obj.emplace_back("added", jStr(f.added));
        arr.arr.push_back(std::move(o));
    }
    root.obj.emplace_back("favourites", std::move(arr));
    std::string out;
    horizon::jsonWrite(out, root);
    out += "\n";
    return out;
}

bool parseFavourites(const std::string& text, std::vector<FavEntry>& out) {
    out.clear();
    horizon::Json root;
    std::string err;
    if (!horizon::JsonParser::parse(text, root, err)) return false;
    const horizon::Json* arr = root.type == horizon::Json::Array ? &root : root.get("favourites");
    if (!arr || arr->type != horizon::Json::Array) return false;
    for (const horizon::Json& e : arr->arr) {
        FavEntry f;
        if (!parseTrack(&e, f.t)) continue;
        if (const horizon::Json* v = e.get("game")) f.game = v->asString();
        if (const horizon::Json* v = e.get("added")) f.added = v->asString();
        out.push_back(std::move(f));
    }
    return true;
}

} // namespace tmx
