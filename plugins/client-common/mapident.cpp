#include "mapident.h"

#include <cstring>
#include <vector>

namespace ck {

namespace {
const unsigned char kPattern[] = {0x8B, 0x01, 0x8B, 0xC8, 0x81, 0xE1, 0x00, 0x00, 0x00, 0xC0, 0x81, 0xF9, 0x00, 0x00, 0x00, 0x40};

uint32_t le32(const unsigned char* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
} // namespace

bool decodeIdTable(const unsigned char* code, size_t n, IdTable* out) {
    if (n < 0x40 || memcmp(code, kPattern, sizeof(kPattern)) != 0) return false;
    for (size_t i = sizeof(kPattern); i + 6 <= 0x40 && i + 6 <= n; i++) {
        // TMNF/TMUF: push esi; mov esi, [g]  ...  lea ecx, [esi + eax*8 + 0Ch]
        if (code[i] == 0x8B && code[i + 1] == 0x35) {
            out->global = le32(code + i + 2);
            out->stride = 8;
            out->base = 0x10;
            return out->global > 0x10000;
        }
        // ESWC/Sunrise: lea edx, [ecx + ecx*2]; mov ecx, [g]; mov edx, [ecx + edx*4 + 14h]
        if (code[i] == 0x8D && code[i + 1] == 0x14 && code[i + 2] == 0x49 && i + 9 <= n && code[i + 3] == 0x8B && code[i + 4] == 0x0D) {
            out->global = le32(code + i + 5);
            out->stride = 12;
            out->base = 0x14;
            if (i + 13 <= n && code[i + 9] == 0x8B && code[i + 10] == 0x54 && code[i + 11] == 0x91) out->base = code[i + 12];
            return out->global > 0x10000;
        }
    }
    return false;
}

size_t imageSize(Mem& m, uintptr_t image) {
    uint32_t pe = 0, size = 0;
    uint16_t mz = 0;
    if (!m.get(image, &mz) || mz != 0x5A4D || !m.get(image + 0x3C, &pe) || pe > 0x1000) return 0;
    if (!m.get(image + pe + 0x50, &size) || size > (256u << 20)) return 0; // OptionalHeader.SizeOfImage
    return size;
}

bool findIdTable(Mem& m, uintptr_t image, size_t size, IdTable* out) {
    *out = IdTable();
    const size_t chunk = 1 << 16, overlap = 0x40;
    std::vector<unsigned char> buf(chunk + overlap);
    for (size_t at = 0x1000; at < size; at += chunk) {
        const size_t n = (size - at < chunk + overlap) ? size - at : chunk + overlap;
        if (!m.read(image + at, buf.data(), n)) continue;
        for (size_t i = 0; i + 0x40 <= n; i++) {
            if (buf[i] != 0x8B || buf[i + 1] != 0x01 || memcmp(&buf[i], kPattern, sizeof(kPattern)) != 0) continue;
            if (decodeIdTable(&buf[i], n - i, out)) return true;
        }
    }
    return false;
}

std::string idName(Mem& m, const IdTable& t, uint32_t id) {
    if (!t.ok() || id == 0xffffffffu) return std::string();
    const uint32_t kind = id & 0xC0000000u;
    if (kind != 0x40000000u && kind != 0x80000000u) return std::string();
    const uint32_t v = id & 0x3FFFFFFFu;
    const uint32_t hi = v >> 16, lo = v & 0xFFFFu;
    uintptr_t table = 0, pool = 0, data = 0;
    uint32_t entry = 0;
    if (!m.ptr(t.global, 0, &table) || !m.ptr(table, 4, &pool)) return std::string();
    if (!m.ptr(table, static_cast<int>(hi) * t.stride + t.base, &data)) return std::string();
    if (!m.get(data + lo * 4u, &entry)) return std::string();
    char s[129] = {};
    for (size_t i = 0; i < 128; i++) {
        if (!m.get(pool + entry + i, &s[i])) return std::string();
        if (!s[i]) break;
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 32 || c == 127) return std::string();
    }
    return s;
}

bool plausibleUid(const std::string& s) {
    if (s.size() < 20 || s.size() > 32) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

bool knownEnvironment(const std::string& s) {
    static const char* const kEnv[] = {"Stadium", "Island", "Bay", "Coast", "Desert", "Snow", "Rally", "Speed", "Alpine"};
    for (const char* e : kEnv)
        if (s == e) return true;
    return false;
}

std::string challengeUid(Mem& m, const IdTable& t, uintptr_t challenge, int* offset) {
    if (!t.ok() || !challenge) return std::string();
    auto at = [&](int off) -> std::string {
        uint32_t uid = 0, env = 0;
        if (!m.get(challenge + static_cast<uintptr_t>(off), &uid) || !m.get(challenge + static_cast<uintptr_t>(off) + 4, &env)) return std::string();
        const std::string u = idName(m, t, uid);
        if (!plausibleUid(u)) return std::string();
        // the environment follows: a name we know, or a predefined collection number
        const std::string e = idName(m, t, env);
        if (!(knownEnvironment(e) || (e.empty() && env < 0x1000))) return std::string();
        return u;
    };
    if (offset && *offset >= 0) {
        const std::string u = at(*offset);
        if (!u.empty()) return u;
    }
    for (int off = 0x20; off < 0x300; off += 4) {
        const std::string u = at(off);
        if (!u.empty()) {
            if (offset) *offset = off;
            return u;
        }
    }
    return std::string();
}

} // namespace ck
