// The exes the host knows, and the pure matching rule (unit-tested in tests/unit_tests.cpp). The exe is pinned by
// SHA-256, then PE timestamp and file size where given. Anything else: the host stays off and says why.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "horizon_plugin.h"

namespace horizon {

struct KnownExe {
    const char* sha256;
    uint32_t timestamp; // 0 = not checked
    uint32_t game;
    const char* name;
    const char* version;
    uint64_t size; // file size in bytes, 0 = not checked
};

// Measured on local copies (tm-modding-knowledge README, trackmania-internals README, exe identification).
inline const KnownExe* knownExes(size_t* count) {
    static const KnownExe k[] = {
        {"983bd12724739dcb0f876aeb3a4259154b068940f5167f57ede750ca452d8068", 0x5ce7272f, HORIZON_GAME_ESWC, "TrackMania Nations ESWC",
         "0.1.7.9 (unofficial 1.80)", 0},
        {"e59dfc7eaa5314e85b2b0df6f40ef7ba01a23bd5a9525f84407f5dff301df4fe", 0x5cc8f0cf, HORIZON_GAME_SUNRISE, "TrackMania Sunrise",
         "Win10 community repack", 7904768},
        {"3847cf9f20bfc63914450060ed528c12104f743d96ad23d6e76abd178de8c84f", 0x4d431494, HORIZON_GAME_TMNF, "TrackMania Nations Forever",
         "2.11.26", 10498048},
        {"4b6a7b31d86766409e94101f1256cd61dffecb23ea497a40b6617506b2ced2d4", 0x4b9e8add, HORIZON_GAME_TMUF, "TrackMania United Forever",
         "2.11.26", 10493952},
        // TMLoader's own TmForever builds (database\TmForever\products\TmForever\<version>\TmForever.exe).
        {"8c5325d04a10d5e6921afb9931770279cca8cdfc7f66425b6365d0a870f5694a", 0, HORIZON_GAME_TMNF, "TrackMania Forever (TMLoader)",
         "TMLoader 2.12.0", 0},
        {"0a885e194131063be9f78d9e5cf81e0f3017dfc4331d176812109b6aefc633bf", 0, HORIZON_GAME_TMNF, "TrackMania Forever (TMLoader)",
         "TMLoader 2.12.0-compat", 0},
        // TM Original: GreffMASTER's "Online Patch v2" (2023) exe, made from the community StarForce-free build (master
        // server www.trackmania.com -> game.trackmania.com). Other TM Original exes (the StarForce retail exe, patch v1)
        // are refused. Addresses: trackmania-internals games/tm-original.
        {"75e129d769f4595ddde54a8ef484162915bc2bf998c1a2139cc963294fe8175e", 0x5d337477, HORIZON_GAME_ORIGINAL, "TrackMania Original",
         "Online Patch v2 (community build)", 7883264},
    };
    *count = sizeof(k) / sizeof(k[0]);
    return k;
}

// PE timestamp of the TM Original community build: an exe with it but another hash is a TM Original we do not know.
constexpr uint32_t kOriginalTimestamp = 0x5d337477;

struct ExeMatch {
    const KnownExe* exe = nullptr; // set when supported
    std::string why;               // set when not
};

// sha256: lower-case hex; size: file size (0 = unknown, fails a size check); exeName: file name of the exe.
inline ExeMatch matchExe(const std::string& sha256, uint32_t timestamp, uint64_t size, const char* exeName) {
    ExeMatch m;
    size_t n = 0;
    const KnownExe* k = knownExes(&n);
    for (size_t i = 0; i < n; i++) {
        if (sha256 != k[i].sha256) continue;
        if (k[i].timestamp && k[i].timestamp != timestamp) {
            m.why = "exe hash known but PE timestamp differs";
            return m;
        }
        if (k[i].size && k[i].size != size) {
            m.why = "exe hash known but file size differs";
            return m;
        }
        m.exe = &k[i];
        return m;
    }
    if (timestamp == kOriginalTimestamp || (exeName && _stricmp(exeName, "TMORIGINAL.EXE") == 0)) {
        m.why = "TrackMania Original, but not the supported exe (only the Online Patch v2 exe, SHA-256 75e129d7...)";
        return m;
    }
    m.why = "unknown exe (not a supported version of ESWC, Sunrise, Original or Forever)";
    return m;
}

} // namespace horizon
