#include "fake_graph.h"

#include <windows.h>
#include <string.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

// Head of the fake class list. The function below is never called: it only has to exist in the
// exe's code so that reflect's registry scan finds `mov ecx, [head]; mov [eax+18h], ecx; mov [head], eax`.
extern "C" {
volatile uint32_t g_nxFakeHead = 0;
__declspec(noinline) void nxFakeRegisterPattern(void) {
    __asm {
        mov ecx, dword ptr [g_nxFakeHead]
        mov dword ptr [eax + 18h], ecx
        mov dword ptr [g_nxFakeHead], eax
    }
}
}

namespace fake {
namespace {

// Forever (TMNF) type numbers; ESWC/Sunrise = these - 4 from 31 on (27 then reads as uint).
enum T : uint32_t {
    Proc = 0, Bool = 1, Nod = 5, NodBuffer = 6, NodList = 7, Color = 9, Enum = 13, Int = 14, Iso4 = 19, Id = 27,
    Natural = 31, NaturalList = 32, Real = 36, RealList = 37, String = 41, StringList = 43, WString = 45,
    Vec3 = 53, Vec3Array = 56, Int3 = 61, ProcEx = 65
};

bool g_forever = true;
uint8_t* g_arena = nullptr;
size_t g_used = 0;
const size_t kArena = 1 << 20;
Graph g_graph;
void (*volatile g_keep)(void) = nxFakeRegisterPattern; // keeps the pattern linked

uintptr_t alloc(size_t n) {
    g_used = (g_used + 15) & ~static_cast<size_t>(15);
    const uintptr_t p = reinterpret_cast<uintptr_t>(g_arena + g_used);
    g_used += n;
    return p;
}
void put32(uintptr_t a, uint32_t v) { memcpy(reinterpret_cast<void*>(a), &v, 4); }
void putF(uintptr_t a, float v) { memcpy(reinterpret_cast<void*>(a), &v, 4); }
void putFs(uintptr_t a, std::initializer_list<float> v) {
    for (float f : v) {
        putF(a, f);
        a += 4;
    }
}
uintptr_t cstr(const char* s) {
    const uintptr_t p = alloc(strlen(s) + 1);
    memcpy(reinterpret_cast<void*>(p), s, strlen(s) + 1);
    return p;
}
// CFastString {length, chars}
void putStr(uintptr_t a, const char* s) {
    put32(a, static_cast<uint32_t>(strlen(s)));
    put32(a + 4, static_cast<uint32_t>(cstr(s)));
}
void putWStr(uintptr_t a, const wchar_t* s) {
    const size_t n = wcslen(s);
    const uintptr_t p = alloc((n + 1) * 2);
    memcpy(reinterpret_cast<void*>(p), s, (n + 1) * 2);
    put32(a, static_cast<uint32_t>(n));
    put32(a + 4, static_cast<uint32_t>(p));
}
// CFastBuffer {count, items, capacity}
uintptr_t putList(uintptr_t a, uint32_t count, size_t elem) {
    const uintptr_t items = count ? alloc(count * elem) : 0;
    put32(a, count);
    put32(a + 4, static_cast<uint32_t>(items));
    put32(a + 8, count);
    return items;
}

uint32_t ty(uint32_t forever) { return (!g_forever && forever >= 31) ? forever - 4 : forever; }

struct M {
    const char* name;
    uint32_t type; // Forever number
    int offset;    // -1: getter / method
    const char* cls;
};

std::map<std::string, uintptr_t> g_classes;
std::vector<std::pair<uintptr_t, std::string>> g_declared;
uintptr_t g_stubRet = 0;

uintptr_t defClass(const char* name, uint32_t id, const char* parent, std::initializer_list<M> members) {
    const uintptr_t ci = alloc(0x40);
    put32(ci + 0x04, id);
    put32(ci + 0x08, parent ? static_cast<uint32_t>(g_classes.at(parent)) : 0);
    put32(ci + 0x14, static_cast<uint32_t>(cstr(name)));
    put32(ci + 0x18, g_nxFakeHead); // linked like the games: newest first
    const uint32_t n = static_cast<uint32_t>(members.size());
    const uintptr_t arr = n ? alloc(n * 4) : 0;
    uint32_t i = 0;
    for (const M& m : members) {
        const uintptr_t p = alloc(0x28);
        const uint32_t t = ty(m.type);
        put32(p + 0x00, t);
        put32(p + 0x04, id | i);
        put32(p + 0x0c, m.offset < 0 ? 0xffffffffu : static_cast<uint32_t>(m.offset));
        put32(p + 0x10, static_cast<uint32_t>(cstr(m.name)));
        put32(p + 0x20, 0x3f80);
        if ((m.type == Proc || m.type == ProcEx) && !g_forever) put32(p + 0x24, static_cast<uint32_t>(g_stubRet)); // method pointer (never called)
        if (m.cls) g_declared.push_back({p + (g_forever ? 0x1c : 0x24), m.cls});
        put32(arr + i * 4, static_cast<uint32_t>(p));
        i++;
    }
    put32(ci + 0x20, static_cast<uint32_t>(arr));
    put32(ci + 0x24, n);
    g_nxFakeHead = static_cast<uint32_t>(ci);
    g_classes[name] = ci;
    g_graph.classCount++;
    return ci;
}

// `mov eax, imm32; ret`
uintptr_t stub(uint32_t imm) {
    const uintptr_t p = alloc(8);
    auto* b = reinterpret_cast<uint8_t*>(p);
    b[0] = 0xb8;
    memcpy(b + 1, &imm, 4);
    b[5] = 0xc3;
    return p;
}

uintptr_t object(const char* cls, size_t size) {
    const uintptr_t ci = g_classes.at(cls);
    uint32_t id = 0;
    memcpy(&id, reinterpret_cast<void*>(ci + 4), 4);
    const uintptr_t vt = alloc(16);
    put32(vt + 0, static_cast<uint32_t>(g_stubRet));      // some method
    put32(vt + 4, static_cast<uint32_t>(stub(id)));       // GetMwClassId: returns the id (must not be taken for a class)
    put32(vt + 8, static_cast<uint32_t>(stub(static_cast<uint32_t>(ci)))); // MwGetClassInfo
    put32(vt + 12, static_cast<uint32_t>(g_stubRet));
    const uintptr_t o = alloc(size);
    put32(o, static_cast<uint32_t>(vt));
    return o;
}

} // namespace

const Graph& build(bool forever) {
    if (!g_arena) g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    memset(g_arena, 0, kArena);
    g_used = 0;
    g_forever = forever;
    g_graph = Graph();
    g_classes.clear();
    g_declared.clear();
    g_nxFakeHead = 0;
    (void)g_keep;
    g_stubRet = alloc(4);
    reinterpret_cast<uint8_t*>(g_stubRet)[0] = 0xc3;

    // A guard page (like another thread's stack end) and a freed page: members point at both.
    if (!g_graph.guardPage) {
        static uintptr_t guard = 0;
        if (!guard) guard = reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD));
        g_graph.guardPage = guard;
    }
    static uintptr_t freed = 0;
    if (!freed) {
        freed = reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        VirtualFree(reinterpret_cast<void*>(freed), 0, MEM_RELEASE);
    }

    defClass("CMwNod", 0x01001000, nullptr, {});
    defClass("CGameApp", 0x03001000, "CMwNod", {{"Viewport", Nod, 0x08, nullptr}, {"ShowMenu", ProcEx, -1, nullptr}});
    defClass("CGameCtnBlock", 0x0301f000, "CMwNod", {{"Coord", Int3, 0x08, nullptr}, {"Dir", Enum, 0x14, nullptr}, {"BlockId", Id, 0x18, nullptr}});
    defClass("CGameCtnChallenge", 0x03043000, "CMwNod",
             {{"ChallengeName", String, 0x08, nullptr},
              {"AuthorLogin", String, 0x10, nullptr},
              {"TMObjective_AuthorTime", Natural, 0x18, nullptr},
              {"Size", Int3, 0x1c, nullptr},
              {"ThumbnailPosition", Vec3, 0x28, nullptr},
              {"Decoration", Nod, 0x34, nullptr},
              {"Blocks", NodList, 0x38, "CGameCtnBlock"},
              {"ComputeCrc32", ProcEx, -1, nullptr}});
    defClass("CSceneMobil", 0x0a005000, "CMwNod",
             {{"Position", Vec3, 0x08, nullptr},
              {"Velocity", Vec3, 0x14, nullptr},
              {"Speed", Real, 0x20, nullptr},
              {"WheelContacts", Vec3Array, 0x24, nullptr},
              {"Item", Nod, 0x2c, nullptr},
              {"Corpus", Nod, 0x30, nullptr},
              {"IsTurbo", Bool, -1, nullptr}});
    defClass("CGamePlayer", 0x03002000, "CMwNod",
             {{"Login", String, 0x08, nullptr}, {"Name", WString, 0x10, nullptr}, {"IsSpectator", Bool, 0x18, nullptr}});
    defClass("CTrackManiaPlayer", 0x24006000, "CGamePlayer",
             {{"Mobil", Nod, 0x20, "CSceneMobil"},
              {"CurRaceTime", Natural, 0x24, nullptr},
              {"Location", Iso4, 0x28, nullptr},
              {"Color", Color, 0x58, nullptr},
              {"TeamId", Int, 0x64, nullptr},
              {"CheckpointTimes", NaturalList, 0x68, nullptr},
              {"Tags", StringList, 0x74, nullptr},
              {"Respawn", Proc, -1, nullptr}});
    defClass("CGameTerminal", 0x0300b000, "CMwNod",
             {{"ControlledPlayer", Nod, 0x08, "CGamePlayer"}, {"GUIPlayer", Nod, 0x0c, "CGamePlayer"}, {"MediaClipPlayer", Nod, -1, nullptr}});
    defClass("CGamePlayground", 0x0302a000, "CMwNod",
             {{"GameTerminals", NodList, 0x08, "CGameTerminal"},
              {"Players", NodBuffer, 0x14, "CGamePlayer"},
              {"Interface", Nod, 0x20, nullptr},
              {"IsPaused", Bool, 0x24, nullptr},
              {"GameTime", Natural, 0x28, nullptr}});
    defClass("CGameCtnApp", 0x030d3000, "CGameApp",
             {{"Challenge", Nod, 0x10, "CGameCtnChallenge"},
              {"CurrentPlayground", Nod, 0x14, "CGamePlayground"},
              {"GameScene", Nod, -1, nullptr},
              {"PlayerProfiles", NodList, 0x18, nullptr}});
    defClass("CTrackMania", 0x24001000, "CGameCtnApp",
             {{"MenuManager", Nod, 0x30, nullptr},
              {"Switcher", Nod, 0x34, nullptr},
              {"PillarOffset", Real, 0x38, nullptr},
              {"PlayTest", Enum, 0x3c, nullptr},
              {"MatchSettings", NodList, 0x40, nullptr},
              {"Races", Nod, 0x4c, nullptr},
              {"ShowContextHelp", Proc, -1, nullptr}});
    defClass("CGameRace", 0x03035000, "CMwNod", {{"Players", NodBuffer, 0x08, "CGamePlayer"}, {"Challenge", Nod, 0x14, "CGameCtnChallenge"}});
    defClass("CTrackManiaRace", 0x24004000, "CGameRace",
             {{"StartTime", Natural, 0x18, nullptr}, {"IsPaused", Bool, 0x1c, nullptr}, {"SpeedHistory", RealList, 0x20, nullptr}});
    for (auto& d : g_declared) put32(d.first, static_cast<uint32_t>(g_classes.at(d.second)));

    // ---- objects
    const uintptr_t challenge = object("CGameCtnChallenge", 0x48);
    putStr(challenge + 0x08, "A01-Race");
    putStr(challenge + 0x10, "nadeo");
    put32(challenge + 0x18, 23000);
    put32(challenge + 0x1c, 32);
    put32(challenge + 0x20, 40);
    put32(challenge + 0x24, 32);
    putFs(challenge + 0x28, {512.0f, 96.5f, -32.25f});
    const uintptr_t blocks = putList(challenge + 0x38, 400, 4);
    for (uint32_t i = 0; i < 16; i++) {
        const uintptr_t b = object("CGameCtnBlock", 0x20);
        put32(b + 0x08, 10 + i);
        put32(b + 0x0c, 1);
        put32(b + 0x10, 20 + i);
        put32(b + 0x14, i % 4);
        put32(b + 0x18, 0x40000000u | i);
        put32(blocks + i * 4, static_cast<uint32_t>(b));
    }

    const uintptr_t mobil = object("CSceneMobil", 0x40);
    putFs(mobil + 0x08, {512.0f, 9.5f, 140.75f});
    putFs(mobil + 0x14, {0.0f, -0.5f, 61.25f});
    putF(mobil + 0x20, 220.5f);
    put32(mobil + 0x2c, static_cast<uint32_t>(g_graph.guardPage + 64)); // must not trip the guard page
    put32(mobil + 0x30, static_cast<uint32_t>(freed + 16));              // freed memory

    const uintptr_t player = object("CTrackManiaPlayer", 0x80);
    putStr(player + 0x08, "oskar_tm");
    putWStr(player + 0x10, L"$f80Oskär \U0001F3C1"); // umlaut and a flag outside the BMP
    put32(player + 0x18, 0);
    put32(player + 0x20, static_cast<uint32_t>(mobil));
    put32(player + 0x24, 31415);
    putFs(player + 0x28, {1, 0, 0, 0, 1, 0, 0, 0, 1, 512.0f, 9.5f, 140.75f});
    putFs(player + 0x58, {1.0f, 0.42f, 0.1f});
    put32(player + 0x64, static_cast<uint32_t>(-1));
    const uintptr_t cps = putList(player + 0x68, 5, 4);
    for (uint32_t i = 0; i < 5; i++) put32(cps + i * 4, 4500 * (i + 1));
    const uintptr_t tags = putList(player + 0x74, 2, 8);
    putStr(tags, "fast");
    putStr(tags + 8, "line\nbreak \"quoted\"");

    const uintptr_t player2 = object("CTrackManiaPlayer", 0x80);
    putStr(player2 + 0x08, "ghost");
    putWStr(player2 + 0x10, L"Ghost");
    put32(player2 + 0x18, 1);

    const uintptr_t term0 = object("CGameTerminal", 0x10);
    put32(term0 + 0x08, static_cast<uint32_t>(player));
    put32(term0 + 0x0c, static_cast<uint32_t>(player));
    const uintptr_t term1 = object("CGameTerminal", 0x10);
    put32(term1 + 0x08, static_cast<uint32_t>(player2));

    const uintptr_t pg = object("CGamePlayground", 0x30);
    const uintptr_t terms = putList(pg + 0x08, 2, 4);
    put32(terms, static_cast<uint32_t>(term0));
    put32(terms + 4, static_cast<uint32_t>(term1));
    const uintptr_t pls = putList(pg + 0x14, 2, 4);
    put32(pls, static_cast<uint32_t>(player));
    put32(pls + 4, static_cast<uint32_t>(player2));
    put32(pg + 0x24, 0);
    put32(pg + 0x28, 123456);

    const uintptr_t app = object("CTrackMania", 0x58);
    put32(app + 0x10, static_cast<uint32_t>(challenge));
    put32(app + 0x14, static_cast<uint32_t>(pg));
    put32(app + 0x30, 0x1234);                                  // not a pointer
    put32(app + 0x34, static_cast<uint32_t>(player + 0x58));    // readable, but not a nod (points at floats)
    putF(app + 0x38, 8.0f);
    put32(app + 0x3c, 2);
    putList(app + 0x40, 0, 4);
    put32(app + 0x4c, 0xdead0000u);                             // unmapped

    const uintptr_t race = object("CTrackManiaRace", 0x30);
    const uintptr_t rp = putList(race + 0x08, 2, 4);
    put32(rp, static_cast<uint32_t>(player));
    put32(rp + 4, static_cast<uint32_t>(player2));
    put32(race + 0x14, static_cast<uint32_t>(challenge));
    put32(race + 0x18, 1200);
    put32(race + 0x1c, 1);
    const uintptr_t sp = putList(race + 0x20, 12, 4);
    for (uint32_t i = 0; i < 12; i++) putF(sp + i * 4, 100.0f + 10.5f * static_cast<float>(i));

    g_graph.app = app;
    g_graph.race = race;
    g_graph.challenge = challenge;
    g_graph.player = player;
    return g_graph;
}

bool guardIntact() {
    MEMORY_BASIC_INFORMATION mbi;
    if (!g_graph.guardPage || VirtualQuery(reinterpret_cast<void*>(g_graph.guardPage), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    return (mbi.Protect & PAGE_GUARD) != 0;
}

} // namespace fake
