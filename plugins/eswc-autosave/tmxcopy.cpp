// TMX copy, see tmxcopy.h. Port of strip_ghosts.py (verified against its output byte for byte).
//
// Replay file (GBX v6, BUCR, class 0x2407E000):
//   "GBX" u16 6 "BUCR" u32 class | u32 userDataSize, user data (header chunks, XML) |
//   u32 numNodes | u32 numExternalRefs | u32 bodySize | u32 compressedSize | LZO1X body
// Body (main node CGameCtnReplayRecord, class 0x2403F):
//   ... chunk 0x2403F004 = u32 4, u32 10, u32 ghostCount, ghost nodes ...
//       each ghost node: u32 nodeIndex (1..n), u32 0x2401B000, chunks ..., FACADE
//   then u32, u32 0, chunk 0x2403F007 ..., 0x2403F00C (u32 MediaTracker clip node, -1 = none),
//   0x2403F00D (u32 EventsDuration + the input events), 0x2403F00E (race-state node),
//   0x2403F00F (Validate_Version, ExeChecksum, OsKind, CpuKind, RaceSettings), ...
//
// The game sets the header's validable to (Ghosts.count == 1 && EventsDuration != 0) when it
// writes a replay (fn 0x68AF50). The conversion keeps one ghost and changes nothing else than:
// ghost count, the removed ghost nodes, node renumbering, numNodes and the validable digit.
#include "tmxcopy.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

namespace autosave {
namespace tmxcopy {
namespace {

constexpr uint32_t kReplayClass = 0x2407E000;
constexpr uint32_t kGhostClass = 0x2401B000;
constexpr uint32_t kRaceStateClass = 0x2407F000;
constexpr uint32_t kChunkGhostList = 0x2403F004;
constexpr uint32_t kChunkAfterList = 0x2403F007;
constexpr uint32_t kChunkClip = 0x2403F00C;
constexpr uint32_t kChunkEvents = 0x2403F00D;
constexpr uint32_t kChunkRaceState = 0x2403F00E;
constexpr uint32_t kChunkValidate = 0x2403F00F;
constexpr uint32_t kGhostRaceTime = 0x2401B005;
constexpr uint32_t kGhostLoginFrom = 0x2401B006;
constexpr uint32_t kGhostLoginTo = 0x2401B008;
constexpr uint32_t kGhostLastChunk = 0x2401B00C;
constexpr uint32_t kNull = 0xFFFFFFFFu;
constexpr uint32_t kSkip = 0x534B4950; // "PIKS"
const uint8_t kFacade[4] = {0x01, 0xde, 0xca, 0xfa};
const uint8_t kMagic[9] = {'G', 'B', 'X', 6, 0, 'B', 'U', 'C', 'R'};
constexpr size_t kHeaderStart = 17;
constexpr size_t kMaxFile = 64u << 20;
constexpr size_t kMaxBody = 64u << 20;
constexpr uint32_t kMaxGhosts = 128;
constexpr size_t kNotFound = static_cast<size_t>(-1);
constexpr int kNameAttempts = 20;

struct Bytes {
    uint8_t* p = nullptr;
    size_t n = 0;
    Bytes() = default;
    Bytes(const Bytes&) = delete;
    Bytes& operator=(const Bytes&) = delete;
    ~Bytes() { free(p); }
    bool alloc(size_t size) {
        free(p);
        p = static_cast<uint8_t*>(malloc(size ? size : 1));
        n = p ? size : 0;
        return p != nullptr;
    }
};

uint32_t rd32(const uint8_t* b, size_t o) {
    uint32_t v;
    memcpy(&v, b + o, 4);
    return v;
}
void wr32(uint8_t* b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); }

// First occurrence of needle fully inside [from, to), or kNotFound.
size_t find(const uint8_t* h, size_t from, size_t to, const void* needle, size_t len) {
    if (len == 0 || to < len || from > to - len) return kNotFound;
    const uint8_t first = *static_cast<const uint8_t*>(needle);
    for (size_t i = from; i <= to - len; i++)
        if (h[i] == first && memcmp(h + i, needle, len) == 0) return i;
    return kNotFound;
}
size_t find32(const uint8_t* h, size_t from, size_t to, uint32_t v) {
    uint8_t n[4];
    memcpy(n, &v, 4);
    return find(h, from, to, n, 4);
}
size_t count32(const uint8_t* h, size_t from, size_t to, uint32_t v) {
    size_t c = 0;
    for (size_t i = find32(h, from, to, v); i != kNotFound; i = find32(h, i + 1, to, v)) c++;
    return c;
}

void setReason(Report* r, Status s, const char* fmt, ...) {
    r->status = s;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(r->reason, sizeof(r->reason), _TRUNCATE, fmt, ap);
    va_end(ap);
}
#define REFUSE(...)                                  \
    do {                                             \
        setReason(r, Status::Refused, __VA_ARGS__); \
        return false;                                \
    } while (0)

// ---------------------------------------------------------------- LZO1X ---------------------
// Plain LZO1X decompressor (same algorithm as strip_ghosts.py), every access bounds-checked.
bool lzoDecompress(const uint8_t* src, size_t srcLen, uint8_t* out, size_t outLen, const char** err) {
    size_t ip = 0, op = 0;
    *err = "lzo: truncated stream";
    auto literals = [&](size_t k) {
        if (k > srcLen - ip || k > outLen - op) return false;
        memcpy(out + op, src + ip, k);
        ip += k;
        op += k;
        return true;
    };
    auto match = [&](size_t dist, size_t k) {
        if (dist == 0 || dist > op || k > outLen - op) return false;
        for (size_t i = 0; i < k; i++, op++) out[op] = out[op - dist];
        return true;
    };
    auto extLen = [&](size_t base, size_t* t) {
        size_t v = 0;
        while (ip < srcLen && src[ip] == 0) {
            v += 255;
            ip++;
            if (v > outLen + 255) return false;
        }
        if (ip >= srcLen) return false;
        v += base + src[ip++];
        *t = v;
        return true;
    };
    if (srcLen < 1) return false;
    size_t t = src[ip];
    size_t state = 0; // literals copied by the last instruction (0, 1-3, or 4 = 4+)
    if (t > 17) {
        ip++;
        t -= 17;
        if (!literals(t)) return false;
        state = t >= 4 ? 4 : t;
    }
    for (;;) {
        if (ip >= srcLen) return false;
        t = src[ip++];
        size_t s = 0;
        if (t < 16) {
            if (state == 0) {
                if (t == 0 && !extLen(15, &t)) return false;
                if (!literals(t + 3)) return false;
                state = 4;
                continue;
            }
            if (ip >= srcLen) return false;
            const size_t hi = static_cast<size_t>(src[ip++]) << 2;
            const bool ok = state == 4 ? match(1 + 0x0800 + (t >> 2) + hi, 3) : match(1 + (t >> 2) + hi, 2);
            if (!ok) { *err = "lzo: bad match"; return false; }
            s = t & 3;
        } else if (t >= 64) {
            if (ip >= srcLen) return false;
            const size_t dist = 1 + ((t >> 2) & 7) + (static_cast<size_t>(src[ip++]) << 3);
            if (!match(dist, (t >> 5) - 1 + 2)) { *err = "lzo: bad match"; return false; }
            s = t & 3;
        } else if (t >= 32) {
            size_t len = t & 31;
            if (len == 0 && !extLen(31, &len)) return false;
            if (srcLen - ip < 2) return false;
            const size_t d = src[ip] | (static_cast<size_t>(src[ip + 1]) << 8);
            ip += 2;
            if (!match(1 + (d >> 2), len + 2)) { *err = "lzo: bad match"; return false; }
            s = d & 3;
        } else {
            size_t len = t & 7;
            if (len == 0 && !extLen(7, &len)) return false;
            if (srcLen - ip < 2) return false;
            const size_t d = src[ip] | (static_cast<size_t>(src[ip + 1]) << 8);
            ip += 2;
            const size_t dist = ((t & 8) << 11) + (d >> 2);
            if (dist == 0) break; // end of stream
            if (!match(dist + 0x4000, len + 2)) { *err = "lzo: bad match"; return false; }
            s = d & 3;
        }
        if (s && !literals(s)) return false;
        state = s;
    }
    if (op != outLen) { *err = "lzo: wrong decompressed size"; return false; }
    return true;
}

// Valid LZO1X stream made of one literal run plus the end marker 0x11 0x00 0x00.
// The game loads such streams (tested with the reference converter's files).
bool lzoLiteralOnly(const uint8_t* data, size_t n, Bytes* out) {
    if (n < 4 || n > kMaxBody) return false;
    // head: 17+n (n <= 238), L = n-3 (L <= 15), else 0, (L-15)/255 zeros..., remainder
    size_t h = 1;
    if (n > 238 && n - 3 > 15) h = 2 + (n - 3 - 15 - 1) / 255;
    if (!out->alloc(h + n + 3)) return false;
    uint8_t* head = out->p;
    if (n <= 238) {
        head[0] = static_cast<uint8_t>(17 + n);
    } else if (n - 3 <= 15) {
        head[0] = static_cast<uint8_t>(n - 3);
    } else {
        size_t rem = n - 3 - 15;
        size_t k = 0;
        head[k++] = 0;
        while (rem > 255) {
            head[k++] = 0;
            rem -= 255;
        }
        head[k++] = static_cast<uint8_t>(rem);
        if (k != h) return false;
    }
    memcpy(out->p + h, data, n);
    out->p[h + n] = 0x11;
    out->p[h + n + 1] = 0;
    out->p[h + n + 2] = 0;
    return true;
}

// ---------------------------------------------------------------- parsing -------------------
struct Ghost {
    size_t start, end; // [start, end) in the body: node index .. FACADE
    bool hasTime;
    int32_t time;
    size_t loginFrom, loginTo; // chunk 0x2401B006 .. 0x2401B008, kNotFound if absent
};

struct Parsed {
    const uint8_t* file = nullptr;
    size_t fileLen = 0;
    uint32_t headerLen = 0; // user data size
    uint32_t numNodes = 0, numExt = 0, bodyLen = 0, compLen = 0;
    size_t compOff = 0;
    Bytes body;
    size_t cntOff = 0; // ghost count in the body
    uint32_t count = 0;
    Ghost ghosts[kMaxGhosts] = {};
    size_t listEnd = 0; // first byte after the last ghost node
    size_t c00c = kNotFound, c00d = kNotFound, c00e = kNotFound, c00f = kNotFound;
    uint32_t eventsDuration = 0;
    size_t validableOff = kNotFound; // file offset of the digit in validable="X"
};

bool parse(const uint8_t* f, size_t n, Parsed* p, Report* r) {
    p->file = f;
    p->fileLen = n;
    if (n < kHeaderStart + 16 || memcmp(f, kMagic, 9) != 0 || rd32(f, 9) != kReplayClass)
        REFUSE("not a TMN ESWC replay (GBX v6 BUCR, class 0x2407E000)");
    p->headerLen = rd32(f, 13);
    if (p->headerLen > n - kHeaderStart - 16) REFUSE("file truncated (header)");
    const size_t q = kHeaderStart + p->headerLen;
    p->numNodes = rd32(f, q);
    p->numExt = rd32(f, q + 4);
    p->bodyLen = rd32(f, q + 8);
    p->compLen = rd32(f, q + 12);
    p->compOff = q + 16;
    if (p->numExt != 0) REFUSE("external refs not supported");
    if (p->compLen > n - p->compOff) REFUSE("file truncated (body)");
    if (p->bodyLen < 16 || p->bodyLen > kMaxBody) REFUSE("unexpected body size %u", p->bodyLen);
    if (!p->body.alloc(p->bodyLen)) REFUSE("out of memory");
    const char* err = "";
    if (!lzoDecompress(f + p->compOff, p->compLen, p->body.p, p->bodyLen, &err)) REFUSE("body: %s", err);
    const uint8_t* b = p->body.p;
    const size_t bn = p->body.n;

    // header XML: validable="<digit>"
    static const char kVal[] = "validable=\"";
    for (size_t i = find(f, kHeaderStart, q, kVal, 11); i != kNotFound; i = find(f, i + 1, q, kVal, 11)) {
        if (i + 12 < q && f[i + 11] >= '0' && f[i + 11] <= '9' && f[i + 12] == '"') {
            p->validableOff = i + 11;
            break;
        }
    }

    // ghost list chunk 0x2403F004: u32 4, u32 10, u32 count, ghost nodes
    const uint32_t listHead[3] = {kChunkGhostList, 4, 10};
    const size_t m = find(b, 0, bn, listHead, 12);
    if (m == kNotFound || find(b, m + 1, bn, listHead, 12) != kNotFound) REFUSE("ghost list chunk not found exactly once");
    p->cntOff = m + 12;
    if (p->cntOff + 4 > bn) REFUSE("ghost list truncated");
    p->count = rd32(b, p->cntOff);
    if (p->count == 0) REFUSE("replay has no ghosts");
    if (p->count > kMaxGhosts) REFUSE("too many ghosts (%u)", p->count);
    size_t pos = p->cntOff + 4;
    for (uint32_t k = 0; k < p->count; k++) {
        if (pos + 8 > bn || rd32(b, pos) != k + 1 || rd32(b, pos + 4) != kGhostClass)
            REFUSE("unexpected node ref for ghost %u", k);
        p->ghosts[k].start = pos;
        if (k + 1 < p->count) {
            const uint32_t next[2] = {k + 2, kGhostClass};
            const size_t nxt = find(b, pos + 8, bn, next, 8);
            if (nxt == kNotFound || memcmp(b + nxt - 4, kFacade, 4) != 0) REFUSE("cannot find end of ghost %u", k);
            pos = nxt;
        }
    }
    // end of the last ghost: chunk 0x2401B00C (u32) + FACADE, then u32, u32 0, chunk 0x2403F007
    size_t last = find32(b, p->ghosts[p->count - 1].start, bn, kGhostLastChunk);
    while (last != kNotFound && !(last + 24 <= bn && memcmp(b + last + 8, kFacade, 4) == 0 &&
                                  rd32(b, last + 20) == kChunkAfterList))
        last = find32(b, last + 1, bn, kGhostLastChunk);
    if (last == kNotFound) REFUSE("cannot find end of ghost list");
    p->listEnd = last + 12;
    for (uint32_t k = 0; k < p->count; k++) {
        Ghost& g = p->ghosts[k];
        g.end = k + 1 < p->count ? p->ghosts[k + 1].start : p->listEnd;
        // race time: skippable chunk 0x2401B005 'PIKS' size=4 value (first occurrence in the ghost)
        const size_t c5 = find32(b, g.start, g.end, kGhostRaceTime);
        g.hasTime = c5 != kNotFound && c5 + 16 <= g.end && rd32(b, c5 + 4) == kSkip && rd32(b, c5 + 8) == 4;
        g.time = g.hasTime ? static_cast<int32_t>(rd32(b, c5 + 12)) : -1;
        g.loginFrom = find32(b, g.start, g.end, kGhostLoginFrom);
        g.loginTo = find32(b, g.start, g.end, kGhostLoginTo);
    }

    // rest of the main node: first occurrences after the ghost list (as the reference does)
    p->c00c = find32(b, p->listEnd, bn, kChunkClip);
    p->c00d = find32(b, p->listEnd, bn, kChunkEvents);
    p->c00e = find32(b, p->listEnd, bn, kChunkRaceState);
    if (p->c00d != kNotFound && p->c00d + 8 <= bn) p->eventsDuration = rd32(b, p->c00d + 4);
    if (p->c00e != kNotFound) p->c00f = find32(b, p->c00e, bn, kChunkValidate);
    return true;
}

bool ghostHasLogin(const Parsed& p, const Ghost& g, const char* login) {
    if (!login || !*login) return true;
    if (g.loginFrom == kNotFound || g.loginFrom == 0 || g.loginTo == kNotFound || g.loginTo <= g.loginFrom) return false;
    const size_t len = strlen(login);
    if (len > 200) return false;
    uint8_t want[204];
    const uint32_t l32 = static_cast<uint32_t>(len);
    memcpy(want, &l32, 4);
    memcpy(want + 4, login, len);
    return find(p.body.p, g.loginFrom, g.loginTo, want, len + 4) != kNotFound;
}

// The game's rule (fn 0x68AF50), applied to a body: ghost count == 1 && EventsDuration != 0.
int gameValidable(const uint8_t* b, size_t bn, size_t cntOff) {
    if (cntOff + 4 > bn) return 0;
    const size_t ev = find32(b, cntOff, bn, kChunkEvents);
    if (ev == kNotFound || ev + 8 > bn) return 0;
    return (rd32(b, cntOff) == 1 && rd32(b, ev + 4) != 0) ? 1 : 0;
}

struct Plan {
    uint32_t keep = 0;
    uint32_t newNodes = 0;
    bool renumber = false; // race-state node index -> 2
};

// Builds the output file in memory. Mirrors strip_ghosts.py, refusals in the same order.
bool convert(const Parsed& p, const char* login, int expectedTime, Bytes* out, Plan* plan, Report* r) {
    r->ghostsBefore = p.count;
    r->nodesBefore = p.numNodes;
    r->eventsDuration = p.eventsDuration;
    if (p.count == 1) {
        setReason(r, Status::NotNeeded, "replay has 1 ghost, nothing to do");
        return false;
    }
    const uint8_t* b = p.body.p;
    const size_t bn = p.body.n;
    if (p.c00d == kNotFound || p.c00e == kNotFound || p.c00e < p.c00d || p.c00d + 8 > bn)
        REFUSE("chunk 0x2403F00D/0x2403F00E not found");
    const uint32_t ed = p.eventsDuration;
    if (ed == 0) REFUSE("replay contains no input events (EventsDuration = 0) - nothing to validate");

    // Which ghost: the one whose finish time equals EventsDuration (the inputs belong to that run),
    // plus login / local finish time when the caller knows them. Exactly one must match.
    char times[120] = "";
    uint32_t nTime = 0, nAll = 0, keep = 0;
    for (uint32_t k = 0; k < p.count; k++) {
        const Ghost& g = p.ghosts[k];
        char one[16];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%d", k ? "," : "", g.hasTime ? g.time : -1);
        strncat_s(times, sizeof(times), one, _TRUNCATE);
        if (!g.hasTime || g.time < 0 || static_cast<uint32_t>(g.time) != ed) continue;
        nTime++;
        if (!ghostHasLogin(p, g, login)) continue;
        if (expectedTime >= 0 && g.time != expectedTime) continue;
        nAll++;
        keep = k;
    }
    if (nTime == 0)
        REFUSE("no ghost has time == EventsDuration %u (ghost times %s): the inputs belong to none of the saved ghost runs", ed,
               times);
    if (nAll == 0) {
        if (login && *login) REFUSE("the ghost with time == EventsDuration %u is not '%s'", ed, login);
        REFUSE("ghost time %u != local finish time %d", ed, expectedTime);
    }
    if (nAll > 1) REFUSE("%u ghosts have time == EventsDuration %u, cannot tell which one is yours", nAll, ed);
    const Ghost& g = p.ghosts[keep];
    r->keptIndex = static_cast<int>(keep);
    r->time = g.time;

    // 0x2403F00C must be a null node (no MediaTracker clip)
    if (p.c00c == kNotFound || p.c00c + 8 > bn || rd32(b, p.c00c + 4) != kNull)
        REFUSE("chunk 0x2403F00C holds a node (edited replay?) - not supported");
    // stricter than the reference: each of 0x2403F00C/D/E exactly once after the list, in order
    if (count32(b, p.listEnd, bn, kChunkClip) != 1 || count32(b, p.listEnd, bn, kChunkEvents) != 1 ||
        count32(b, p.listEnd, bn, kChunkRaceState) != 1 || !(p.c00c < p.c00d))
        REFUSE("unexpected chunk layout after the ghost list");

    // renumber nodes: kept ghost -> 1, race-state node (0x2403F00E) -> 2
    if (p.c00e + 8 > bn) REFUSE("chunk 0x2403F00E truncated");
    const uint32_t ref = rd32(b, p.c00e + 4);
    if (ref == kNull) {
        if (p.numNodes != p.count + 1)
            REFUSE("unexpected node numbering (numNodes %u, ghosts %u)", p.numNodes, p.count);
        plan->newNodes = 2;
        plan->renumber = false;
    } else {
        if (p.c00e + 12 > bn || rd32(b, p.c00e + 8) != kRaceStateClass)
            REFUSE("0x2403F00E node is a back-reference, not supported");
        // it must be the last node of the file: then it has no nested nodes and nothing after it
        // refers to the removed ghosts
        if (ref + 1 != p.numNodes || ref != p.count + 1)
            REFUSE("unexpected node numbering (ref %u, numNodes %u, ghosts %u)", ref, p.numNodes, p.count);
        plan->newNodes = 3;
        plan->renumber = true;
    }
    plan->keep = keep;

    // new body = body[:cnt] + u32 1 + kept ghost + rest after the list
    const size_t ghostLen = g.end - g.start;
    const size_t tailLen = bn - p.listEnd;
    Bytes nb;
    if (!nb.alloc(p.cntOff + 4 + ghostLen + tailLen)) REFUSE("out of memory");
    memcpy(nb.p, b, p.cntOff);
    wr32(nb.p, p.cntOff, 1);
    memcpy(nb.p + p.cntOff + 4, b + g.start, ghostLen);
    wr32(nb.p, p.cntOff + 4, 1);
    const size_t tailOff = p.cntOff + 4 + ghostLen;
    memcpy(nb.p + tailOff, b + p.listEnd, tailLen);
    if (plan->renumber) wr32(nb.p, tailOff + (p.c00e - p.listEnd) + 4, 2);

    // header: validable recomputed with the game's rule from the NEW body, never hard-coded
    const int validable = gameValidable(nb.p, nb.n, p.cntOff);
    if (p.validableOff == kNotFound) REFUSE("header XML not found");

    Bytes comp;
    if (!lzoLiteralOnly(nb.p, nb.n, &comp)) REFUSE("body too small");
    const size_t q = kHeaderStart + p.headerLen;
    if (!out->alloc(q + 16 + comp.n)) REFUSE("out of memory");
    memcpy(out->p, p.file, q);
    out->p[p.validableOff] = static_cast<uint8_t>('0' + validable);
    wr32(out->p, q, plan->newNodes);
    wr32(out->p, q + 4, 0);
    wr32(out->p, q + 8, static_cast<uint32_t>(nb.n));
    wr32(out->p, q + 12, static_cast<uint32_t>(comp.n));
    memcpy(out->p + q + 16, comp.p, comp.n);
    r->nodesAfter = plan->newNodes;
    r->validable = validable;
    return true;
}

// Re-parses the written file with the same decompressor/parser and checks it against the input:
// one ghost, inputs == ghost time, validable by the game's rule, and every byte of the kept ghost,
// the inputs, times, mode string and checksums unchanged.
bool verify(const Parsed& in, const Plan& plan, const uint8_t* f, size_t n, Report* r) {
    Report tmp;
    Parsed out;
    Parsed* o = &out;
    bool ok = false;
    const char* why = "";
    do {
        if (!parse(f, n, o, &tmp)) { why = tmp.reason; break; }
        if (o->compOff + o->compLen != n) { why = "trailing bytes"; break; }
        if (o->count != 1) { why = "ghost count is not 1"; break; }
        if (o->numNodes != plan.newNodes) { why = "numNodes"; break; }
        const uint8_t* ob = o->body.p;
        const uint8_t* ib = in.body.p;
        const Ghost& ig = in.ghosts[plan.keep];
        const size_t ghostLen = ig.end - ig.start;
        const size_t tailLen = in.body.n - in.listEnd;
        if (ghostLen < 8) { why = "ghost too small"; break; }
        if (o->cntOff != in.cntOff || o->body.n != in.cntOff + 4 + ghostLen + tailLen) { why = "body layout"; break; }
        if (memcmp(ob, ib, in.cntOff) != 0) { why = "bytes before the ghost list changed"; break; }
        if (o->ghosts[0].start != in.cntOff + 4 || o->ghosts[0].end != in.cntOff + 4 + ghostLen ||
            memcmp(ob + in.cntOff + 8, ib + ig.start + 4, ghostLen - 4) != 0) {
            why = "kept ghost changed";
            break;
        }
        const size_t tailOff = in.cntOff + 4 + ghostLen;
        const size_t refAt = (in.c00e - in.listEnd) + 4; // race-state node index, relative to the tail
        if (plan.renumber) {
            if (memcmp(ob + tailOff, ib + in.listEnd, refAt) != 0 || rd32(ob, tailOff + refAt) != 2 ||
                memcmp(ob + tailOff + refAt + 4, ib + in.listEnd + refAt + 4, tailLen - refAt - 4) != 0) {
                why = "inputs/race state/checksums changed";
                break;
            }
        } else if (memcmp(ob + tailOff, ib + in.listEnd, tailLen) != 0) {
            why = "inputs/race state/checksums changed";
            break;
        }
        if (o->eventsDuration == 0 || o->eventsDuration != in.eventsDuration || !o->ghosts[0].hasTime ||
            o->ghosts[0].time < 0 || static_cast<uint32_t>(o->ghosts[0].time) != o->eventsDuration) {
            why = "inputs do not match the ghost time";
            break;
        }
        const size_t q = kHeaderStart + in.headerLen;
        if (o->headerLen != in.headerLen || o->validableOff != in.validableOff ||
            memcmp(f, in.file, in.validableOff) != 0 ||
            memcmp(f + in.validableOff + 1, in.file + in.validableOff + 1, q - in.validableOff - 1) != 0) {
            why = "header changed beyond validable";
            break;
        }
        const int rule = gameValidable(ob, o->body.n, o->cntOff);
        if (f[o->validableOff] != '0' + rule) { why = "header validable does not follow the game's rule"; break; }
        r->validable = rule;
        ok = true;
    } while (false);
    if (!ok) setReason(r, Status::Error, "verification failed: %s - output deleted", why);
    return ok;
}

// ---------------------------------------------------------------- files ---------------------
bool readFile(const wchar_t* path, Bytes* out, DWORD* err) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && static_cast<uint64_t>(size.QuadPart) <= kMaxFile &&
              out->alloc(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    ok = ok && ReadFile(h, out->p, static_cast<DWORD>(out->n), &got, nullptr) && got == out->n;
    *err = ok ? 0 : (GetLastError() ? GetLastError() : ERROR_INVALID_DATA);
    CloseHandle(h);
    return ok;
}

bool writeNewFile(const wchar_t* path, const uint8_t* data, size_t n, DWORD* err) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    DWORD put = 0;
    bool ok = WriteFile(h, data, static_cast<DWORD>(n), &put, nullptr) && put == n;
    ok = FlushFileBuffers(h) && ok;
    *err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) DeleteFileW(path);
    return ok;
}

bool fmtw(wchar_t* out, size_t cap, const wchar_t* f, ...) {
    va_list ap;
    va_start(ap, f);
    const int n = _vsnwprintf_s(out, cap, _TRUNCATE, f, ap);
    va_end(ap);
    return n >= 0;
}

bool endsWithI(const wchar_t* s, const wchar_t* suffix) {
    const size_t a = wcslen(s), b = wcslen(suffix);
    return a >= b && _wcsicmp(s + a - b, suffix) == 0;
}

double msSince(const LARGE_INTEGER& t0) {
    LARGE_INTEGER t1, fq;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    return static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(fq.QuadPart);
}

void convertFile(const wchar_t* srcPath, const wchar_t* outDir, const char* login, int expectedTime, Report* r) {
    Bytes in;
    DWORD err = 0;
    if (!readFile(srcPath, &in, &err)) {
        setReason(r, Status::Error, "cannot read the saved replay (error %lu)", err);
        return;
    }
    Parsed parsed;
    Parsed* p = &parsed;
    Bytes out;
    Plan plan;
    do {
        if (!parse(in.p, in.n, p, r)) break;
        if (!convert(*p, login, expectedTime, &out, &plan, r)) break;

        // names: <dir>\<stem> TMX.Replay.Gbx, never overwriting
        const wchar_t* slash = wcsrchr(srcPath, L'\\');
        const wchar_t* slash2 = wcsrchr(srcPath, L'/');
        if (slash2 > slash) slash = slash2;
        const wchar_t* fileName = slash ? slash + 1 : srcPath;
        wchar_t dir[1024];
        if (outDir) {
            if (!fmtw(dir, _countof(dir), L"%s%s", outDir,
                      endsWithI(outDir, L"\\") || endsWithI(outDir, L"/") ? L"" : L"\\")) {
                setReason(r, Status::Error, "path too long");
                break;
            }
        } else {
            const size_t dl = static_cast<size_t>(fileName - srcPath);
            if (dl >= _countof(dir)) { setReason(r, Status::Error, "path too long"); break; }
            wmemcpy(dir, srcPath, dl);
            dir[dl] = 0;
        }
        static const wchar_t kExt[] = L".Replay.Gbx";
        if (!endsWithI(fileName, kExt)) { setReason(r, Status::Refused, "file name does not end in .Replay.Gbx"); break; }
        wchar_t stem[600];
        const size_t sl = wcslen(fileName) - (_countof(kExt) - 1);
        if (sl + 8 >= _countof(stem)) { setReason(r, Status::Error, "path too long"); break; }
        wmemcpy(stem, fileName, sl);
        wcscpy_s(stem + sl, _countof(stem) - sl, L" TMX");

        wchar_t temp[1024];
        if (!fmtw(temp, _countof(temp), L"%s~tmxcopy_%lu_%lu_%lu.tmp", dir, GetCurrentProcessId(),
                  GetCurrentThreadId(), GetTickCount())) {
            setReason(r, Status::Error, "path too long");
            break;
        }
        if (!writeNewFile(temp, out.p, out.n, &err)) {
            setReason(r, Status::Error, "cannot write %s (error %lu)", "the temp file", err);
            break;
        }
        // re-read what is on disk and verify it before it gets its real name
        Bytes back;
        if (!readFile(temp, &back, &err)) {
            DeleteFileW(temp);
            setReason(r, Status::Error, "cannot re-read the written file (error %lu) - output deleted", err);
            break;
        }
        if (!verify(*p, plan, back.p, back.n, r)) {
            DeleteFileW(temp);
            break;
        }
        bool moved = false;
        err = 0;
        for (int attempt = 1; attempt <= kNameAttempts && !moved; attempt++) {
            wchar_t name[700], full[1024];
            if (!replayFileName(name, _countof(name), stem, attempt) || !fmtw(full, _countof(full), L"%s%s", dir, name)) {
                err = ERROR_FILENAME_EXCED_RANGE;
                break;
            }
            if (MoveFileExW(temp, full, MOVEFILE_WRITE_THROUGH)) { // no REPLACE_EXISTING: never overwrites
                moved = true;
                wmemcpy(r->outPath, full, _countof(full)); // same size
            } else {
                err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS) break;
            }
        }
        if (!moved) {
            DeleteFileW(temp);
            setReason(r, Status::Error, "cannot name the copy (error %lu, %d names tried)", err, kNameAttempts);
            break;
        }
        r->status = Status::Converted;
        r->reason[0] = 0;
    } while (false);
}

} // namespace

bool replayFileName(wchar_t* out, size_t cap, const wchar_t* stem, int attempt) {
    const int n = attempt <= 1 ? _snwprintf_s(out, cap, _TRUNCATE, L"%s.Replay.Gbx", stem)
                               : _snwprintf_s(out, cap, _TRUNCATE, L"%s (%d).Replay.Gbx", stem, attempt);
    return n >= 0;
}

void makeCopy(const wchar_t* srcPath, const wchar_t* outDir, const char* login, int expectedTime, Report* r) {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    r->status = Status::Error;
    r->reason[0] = 0;
    r->outPath[0] = 0;
    convertFile(srcPath, outDir, login, expectedTime, r);
    r->ms = msSince(t0);
}

bool describe(const wchar_t* path, char* out, size_t cap) {
    Bytes in;
    DWORD err = 0;
    if (!readFile(path, &in, &err)) {
        _snprintf_s(out, cap, _TRUNCATE, "cannot read (error %lu)", err);
        return false;
    }
    Parsed parsed;
    Parsed* p = &parsed;
    Report r;
    bool ok = parse(in.p, in.n, p, &r);
    if (!ok) {
        _snprintf_s(out, cap, _TRUNCATE, "not parsed: %s", r.reason);
    } else {
        char times[200] = "";
        for (uint32_t k = 0; k < p->count; k++) {
            char one[16];
            _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%d", k ? "," : "", p->ghosts[k].time);
            strncat_s(times, sizeof(times), one, _TRUNCATE);
        }
        // RaceSettings: 0x2403F00F 'PIKS' size, str Validate_Version, u32 ExeChecksum, u32 OsKind,
        // u32 CpuKind, str RaceSettings
        char mode[64] = "?";
        const uint8_t* b = p->body.p;
        const size_t bn = p->body.n;
        size_t c = p->c00f;
        if (c != kNotFound && c + 16 <= bn && rd32(b, c + 4) == kSkip) {
            const uint32_t l1 = rd32(b, c + 12);
            const size_t m = c + 16 + static_cast<size_t>(l1) + 12;
            if (l1 < 256 && m + 4 <= bn) {
                const uint32_t l2 = rd32(b, m);
                if (l2 < sizeof(mode) && m + 4 + l2 <= bn) {
                    memcpy(mode, b + m + 4, l2);
                    mode[l2] = 0;
                }
            }
        }
        const char hv = p->validableOff != kNotFound ? static_cast<char>(in.p[p->validableOff]) : '?';
        _snprintf_s(out, cap, _TRUNCATE,
                    "ghosts=%u times=[%s] EventsDuration=%u numNodes=%u header validable=%c game rule=%d mode=%s",
                    p->count, times, p->eventsDuration, p->numNodes, hv, gameValidable(b, bn, p->cntOff), mode);
    }
    return ok;
}

} // namespace tmxcopy
} // namespace autosave
