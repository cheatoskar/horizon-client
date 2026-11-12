// Unit tests: WR Hunter's split logic and the client-common parts it uses (game reader against fake memory,
// map UID table decoding with the real instruction bytes of TMNF and ESWC, replay files).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "gameread.h"
#include "mapident.h"
#include "replayfile.h"
#include "src/splits.h"

namespace {
int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL  %s\n", what);
    }
}

// Byte-addressed fake memory with object classes.
struct FakeMem : ck::Mem {
    std::map<uintptr_t, uint8_t> bytes;
    std::map<uintptr_t, std::string> cls;
    bool read(uintptr_t a, void* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            auto it = bytes.find(a + i);
            if (it == bytes.end()) return false;
            static_cast<uint8_t*>(out)[i] = it->second;
        }
        return true;
    }
    bool isKind(uintptr_t o, const char* c) override {
        auto it = cls.find(o);
        return it != cls.end() && (it->second == c || (strcmp(c, "CTrackManiaRace") == 0 && it->second.find("CTrackManiaRace") == 0));
    }
    void put(uintptr_t a, const void* p, size_t n) {
        for (size_t i = 0; i < n; i++) bytes[a + i] = static_cast<const uint8_t*>(p)[i];
    }
    void u32(uintptr_t a, uint32_t v) { put(a, &v, 4); }
    void f32(uintptr_t a, float v) { put(a, &v, 4); }
    void str(uintptr_t a, const char* s) { put(a, s, strlen(s) + 1); }
};

std::string u32s(uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); }

void testSplits() {
    wrh::MapStats s;
    auto r1 = wrh::applyFinish(s, {5000, 10000, 15000}, 0, 100);
    check(r1.pb && r1.firstRun && s.pbTime() == 15000, "first finish is the PB");
    check(wrh::sumOfBest(s) == 15000, "sum of best = first run");
    auto r2 = wrh::applyFinish(s, {4800, 10200, 15500}, 1, 200);
    check(!r2.pb && r2.gold[0] && !r2.gold[1] && !r2.gold[2], "slower run with one gold sector");
    check(wrh::sumOfBest(s) == 4800 + 5000 + 5000, "sum of best takes the gold");
    auto r3 = wrh::applyFinish(s, {5100, 9900, 14700}, 0, 300);
    check(r3.pb && s.pbTime() == 14700, "faster run is the new PB");
    check(r3.gold[1] && r3.gold[2] == (14700 - 9900 < 5000), "golds of the new PB");
    check(s.finishes == 3 && s.runs.size() == 3, "history counts finishes");
    auto r4 = wrh::applyFinish(s, {7000, 14000}, 0, 400);
    check(!r4.pb && s.pb.size() == 3 && s.runs.size() == 4, "other split count: history only");

    // rows against PB and a target
    wrh::Comparison t;
    t.name = "TMX record";
    t.time = 14000;
    t.splits = {4700, 9500, 14000};
    const auto rows = wrh::buildRows({5000, 9800}, s, t);
    check(rows.size() == 3 && rows[2].finish, "three rows, the last the finish");
    check(rows[0].dPb == 5000 - 5100 && rows[0].dTarget == 300, "deltas at the first split");
    check(rows[1].dPb == 9800 - 9900 && rows[1].prevDPb == -100, "previous delta kept for the colour");
    check(rows[1].sector == 4800 && rows[2].time == -1, "sector time, finish not reached");
    check(rows[1].isGold == (4800 < s.golds[1]), "gold flag against the stored gold");
    check(wrh::runningDelta({5000}, 9000, t.splits) == wrh::kNoDelta, "in time: no running delta");
    check(wrh::runningDelta({5000}, 9800, t.splits) == 300, "behind the next split: running delta");

    // JSON round trip
    s.key = "Abc";
    s.uid = "Abc";
    s.name = "Map \"one\"";
    s.attempts = 9;
    s.resets = 4;
    wrh::MapStats back;
    check(wrh::fromJson(wrh::toJson(s), &back), "JSON parses");
    check(back.name == s.name && back.attempts == 9 && back.resets == 4 && back.pb == s.pb && back.golds == s.golds && back.runs.size() == s.runs.size(),
          "JSON round trip keeps everything");
    check(!wrh::fromJson("{oops", &back), "bad JSON refused");
    check(wrh::fileKey("a/b:c") == "a_b_c" && wrh::localKey("A01", 24000).compare(0, 6, "local-") == 0, "file keys");
}

void testIdTable() {
    // CMwId::GetString of TMNF (0x935400) and ESWC 1.80 (0x4940b0), the first 0x40 bytes
    const unsigned char tmnf[] = {0x8B, 0x01, 0x8B, 0xC8, 0x81, 0xE1, 0x00, 0x00, 0x00, 0xC0, 0x81, 0xF9, 0x00, 0x00, 0x00, 0x40, 0x74, 0x0B, 0x81, 0xF9, 0x00, 0x00,
                                  0x00, 0x80, 0x74, 0x03, 0x33, 0xC0, 0xC3, 0x25, 0xFF, 0xFF, 0xFF, 0x3F, 0x0F, 0xB7, 0xC8, 0x56, 0x8B, 0x35, 0xE0, 0x39, 0xD7, 0x00,
                                  0xC1, 0xE8, 0x10, 0x51, 0x8D, 0x4C, 0xC6, 0x0C, 0xE8, 0xF7, 0x72, 0xD6, 0xFF, 0x8B, 0x10, 0x52, 0x8B, 0xCE, 0xE8, 0xAD};
    const unsigned char eswc[] = {0x8B, 0x01, 0x8B, 0xC8, 0x81, 0xE1, 0x00, 0x00, 0x00, 0xC0, 0x81, 0xF9, 0x00, 0x00, 0x00, 0x40, 0x74, 0x0B, 0x81, 0xF9, 0x00, 0x00,
                                  0x00, 0x80, 0x74, 0x03, 0x33, 0xC0, 0xC3, 0x25, 0xFF, 0xFF, 0xFF, 0x3F, 0x8B, 0xC8, 0xC1, 0xE9, 0x10, 0x8D, 0x14, 0x49, 0x8B, 0x0D,
                                  0x9C, 0x63, 0xC9, 0x00, 0x8B, 0x54, 0x91, 0x14, 0x25, 0xFF, 0xFF, 0x00, 0x00, 0x8B, 0x04, 0x82, 0x03, 0x41, 0x04, 0xC3};
    ck::IdTable t;
    check(ck::decodeIdTable(tmnf, sizeof(tmnf), &t) && t.global == 0xd739e0 && t.stride == 8 && t.base == 0x10, "TMNF id table decoded");
    check(ck::decodeIdTable(eswc, sizeof(eswc), &t) && t.global == 0xc9639c && t.stride == 12 && t.base == 0x14, "ESWC id table decoded");

    // resolve names through fake memory (ESWC layout) and find the UID triple in a challenge
    FakeMem m;
    const uintptr_t G = 0x500000, T = 0x510000, pool = 0x520000, data = 0x530000, ch = 0x540000;
    m.u32(G, T);
    m.u32(T + 4, pool);
    m.u32(T + 0x14, data); // bucket 0
    const char* names[] = {"x", "UidOfTheTestMap000000000aB", "Stadium", "nadeo"};
    uint32_t at = 0;
    for (uint32_t i = 0; i < 4; i++) {
        m.u32(data + i * 4, at);
        m.str(pool + at, names[i]);
        at += static_cast<uint32_t>(strlen(names[i]) + 1);
    }
    for (uint32_t o = 0; o < 0x300; o += 4) m.u32(ch + o, 0);
    m.u32(ch + 0x60, 0x40000000u | 0); // "x": not a UID
    m.u32(ch + 0xc8, 0x40000000u | 1);
    m.u32(ch + 0xcc, 0x40000000u | 2);
    m.u32(ch + 0xd0, 0x40000000u | 3);
    check(ck::decodeIdTable(eswc, sizeof(eswc), &t), "decode for the fake");
    t.global = G;
    check(ck::idName(m, t, 0x40000002u) == "Stadium", "id name read");
    check(ck::idName(m, t, 0x00000002u).empty(), "a numbered id has no name");
    int off = -1;
    check(ck::challengeUid(m, t, ch, &off) == "UidOfTheTestMap000000000aB" && off == 0xc8, "UID found by scanning the challenge");
    check(ck::plausibleUid("cbgiuwy8lVj1JajzrehqQk0GkMb") && !ck::plausibleUid("Stadium"), "UID plausibility");
}

void testReader() {
    FakeMem m;
    const uintptr_t race = 0x100000, items = 0x110000, player = 0x120000, info = 0x130000, gm = 0x140000, veh = 0x150000, cps = 0x160000;
    ck::Layout L = ck::defaultLayout(ck::Engine::Forever);
    for (uintptr_t o = 0; o < 0x800; o += 4) {
        m.u32(race + o, 0);
        m.u32(info + o, 0);
        m.u32(veh + o, 0);
        m.u32(player + o, 0);
        m.u32(gm + o, 0);
    }
    m.cls[race] = "CTrackManiaRace1P";
    m.cls[info] = "CTrackManiaPlayerInfo";
    m.cls[veh] = "CSceneVehicleCar";
    m.u32(race + L.racePlayers, 1);
    m.u32(race + L.racePlayers + 4, items);
    m.u32(items, player);
    m.u32(race + L.raceTarget, info);
    m.u32(player + L.playerInfo, info);
    m.u32(player + L.playerMobil, gm);
    m.u32(gm + L.mobilScene, veh);
    m.u32(info + L.piState, 1);
    m.u32(info + L.piStart, 1000);
    m.u32(info + L.piNow, 1000 + 7400);
    m.u32(info + L.piCpList, 2);
    m.u32(info + L.piCpList + 4, cps);
    m.u32(cps, 3100);
    m.u32(cps + 4, 0);
    m.u32(cps + 8, 6900);
    m.u32(cps + 12, 0);
    m.f32(veh + L.vSteer, -0.5f);
    m.f32(veh + L.vGas, 1.0f);
    m.f32(veh + L.vBrake, 0.0f);
    m.u32(veh + L.vGear, 3);
    m.f32(veh + L.vRpm, 6500.0f);
    m.u32(race + L.raceSpeed, 214);
    ck::Reader r(L);
    r.poll(m, race);
    check(r.run().race && r.run().state == 1 && r.run().timeMs == 7400, "run state and clock");
    check(r.run().cps.size() == 2 && r.run().cps[1] == 6900 && r.run().cpsFromList, "checkpoint list read");
    check(r.car().ok && r.car().steer == -0.5f && r.car().gas == 1.0f && r.car().haveGear && r.car().gear == 3 && r.car().haveSpeed &&
              r.car().speedKmh == 214.0f,
          "car read");
    m.f32(veh + L.vGas, 37.0f); // garbage: wrong offset
    r.poll(m, race);
    check(!r.car().ok, "implausible input refused");
    m.u32(cps, 0xffffffffu); // a list with "not yet" entries stops there
    r.poll(m, race);
    check(r.run().cps.empty(), "times -1 end the list");
    r.poll(m, 0);
    check(!r.run().race, "no race");

    // counter fallback (no list)
    ck::Layout L2 = L;
    L2.piCpList = -1;
    ck::Reader r2(L2);
    m.u32(info + L.piCurCp, 0);
    m.u32(info + L.piNow, 1000 + 2000);
    r2.poll(m, race);
    m.u32(info + L.piCurCp, 1);
    m.u32(info + L.piNow, 1000 + 3120);
    r2.poll(m, race);
    check(r2.run().cps.size() == 1 && r2.run().cps[0] == 3120 && !r2.run().cpsFromList, "counter fallback samples the clock");
    m.u32(info + L.piNow, 1000 + 500); // restart without countdown
    m.u32(info + L.piCurCp, 0);
    r2.poll(m, race);
    check(r2.run().cps.empty(), "restart clears the sampled splits");

    // the best ghost
    const uintptr_t ghosts = 0x170000, g = 0x180000, gc = 0x190000;
    for (uintptr_t o = 0; o < 0x100; o += 4) m.u32(g + o, 0);
    m.cls[g] = "CGameCtnGhost";
    m.u32(race + L.raceGhosts, 1);
    m.u32(race + L.raceGhosts + 4, ghosts);
    m.u32(ghosts, g);
    m.u32(g + L.ghostTime, 9000);
    m.u32(g + L.ghostCps, 2);
    m.u32(g + L.ghostCps + 4, gc);
    m.u32(gc, 4000);
    m.u32(gc + 4, 0);
    m.u32(gc + 8, 9000);
    m.u32(gc + 12, 0);
    m.u32(cps, 3100);
    r.poll(m, race);
    ck::GhostTimes gt;
    check(r.bestGhost(m, &gt) && gt.raceTime == 9000 && gt.cps.size() == 2, "ghost times read");

    // layout resolution by name
    std::string log;
    auto fn = [](const char* c, const char* mem) -> int {
        if (!strcmp(c, "CSceneVehicleCar") && !strcmp(mem, "InputSteer")) return 0x99;
        if (!strcmp(c, "CSceneVehicleCar") && !strcmp(mem, "InputGas")) return 0x9c;
        if (!strcmp(c, "CSceneVehicleCar") && !strcmp(mem, "InputBrake")) return 0xa0;
        if (!strcmp(c, "CTrackManiaPlayerInfo") && !strcmp(mem, "RaceState")) return 0x400;
        return -1;
    };
    const ck::Layout R = ck::resolveLayout(ck::Engine::Forever, fn, &log);
    check(R.vSteer == 0x99 && R.vNamed && R.piState == 0x400 && R.raceTarget == 0x98, "layout: names win, defaults stay");
}

void testReplay() {
    // a minimal replay (uncompressed body), the same builder the fake bridge uses
    std::string h = u32s(2) + u32s(3) + u32s(0x40000000) + u32s(4) + "UIDx" + u32s(0x40000000) + u32s(7) + "Stadium" + u32s(0x40000000) + u32s(5) +
                    "nadeo" + u32s(9000) + u32s(3) + "Bob";
    std::string user = u32s(1) + u32s(0x03093000) + u32s(static_cast<uint32_t>(h.size())) + h;
    std::string body = u32s(0x03092005) + u32s(0x534B4950) + u32s(4) + u32s(9000) + u32s(0x0309200B) + u32s(0x534B4950) + u32s(4 + 2 * 8) + u32s(2) +
                       u32s(4000) + u32s(0) + u32s(9000) + u32s(0);
    std::string f = std::string("GBX\x06\x00", 5) + "BUUR" + u32s(0x03093000) + u32s(static_cast<uint32_t>(user.size())) + user + u32s(3) + u32s(0) + body;
    std::vector<unsigned char> bytes(f.begin(), f.end());
    ck::Replay r;
    std::string err;
    check(ck::parseReplay(bytes, true, r, err) && r.mapUid == "UIDx" && r.driver == "Bob" && r.raceTime == 9000 && r.checkpoints.size() == 2,
          "replay parsed");
    bytes[bytes.size() - 8] = 0x10; // finish time != race time
    check(!ck::parseReplay(bytes, true, r, err), "unfinished run refused");
    std::vector<unsigned char> junk = {'G', 'B', 'X', 6, 0, 'B', 'U', 'U', 'R', 1, 2, 3, 4};
    check(!ck::parseReplay(junk, true, r, err), "not a replay refused");
    // LZO: a literal-only stream
    const unsigned char lzo[] = {0x11 + 5, 'h', 'e', 'l', 'l', 'o', 0x11, 0x00, 0x00};
    unsigned char out[5];
    check(ck::lzoDecompress(lzo, sizeof(lzo), out, 5) && memcmp(out, "hello", 5) == 0, "LZO literal run");
}

// The host's CHECKPOINT events as TM Original sent them (0.3.0-rc test log 2026-11-12, RaceA1): about 50 events within
// 0.13 s at each of the 6 checkpoints, each with the next index, times -1 (first run) or 19500 every time (second run).
void testCpEvents() {
    const int cps[] = {4900, 7300, 11100, 15400, 16500, 18600};
    for (int hostTime : {-1, 19500}) {
        wrh::CpEvents ev;
        const uint64_t t0 = 1000000;
        ev.start(t0);
        std::vector<int> live;
        for (int cp : cps)
            for (int k = 0; k < 50; k++) {
                int split = -1;
                const int last = live.empty() ? 0 : live.back();
                if (ev.add(hostTime, t0 + static_cast<uint64_t>(cp) + static_cast<uint64_t>(k) * 2u, last, &split)) live.push_back(split);
            }
        check(live.size() == 6, "a burst of checkpoint events counts once (6 checkpoints, 300 events)");
        bool times = live.size() == 6;
        for (size_t i = 0; i < live.size() && i < 6; i++) times = times && live[i] == cps[i];
        check(times, "unknown or stale host times replaced by the run's clock");
        check(ev.justTaken(t0 + 18600 + 150) && !ev.justTaken(t0 + 21100), "finish right after a burst is seen");
    }
    // ESWC: exact times from the game's list, two checkpoints in the same poll are both kept
    wrh::CpEvents ev;
    ev.start(5000);
    int a = -1, b = -1, c = -1;
    check(ev.add(5300, 5000 + 5310, 0, &a) && a == 5300, "ESWC time taken");
    check(ev.add(5420, 5000 + 5310, 5300, &b) && b == 5420, "second checkpoint in the same poll kept");
    check(!ev.add(5420, 5000 + 5320, 5420, &c), "the same time again is the same checkpoint");
    // a time that lags the clock (pause) is still the game's
    check(ev.add(9000, 5000 + 15000, 5420, &c) && c == 9000, "host time behind the clock kept");
    // plugin switched on mid-run (no start): the host's times
    wrh::CpEvents late;
    check(late.add(12000, 77, 0, &c) && c == 12000, "no start seen: host time");

    // the stats file 0.3.0-rc wrote on Original: ~280 splits of -1 as the PB
    wrh::MapStats bad;
    bad.key = "pfG3yOqWJaTihqfo7OOE4Pf9iBf";
    bad.pb.assign(275, -1);
    bad.pb.push_back(21100);
    bad.golds = wrh::sectors(bad.pb);
    wrh::RunRecord rr;
    rr.time = 21100;
    rr.splits = bad.pb;
    bad.runs.push_back(rr);
    bad.finishes = 1;
    wrh::MapStats back;
    check(wrh::fromJson(wrh::toJson(bad), &back) && back.pb.empty() && back.golds.empty() && back.runs.size() == 1 &&
              back.runs[0].splits == std::vector<int>{21100},
          "broken PB dropped on load, the run keeps its time");
    // a run with unknown split times: only its time; the next run with real splits replaces that PB when faster
    wrh::MapStats s;
    std::vector<int> unknown(6, -1);
    unknown.push_back(21100);
    wrh::applyFinish(s, unknown, -1, 1);
    check(s.pb == std::vector<int>{21100}, "unknown splits: the PB is the final time only");
    const wrh::FinishResult r = wrh::applyFinish(s, {4900, 7300, 11100, 15400, 16500, 18600, 21000}, -1, 2);
    check(r.pb && s.pb.size() == 7 && s.golds.size() == 7, "faster run with splits replaces a time-only PB");
    check(wrh::saneSplits({1, 2, 3}) && !wrh::saneSplits({1, 1}) && !wrh::saneSplits({}) && !wrh::saneSplits({-1, 5}), "sane split lists");
}

} // namespace

int main() {
    testSplits();
    testCpEvents();
    testIdTable();
    testReader();
    testReplay();
    printf("wr_hunter_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
