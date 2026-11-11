#include "values.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace nx {
namespace {

struct Family {
    uint8_t first, slots; // slots: 1 (single), 4 (single/buffer/cat/array), 5 (+ range)
    Kind kind;
};
// ESWC / Sunrise numbering. Forever: 27..30 = Id, everything from 31 on = legacy number + 4.
const Family kLegacy[] = {
    {0, 1, Kind::Proc},     {1, 4, Kind::Bool},     {5, 4, Kind::Nod},      {9, 4, Kind::Color},
    {13, 1, Kind::Enum},    {14, 5, Kind::Int},     {19, 4, Kind::Iso4},    {23, 4, Kind::Iso3},
    {27, 5, Kind::Natural}, {32, 5, Kind::Real},    {37, 4, Kind::String},  {41, 4, Kind::WString},
    {45, 4, Kind::Vec2},    {49, 4, Kind::Vec3},    {53, 4, Kind::Vec4},    {57, 4, Kind::Int3},
    {61, 1, Kind::ProcEx},
};

TypeInfo fromLegacy(uint32_t t) {
    TypeInfo r;
    for (const Family& f : kLegacy) {
        if (t < f.first || t >= static_cast<uint32_t>(f.first) + f.slots) continue;
        r.kind = f.kind;
        const uint32_t i = t - f.first;
        r.shape = i == 0 ? Shape::Single : i == 1 ? Shape::Buffer : i == 2 ? Shape::BufferCat : i == 3 ? Shape::Array : Shape::Range;
        return r;
    }
    return r;
}

std::string printf1(const char* fmt, double v) {
    char b[64];
    snprintf(b, sizeof(b), fmt, v);
    return b;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

void appendEscaped(std::string& out, uint32_t cp) {
    switch (cp) {
    case '\n': out += "\\n"; return;
    case '\r': out += "\\r"; return;
    case '\t': out += "\\t"; return;
    case '"': out += "\\\""; return;
    case '\\': out += "\\\\"; return;
    default: break;
    }
    if (cp < 0x20 || cp == 0x7f) {
        char b[8];
        snprintf(b, sizeof(b), "\\x%02X", cp);
        out += b;
        return;
    }
    appendUtf8(out, cp);
}

} // namespace

TypeInfo classify(uint32_t type, Engine engine) {
    if (engine == Engine::Forever && type >= 27) {
        if (type <= 30) {
            TypeInfo r;
            r.kind = Kind::Id;
            r.shape = type == 27 ? Shape::Single : type == 28 ? Shape::Buffer : type == 29 ? Shape::BufferCat : Shape::Array;
            return r;
        }
        return fromLegacy(type - 4);
    }
    return fromLegacy(type);
}

const char* kindName(Kind k) {
    switch (k) {
    case Kind::Proc: return "method";
    case Kind::ProcEx: return "method";
    case Kind::Bool: return "bool";
    case Kind::Nod: return "nod";
    case Kind::Color: return "color";
    case Kind::Enum: return "enum";
    case Kind::Int: return "int";
    case Kind::Iso4: return "iso4";
    case Kind::Iso3: return "iso3";
    case Kind::Id: return "id";
    case Kind::Natural: return "uint";
    case Kind::Real: return "float";
    case Kind::String: return "string";
    case Kind::WString: return "wstring";
    case Kind::Vec2: return "vec2";
    case Kind::Vec3: return "vec3";
    case Kind::Vec4: return "vec4";
    case Kind::Int3: return "int3";
    default: return "?";
    }
}

std::string typeLabel(TypeInfo t, const char* nodClass) {
    std::string base = (t.kind == Kind::Nod && nodClass && *nodClass) ? nodClass : kindName(t.kind);
    switch (t.shape) {
    case Shape::Buffer:
    case Shape::BufferCat: return base + " list";
    case Shape::Array: return base + " array";
    case Shape::Range: return base + " range";
    default: return base;
    }
}

size_t elemSize(Kind k) {
    switch (k) {
    case Kind::Bool:
    case Kind::Nod:
    case Kind::Enum:
    case Kind::Int:
    case Kind::Id:
    case Kind::Natural:
    case Kind::Real: return 4;
    case Kind::Color: return 12;
    case Kind::Iso4: return 48;
    case Kind::Iso3: return 24;
    case Kind::String:
    case Kind::WString: return 8; // {length, pointer}
    case Kind::Vec2: return 8;
    case Kind::Vec3: return 12;
    case Kind::Vec4: return 16;
    case Kind::Int3: return 12;
    default: return 0;
    }
}

bool isMethod(Kind k) { return k == Kind::Proc || k == Kind::ProcEx; }
bool isList(Shape s) { return s == Shape::Buffer || s == Shape::BufferCat; }

std::string fmtFloat(float v) {
    if (isnan(v)) return "nan";
    if (isinf(v)) return v < 0 ? "-inf" : "inf";
    if (v == 0.0f) return "0"; // also -0
    return printf1("%.6g", v);
}

std::string fmtFloats(const float* v, int n) {
    std::string s;
    for (int i = 0; i < n; i++) {
        if (i) s += ", ";
        s += fmtFloat(v[i]);
    }
    return s;
}

std::string fmtBool(uint32_t v) {
    if (v == 0) return "false";
    if (v == 1) return "true";
    return fmtUint(v) + " (bool?)";
}

std::string fmtInt(int32_t v) { return std::to_string(v); }
std::string fmtUint(uint32_t v) { return std::to_string(v); }

std::string fmtHex(uint32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

std::string fmtPtr(uintptr_t p) { return p ? fmtHex(static_cast<uint32_t>(p)) : "null"; }

std::string fmtText(const char* s, size_t n, size_t max) {
    std::string out = "\"";
    const size_t m = n < max ? n : max;
    for (size_t i = 0; i < m; i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) out += static_cast<char>(c); // UTF-8 bytes pass through
        else appendEscaped(out, c);
    }
    out += "\"";
    if (n > max) out += "...";
    return out;
}

std::string fmtWText(const wchar_t* s, size_t n, size_t max) {
    std::string out = "\"";
    const size_t m = n < max ? n : max;
    for (size_t i = 0; i < m; i++) {
        uint32_t cp = static_cast<uint16_t>(s[i]);
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < m) {
            const uint32_t lo = static_cast<uint16_t>(s[i + 1]);
            if (lo >= 0xdc00 && lo <= 0xdfff) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                i++;
            } else {
                cp = '?';
            }
        } else if (cp >= 0xd800 && cp <= 0xdfff) {
            cp = '?';
        }
        appendEscaped(out, cp);
    }
    out += "\"";
    if (n > max) out += "...";
    return out;
}

std::string fmtValue(Kind k, const void* raw, size_t n) {
    if (!raw || n < elemSize(k) || elemSize(k) == 0) return "?";
    uint32_t u = 0;
    memcpy(&u, raw, 4);
    float f[12] = {};
    memcpy(f, raw, elemSize(k) <= sizeof(f) ? elemSize(k) : sizeof(f));
    switch (k) {
    case Kind::Bool: return fmtBool(u);
    case Kind::Nod: return fmtPtr(u);
    case Kind::Enum: return fmtUint(u);
    case Kind::Int: return fmtInt(static_cast<int32_t>(u));
    case Kind::Id: return fmtHex(u);
    case Kind::Natural: return fmtUint(u);
    case Kind::Real: return fmtFloat(f[0]);
    case Kind::Color: return fmtFloats(f, 3);
    case Kind::Vec2: return fmtFloats(f, 2);
    case Kind::Vec3: return fmtFloats(f, 3);
    case Kind::Vec4: return fmtFloats(f, 4);
    case Kind::Iso4: return "pos " + fmtFloats(f + 9, 3);
    case Kind::Iso3: return fmtFloats(f, 6);
    case Kind::Int3: {
        int32_t v[3];
        memcpy(v, raw, sizeof(v));
        return fmtInt(v[0]) + ", " + fmtInt(v[1]) + ", " + fmtInt(v[2]);
    }
    default: return "?";
    }
}

std::string fmtValueLong(Kind k, const void* raw, size_t n) {
    if (!raw || n < elemSize(k)) return "";
    if (k == Kind::Iso4) {
        float f[12];
        memcpy(f, raw, sizeof(f));
        return "x " + fmtFloats(f, 3) + "\ny " + fmtFloats(f + 3, 3) + "\nz " + fmtFloats(f + 6, 3) + "\npos " + fmtFloats(f + 9, 3);
    }
    if (k == Kind::Enum || k == Kind::Natural || k == Kind::Int) {
        uint32_t u;
        memcpy(&u, raw, 4);
        return fmtHex(u);
    }
    return "";
}

} // namespace nx
