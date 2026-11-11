// Tiny JSON reader and writer (profile.json, plugin.json, settings). No exceptions, no dependencies.
// Reader from my legacy-randomizer repo (src/core/json.h, MIT); writer and BOM handling added here.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace horizon {

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* get(const char* key) const {
        if (type != Object) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool isNumber() const { return type == Number; }
    // numeric value or fallback (null / missing / wrong type)
    long long asInt(long long fallback = 0) const { return type == Number ? static_cast<long long>(num) : fallback; }
    std::string asString() const { return type == String ? str : std::string(); }
};

class JsonParser {
public:
    // Returns false and sets err on malformed input.
    static bool parse(const std::string& text, Json& out, std::string& err) {
        JsonParser p(text);
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
            p.pos_ = 3; // UTF-8 BOM (PowerShell/Notepad write one)
        p.skipWs();
        if (!p.value(out, 0)) { err = p.err_.empty() ? "invalid JSON" : p.err_; return false; }
        p.skipWs();
        if (p.pos_ != text.size()) { err = "trailing characters after JSON value"; return false; }
        return true;
    }

private:
    explicit JsonParser(const std::string& t) : s_(t) {}
    const std::string& s_;
    size_t pos_ = 0;
    std::string err_;

    bool fail(const char* m) { if (err_.empty()) err_ = m; return false; }
    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) ++pos_;
    }
    bool lit(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (s_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }
    static void putUtf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (pos_ + 4 > s_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected string");
        ++pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (pos_ >= s_.size()) break;
            char e = s_[pos_++];
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return fail("bad \\u escape");
                if (cp >= 0xD800 && cp < 0xDC00 && pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                    pos_ += 2;
                    unsigned lo;
                    if (!hex4(lo)) return fail("bad \\u escape");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                putUtf8(o, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(Json& out, int depth) {
        if (depth > 32) return fail("nesting too deep");
        if (pos_ >= s_.size()) return fail("unexpected end");
        char c = s_[pos_];
        if (c == '{') {
            ++pos_;
            out.type = Json::Object;
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            for (;;) {
                skipWs();
                std::string key;
                if (!string(key)) return false;
                skipWs();
                if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':'");
                ++pos_;
                skipWs();
                Json v;
                if (!value(v, depth + 1)) return false;
                out.obj.emplace_back(std::move(key), std::move(v));
                skipWs();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++pos_;
            out.type = Json::Array;
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            for (;;) {
                skipWs();
                Json v;
                if (!value(v, depth + 1)) return false;
                out.arr.push_back(std::move(v));
                skipWs();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { out.type = Json::String; return string(out.str); }
        if (lit("true")) { out.type = Json::Bool; out.b = true; return true; }
        if (lit("false")) { out.type = Json::Bool; out.b = false; return true; }
        if (lit("null")) { out.type = Json::Null; return true; }
        // number
        size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' || s_[pos_] == 'e' ||
                                    s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-'))
            ++pos_;
        if (pos_ == start) return fail("unexpected character");
        out.type = Json::Number;
        out.num = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }
};

// Writes a string as a JSON string literal (UTF-8 kept as is, control characters escaped).
inline void jsonQuote(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char* hex = "0123456789abcdef";
                out += "\\u00";
                out += hex[(c >> 4) & 0xf];
                out += hex[c & 0xf];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

// Pretty-prints a value (2 spaces), objects keep their key order.
inline void jsonWrite(std::string& out, const Json& v, int indent = 0) {
    const std::string pad(static_cast<size_t>(indent) * 2, ' ');
    const std::string pad2(static_cast<size_t>(indent + 1) * 2, ' ');
    switch (v.type) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += v.b ? "true" : "false"; break;
    case Json::Number: {
        char buf[64];
        if (v.num == static_cast<double>(static_cast<long long>(v.num))) snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.num));
        else snprintf(buf, sizeof(buf), "%.9g", v.num);
        out += buf;
        break;
    }
    case Json::String: jsonQuote(out, v.str); break;
    case Json::Array:
        if (v.arr.empty()) { out += "[]"; break; }
        out += "[\n";
        for (size_t i = 0; i < v.arr.size(); ++i) {
            out += pad2;
            jsonWrite(out, v.arr[i], indent + 1);
            out += i + 1 < v.arr.size() ? ",\n" : "\n";
        }
        out += pad + "]";
        break;
    case Json::Object:
        if (v.obj.empty()) { out += "{}"; break; }
        out += "{\n";
        for (size_t i = 0; i < v.obj.size(); ++i) {
            out += pad2;
            jsonQuote(out, v.obj[i].first);
            out += ": ";
            jsonWrite(out, v.obj[i].second, indent + 1);
            out += i + 1 < v.obj.size() ? ",\n" : "\n";
        }
        out += pad + "}";
        break;
    }
}

} // namespace horizon
