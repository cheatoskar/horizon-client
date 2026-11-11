#include "gameread.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "reflect/reflect.h"

namespace ck {

Layout defaultLayout(Engine e) {
    Layout L;
    L.engine = e;
    if (e == Engine::Legacy) {
        // ESWC values (reflection dump); Sunrise differs and is resolved by name at runtime.
        L.racePlayers = 0x20;
        L.raceTarget = 0x10c;
        L.raceSpeed = -1;   // LocalCarSpeed is a getter there (no field)
        L.raceClock = -1;
        L.raceGhosts = -1;
        L.playerInfo = 0x34;
        L.playerMobil = 0x24;
        L.mobilScene = 0x14;
        L.piState = -1;
        L.piTime = 0x1f4;
        L.piStart = -1;
        L.piNow = 0x258;
        L.piCurCp = 0x22c;
        L.piCpList = -1;    // the host sends CHECKPOINT events on ESWC and Sunrise
        L.ghostTime = -1;
        L.ghostCps = -1;
        L.vehicleClass = "CSceneToyVehicle";
        L.vSteer = 0xc0;
        L.vGas = 0xb8;
        L.vBrake = 0xbc;
        L.vGear = -1;       // CurGear / CurRPM are getters there
        L.vRpm = -1;
    }
    return L;
}

namespace {
void take(int* slot, OffsetFn fn, const char* cls, const char* member, std::string* log, bool* found = nullptr) {
    const int v = fn ? fn(cls, member) : -1;
    if (v >= 0) {
        *slot = v;
        if (found) *found = true;
    } else if (log) {
        *log += std::string(cls) + "." + member + " ";
    }
}
} // namespace

Layout resolveLayout(Engine e, OffsetFn fn, std::string* log) {
    Layout L = defaultLayout(e);
    std::string missing;
    if (e == Engine::Forever) {
        take(&L.racePlayers, fn, "CGamePlayground", "Players", &missing);
        take(&L.raceTarget, fn, "CTrackManiaRace", "TMTargetPlayerInfo", &missing);
        take(&L.raceSpeed, fn, "CTrackManiaRace", "CarSpeedDisplay", &missing);
        take(&L.raceGhosts, fn, "CTrackManiaRace", "RaceGhosts", &missing);
        take(&L.playerInfo, fn, "CGamePlayer", "PlayerInfo", &missing);
        take(&L.playerMobil, fn, "CGamePlayer", "Mobil", &missing);
        take(&L.mobilScene, fn, "CGameMobil", "SceneMobil", &missing);
        take(&L.ghostTime, fn, "CGameCtnGhost", "RaceTime", &missing);
    } else {
        take(&L.racePlayers, fn, "CConstructionRace", "Players", &missing);
        take(&L.raceTarget, fn, "CTrackManiaRace", "TMTargetPlayerInfo", &missing);
        take(&L.playerInfo, fn, "CConstructionPlayer", "PlayerInfo", &missing);
        take(&L.playerMobil, fn, "CGamePlayer", "Mobil", &missing);
        take(&L.mobilScene, fn, "CGameMobil", "Mobil", &missing);
        take(&L.piNow, fn, "CTrackManiaPlayerInfo", "CurrentTime", &missing);
    }
    take(&L.piState, fn, "CTrackManiaPlayerInfo", "RaceState", e == Engine::Forever ? &missing : nullptr);
    take(&L.piTime, fn, "CTrackManiaPlayerInfo", "RaceTime", &missing);
    take(&L.piStart, fn, "CTrackManiaPlayerInfo", "RaceStartTime", e == Engine::Forever ? &missing : nullptr);
    take(&L.piCurCp, fn, "CTrackManiaPlayerInfo", "CurCheckpoint", &missing);
    if (e == Engine::Forever) take(&L.piNow, fn, "CTrackManiaPlayerInfo", "CurrentTime", &missing);
    // the car: by name when the class has reflection members (ESWC/Sunrise CSceneToyVehicle; Forever's
    // CSceneVehicleCar may not), else the defaults above
    bool s = false, g = false, b = false;
    for (const char* cls : {L.vehicleClass, "CSceneVehicle", "CSceneVehicleCar", "CSceneToyVehicle"}) {
        if (!s) take(&L.vSteer, fn, cls, "InputSteer", nullptr, &s);
        if (!g) take(&L.vGas, fn, cls, "InputGas", nullptr, &g);
        if (!b) take(&L.vBrake, fn, cls, "InputBrake", nullptr, &b);
    }
    L.vNamed = s && g && b;
    if (!L.vNamed) missing += "vehicle InputSteer/InputGas/InputBrake (default offsets) ";
    if (log) *log = missing.empty() ? std::string("all members found by name") : "not found by name: " + missing;
    return L;
}

bool Reader::readTimes(Mem& m, uintptr_t buf, std::vector<int>* out) {
    out->clear();
    uint32_t count = 0, items = 0;
    if (!m.get(buf, &count) || count > static_cast<uint32_t>(kMaxCps)) return false;
    if (count == 0) return true;
    if (!m.get(buf + 4, &items) || items <= 0x10000u) return false;
    std::vector<uint32_t> raw(static_cast<size_t>(count) * 2);
    if (!m.read(items, raw.data(), raw.size() * 4)) return false;
    int prev = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t t = raw[i * 2];
        if (t == 0xffffffffu) break; // not reached yet
        if (t > 24u * 3600u * 1000u || static_cast<int>(t) < prev) return false;
        prev = static_cast<int>(t);
        out->push_back(prev);
    }
    return true;
}

uintptr_t Reader::localPlayer(Mem& m, uintptr_t race, uintptr_t* info) {
    *info = 0;
    uintptr_t target = 0;
    m.ptr(race, L_.raceTarget, &target);
    uint32_t count = 0;
    uintptr_t items = 0;
    if (L_.racePlayers < 0 || !m.get(race + static_cast<uintptr_t>(L_.racePlayers), &count) || count == 0 || count > 256 ||
        !m.ptr(race, L_.racePlayers + 4, &items))
        return 0;
    uintptr_t first = 0, firstInfo = 0;
    for (uint32_t i = 0; i < count && i < 64; i++) {
        uintptr_t p = 0, pi = 0;
        if (!m.ptr(items, static_cast<int>(i * 4), &p) || !m.ptr(p, L_.playerInfo, &pi)) continue;
        if (!first) {
            first = p;
            firstInfo = pi;
        }
        if (target && pi == target) {
            *info = pi;
            return p;
        }
    }
    // no target (solo races before the start): the first player is the local one
    if (!target && first && m.isKind(firstInfo, "CTrackManiaPlayerInfo")) {
        *info = firstInfo;
        return first;
    }
    return 0;
}

void Reader::poll(Mem& m, uintptr_t race) {
    Run r;
    Car c;
    uintptr_t info = 0;
    const uintptr_t player = race && m.isKind(race, "CTrackManiaRace") ? localPlayer(m, race, &info) : 0;
    if (player && info) {
        r.race = true;
        r.raceObj = race;
        r.infoObj = info;
        r.playerObj = player;
        uint32_t st = 0;
        if (L_.piState >= 0 && m.get(info + static_cast<uintptr_t>(L_.piState), &st) && st <= 2) r.state = static_cast<int>(st);
        // the race time: finished -> RaceTime; running -> clock - start
        if (r.state == 2) {
            int32_t t = -1;
            if (L_.piTime >= 0 && m.get(info + static_cast<uintptr_t>(L_.piTime), &t) && t > 0 && t < 86400000) r.timeMs = t;
        } else if (r.state == 1 && L_.piStart >= 0) {
            uint32_t start = 0, now = 0;
            if (m.get(info + static_cast<uintptr_t>(L_.piStart), &start) && start) {
                bool got = L_.piNow >= 0 && m.get(info + static_cast<uintptr_t>(L_.piNow), &now) && now >= start && now - start < 86400000u;
                if (!got && L_.raceClock >= 0)
                    got = m.get(race + static_cast<uintptr_t>(L_.raceClock), &now) && now >= start && now - start < 86400000u;
                if (got) r.timeMs = static_cast<int>(now - start);
            }
        } else if (r.state == 0) {
            r.timeMs = 0;
        }
        // checkpoints: the game's own list (exact) where known, else the time when the counter moved
        bool listOk = false;
        if (L_.piCpList >= 0) {
            std::vector<int> times;
            if (readTimes(m, info + static_cast<uintptr_t>(L_.piCpList), &times)) {
                listOk = true;
                r.cps = times;
                r.cpsFromList = true;
            }
        }
        int32_t cur = 0;
        const bool haveCur = L_.piCurCp >= 0 && m.get(info + static_cast<uintptr_t>(L_.piCurCp), &cur) && cur >= 0 && cur < kMaxCps;
        if (!listOk) {
            bool same = run_.race && run_.infoObj == info && run_.raceObj == race;
            // a restart without a countdown: the clock jumps back
            if (same && r.state == 1 && run_.state == 1 && r.timeMs >= 0 && run_.timeMs > r.timeMs + 100) {
                same = false;
                lastCurCp_ = 0;
            }
            if (same && r.state == 1 && haveCur && cur > lastCurCp_ && r.timeMs > 0) {
                r.cps = run_.cps;
                while (static_cast<int>(r.cps.size()) < cur) r.cps.push_back(r.timeMs);
            } else if (same && r.state != 0 && !(r.state == 1 && run_.state == 2)) {
                r.cps = run_.cps;
            }
            if (same && r.state == 2 && run_.state == 1 && r.timeMs > 0 &&
                (r.cps.empty() || r.cps.back() != r.timeMs))
                r.cps.push_back(r.timeMs); // the finish
        }
        if (haveCur) lastCurCp_ = r.state == 0 ? 0 : cur;
        // the car
        uintptr_t gm = 0, veh = 0;
        if (m.ptr(player, L_.playerMobil, &gm) && m.ptr(gm, L_.mobilScene, &veh)) {
            float s = 0, g = 0, b = 0;
            if (m.get(veh + static_cast<uintptr_t>(L_.vSteer), &s) && m.get(veh + static_cast<uintptr_t>(L_.vGas), &g) &&
                m.get(veh + static_cast<uintptr_t>(L_.vBrake), &b) && plausibleInput(s, g, b) &&
                (L_.vNamed || m.isKind(veh, L_.vehicleClass) || m.isKind(veh, "CSceneVehicle"))) {
                c.ok = true;
                c.steer = s < -1 ? -1 : (s > 1 ? 1 : s);
                c.gas = g < 0 ? 0 : (g > 1 ? 1 : g);
                c.brake = b < 0 ? 0 : (b > 1 ? 1 : b);
            }
            int32_t gear = 0;
            if (c.ok && L_.vGear >= 0 && m.get(veh + static_cast<uintptr_t>(L_.vGear), &gear) && gear >= 0 && gear <= 7) {
                c.haveGear = true;
                c.gear = gear;
            }
            float rpm = 0;
            if (c.ok && L_.vRpm >= 0 && m.get(veh + static_cast<uintptr_t>(L_.vRpm), &rpm) && rpm == rpm && rpm >= 0.0f && rpm < 20000.0f) {
                c.haveRpm = true;
                c.rpm = rpm;
            }
        }
        uint32_t speed = 0;
        if (L_.raceSpeed >= 0 && m.get(race + static_cast<uintptr_t>(L_.raceSpeed), &speed) && speed < 2000u) {
            c.haveSpeed = true;
            c.speedKmh = static_cast<float>(speed);
        }
    }
    run_ = r;
    car_ = c;
}

bool Reader::bestGhost(Mem& m, GhostTimes* out) const {
    *out = GhostTimes();
    if (!run_.raceObj || L_.raceGhosts < 0 || L_.ghostTime < 0 || L_.ghostCps < 0) return false;
    uint32_t n = 0;
    uintptr_t items = 0;
    if (!m.get(run_.raceObj + static_cast<uintptr_t>(L_.raceGhosts), &n) || n == 0 || n > 64 ||
        !m.ptr(run_.raceObj, L_.raceGhosts + 4, &items))
        return false;
    for (uint32_t i = 0; i < n; i++) {
        uintptr_t g = 0;
        int32_t t = -1;
        if (!m.ptr(items, static_cast<int>(i * 4), &g) || !m.isKind(g, "CGameCtnGhost") ||
            !m.get(g + static_cast<uintptr_t>(L_.ghostTime), &t) || t <= 0 || t > 86400000)
            continue;
        if (out->raceTime > 0 && t >= out->raceTime) continue;
        std::vector<int> cps;
        if (!readTimes(m, g + static_cast<uintptr_t>(L_.ghostCps), &cps) || cps.empty() || cps.back() != t) continue;
        out->raceTime = t;
        out->cps = cps;
    }
    return out->raceTime > 0;
}

// ---------------------------------------------------------------- live game
namespace {
bool sehCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
const HorizonHostApi* g_host = nullptr;
bool g_reflect = false;
} // namespace

bool LiveMem::read(uintptr_t addr, void* out, size_t n) {
    if (addr <= 0x10000 || addr + n < addr || addr >= 0x7fff0000u) return false;
    return sehCopy(out, reinterpret_cast<const void*>(addr), n);
}

bool LiveMem::isKind(uintptr_t obj, const char* cls) {
    if (obj <= 0x10000) return false;
    if (!g_reflect) {
        uint32_t vt = 0;
        return get(obj, &vt) && vt > 0x10000u;
    }
    const reflect::ClassInfo* c = reflect::classOfObject(reinterpret_cast<const void*>(obj));
    if (!c) {
        // a class without reflection registration (e.g. Forever's car): accept a readable vtable,
        // the values are checked for plausibility by the caller
        uint32_t vt = 0;
        return get(obj, &vt) && vt > 0x10000u && !reflect::findClass(cls);
    }
    return reflect::isKindOf(c, cls);
}

bool liveReflectInit(const HorizonHostApi* host) {
    g_host = host;
    const HorizonGameInfo* gi = host && host->game_info ? host->game_info() : nullptr;
    g_reflect = gi && reflect::init(gi->exe_timestamp) && reflect::ready();
    return g_reflect;
}

int liveOffset(const char* cls, const char* member) {
    if (g_host && g_host->reflect_offset) return g_host->reflect_offset(cls, member);
    return g_reflect ? reflect::offsetOf(cls, member) : -1;
}

} // namespace ck
