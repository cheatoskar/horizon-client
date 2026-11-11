#include "manifest.h"

#include <ctype.h>
#include <stdio.h>

#include "common/json.h"
#include "horizon_plugin.h"

namespace hs {
namespace {

const uint32_t kSupportedGames = HORIZON_GAME_ESWC | HORIZON_GAME_SUNRISE | HORIZON_GAME_TMNF | HORIZON_GAME_TMUF;

// One manifest value: a string or a list of strings (numbers and booleans are not used by any key).
struct Value {
    bool isList = false;
    std::string str;
    std::vector<std::string> list;
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string lineError(int line, const std::string& what) {
    char b[32];
    snprintf(b, sizeof(b), "line %d: ", line);
    return b + what;
}

// Applies one key. False with *error for a bad value; unknown keys only warn.
bool apply(const std::string& key, const Value& v, Manifest* m, std::string* error, std::vector<std::string>* seen) {
    for (const std::string& s : *seen)
        if (s == key) {
            *error = "\"" + key + "\" is set twice";
            return false;
        }
    seen->push_back(key);
    auto needString = [&](std::string* field) {
        if (v.isList) {
            *error = "\"" + key + "\" must be a text in quotes";
            return false;
        }
        *field = v.str;
        return true;
    };
    if (key == "name") return needString(&m->name);
    if (key == "author") return needString(&m->author);
    if (key == "version") return needString(&m->version);
    if (key == "description") return needString(&m->description);
    if (key == "online") {
        std::string s;
        if (!needString(&s)) return false;
        s = lower(s);
        if (s == "solo-only") m->online = OnlineMode::SoloOnly;
        else if (s == "online-ok") m->online = OnlineMode::OnlineOk;
        else {
            *error = "online must be \"solo-only\" or \"online-ok\", not \"" + v.str + "\"";
            return false;
        }
        return true;
    }
    if (key == "games") {
        const std::vector<std::string> items = v.isList ? v.list : std::vector<std::string>{v.str};
        uint32_t bits = 0;
        for (const std::string& g : items) {
            const uint32_t b = gameBits(lower(g));
            if (!b) {
                if (lower(g) == "original") {
                    m->warnings.push_back("TrackMania Original is not supported yet; it is ignored in games");
                    continue;
                }
                *error = "unknown game \"" + g + "\" (eswc, sunrise, tmnf, tmuf or all)";
                return false;
            }
            bits |= b;
        }
        if (!bits) {
            *error = "games lists no supported game";
            return false;
        }
        m->games = bits;
        return true;
    }
    if (key == "permissions") {
        const std::vector<std::string> items = v.isList ? v.list : std::vector<std::string>{v.str};
        uint32_t bits = 0;
        for (const std::string& p : items) {
            const uint32_t b = permBit(lower(p));
            if (!b) {
                *error = "unknown permission \"" + p + "\" (ui, settings, read-game, game-control)";
                return false;
            }
            bits |= b;
        }
        m->perms = bits;
        return true;
    }
    m->warnings.push_back("unknown key \"" + key + "\" is ignored");
    return true;
}

bool finish(Manifest* m, std::string* error) {
    if (m->name.empty()) {
        *error = "name is missing";
        return false;
    }
    if (m->name.size() > 64) {
        *error = "name is longer than 64 characters";
        return false;
    }
    if (m->version.empty()) m->version = "0.0.0";
    if (!m->games) m->games = kSupportedGames;
    return true;
}

// ---- TOML subset: key = "text" | 'text' | ["a", "b"] (may span lines) | bare word, # comments ----

struct TomlReader {
    const std::string& s;
    size_t pos = 0;
    int line = 1;

    explicit TomlReader(const std::string& t) : s(t) {}
    bool end() const { return pos >= s.size(); }
    void skipSpace() {
        while (!end() && (s[pos] == ' ' || s[pos] == '\t')) pos++;
    }
    void skipComment() {
        if (!end() && s[pos] == '#')
            while (!end() && s[pos] != '\n') pos++;
    }
    // Spaces, comments and line breaks (inside lists).
    void skipAll() {
        for (;;) {
            skipSpace();
            skipComment();
            if (!end() && (s[pos] == '\n' || s[pos] == '\r')) {
                if (s[pos] == '\n') line++;
                pos++;
                continue;
            }
            return;
        }
    }
    bool string(std::string* out, std::string* error) {
        const char q = s[pos++];
        out->clear();
        while (!end() && s[pos] != q) {
            char c = s[pos++];
            if (c == '\n') {
                *error = lineError(line, "text is not closed with " + std::string(1, q));
                return false;
            }
            if (c == '\\' && q == '"') {
                if (end()) break;
                const char e = s[pos++];
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                default:
                    *error = lineError(line, std::string("unknown escape \\") + e);
                    return false;
                }
            }
            *out += c;
        }
        if (end()) {
            *error = lineError(line, "text is not closed");
            return false;
        }
        pos++;
        return true;
    }
    bool scalar(std::string* out, std::string* error) {
        if (end()) {
            *error = lineError(line, "value is missing");
            return false;
        }
        if (s[pos] == '"' || s[pos] == '\'') return string(out, error);
        // bare value (true, false, numbers): kept as text
        out->clear();
        while (!end() && s[pos] != '\n' && s[pos] != '\r' && s[pos] != '#' && s[pos] != ',' && s[pos] != ']' && s[pos] != ' ' && s[pos] != '\t')
            *out += s[pos++];
        if (out->empty()) {
            *error = lineError(line, "value is missing");
            return false;
        }
        return true;
    }
    bool value(Value* v, std::string* error) {
        skipSpace();
        if (!end() && s[pos] == '[') {
            pos++;
            v->isList = true;
            for (;;) {
                skipAll();
                if (end()) {
                    *error = lineError(line, "list is not closed with ]");
                    return false;
                }
                if (s[pos] == ']') {
                    pos++;
                    return true;
                }
                std::string item;
                if (!scalar(&item, error)) return false;
                v->list.push_back(item);
                skipAll();
                if (!end() && s[pos] == ',') pos++;
                else if (!end() && s[pos] != ']') {
                    *error = lineError(line, "expected , or ] in the list");
                    return false;
                }
            }
        }
        return scalar(&v->str, error);
    }
};

} // namespace

uint32_t gameBits(const std::string& name) {
    if (name == "eswc") return HORIZON_GAME_ESWC;
    if (name == "sunrise") return HORIZON_GAME_SUNRISE;
    if (name == "tmnf") return HORIZON_GAME_TMNF;
    if (name == "tmuf") return HORIZON_GAME_TMUF;
    if (name == "forever") return HORIZON_GAME_TMNF | HORIZON_GAME_TMUF;
    if (name == "all") return kSupportedGames;
    return 0;
}

uint32_t permBit(const std::string& name) {
    if (name == "ui") return PermUi;
    if (name == "settings") return PermSettings;
    if (name == "read-game") return PermReadGame;
    if (name == "game-control") return PermGameControl;
    return 0;
}

const char* permName(uint32_t bit) {
    switch (bit) {
    case PermUi: return "ui";
    case PermSettings: return "settings";
    case PermReadGame: return "read-game";
    case PermGameControl: return "game-control";
    default: return "?";
    }
}

const char* permLabel(uint32_t bit) {
    switch (bit) {
    case PermUi: return "UI";
    case PermSettings: return "Settings";
    case PermReadGame: return "Read game";
    case PermGameControl: return "Game control";
    default: return "?";
    }
}

std::string gamesText(uint32_t games) {
    std::string t;
    const struct {
        uint32_t bit;
        const char* name;
    } names[] = {{HORIZON_GAME_ESWC, "ESWC"}, {HORIZON_GAME_SUNRISE, "Sunrise"}, {HORIZON_GAME_TMNF, "TMNF"}, {HORIZON_GAME_TMUF, "TMUF"}};
    for (const auto& n : names)
        if (games & n.bit) t += (t.empty() ? "" : ", ") + std::string(n.name);
    return t.empty() ? "none" : t;
}

bool parseManifestToml(const std::string& text, Manifest* out, std::string* error) {
    *out = Manifest();
    error->clear();
    TomlReader r(text);
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        r.pos = 3;
    std::vector<std::string> seen;
    for (;;) {
        r.skipAll();
        if (r.end()) break;
        const int keyLine = r.line;
        if (text[r.pos] == '[') {
            *error = lineError(keyLine, "sections ([...]) are not used in info.toml");
            return false;
        }
        std::string key;
        if (text[r.pos] == '"') {
            if (!r.string(&key, error)) return false;
        } else {
            while (!r.end() && (isalnum(static_cast<unsigned char>(text[r.pos])) || text[r.pos] == '_' || text[r.pos] == '-')) key += text[r.pos++];
        }
        if (key.empty()) {
            *error = lineError(keyLine, "expected a key like name = \"...\"");
            return false;
        }
        r.skipSpace();
        if (r.end() || text[r.pos] != '=') {
            *error = lineError(keyLine, "expected = after \"" + key + "\"");
            return false;
        }
        r.pos++;
        Value v;
        if (!r.value(&v, error)) return false;
        r.skipSpace();
        r.skipComment();
        if (!r.end() && text[r.pos] != '\n' && text[r.pos] != '\r') {
            *error = lineError(r.line, "unexpected text after the value of \"" + key + "\"");
            return false;
        }
        std::string e;
        if (!apply(lower(key), v, out, &e, &seen)) {
            *error = lineError(keyLine, e);
            return false;
        }
    }
    return finish(out, error);
}

bool parseManifestJson(const std::string& text, Manifest* out, std::string* error) {
    *out = Manifest();
    error->clear();
    horizon::Json j;
    std::string err;
    if (!horizon::JsonParser::parse(text, j, err)) {
        *error = "not valid JSON: " + err;
        return false;
    }
    if (j.type != horizon::Json::Object) {
        *error = "info.json must be an object { ... }";
        return false;
    }
    std::vector<std::string> seen;
    for (const auto& kv : j.obj) {
        Value v;
        if (kv.second.type == horizon::Json::Array) {
            v.isList = true;
            for (const horizon::Json& item : kv.second.arr) {
                if (item.type != horizon::Json::String) {
                    *error = "\"" + kv.first + "\" must be a list of texts";
                    return false;
                }
                v.list.push_back(item.str);
            }
        } else if (kv.second.type == horizon::Json::String) {
            v.str = kv.second.str;
        } else {
            out->warnings.push_back("\"" + kv.first + "\" is not a text or list and is ignored");
            continue;
        }
        if (!apply(lower(kv.first), v, out, error, &seen)) return false;
    }
    return finish(out, error);
}

} // namespace hs
