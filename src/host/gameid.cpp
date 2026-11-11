// Which game runs: the exe is pinned by SHA-256 (and PE timestamp). Unknown exe -> host stays off.
#include "host/host.h"

#include "common/util.h"

namespace horizon {
namespace {
struct KnownExe {
    const char* sha256;
    uint32_t timestamp; // 0 = not checked
    uint32_t game;
    const char* name;
    const char* version;
};
// Measured on local copies (tm-modding-knowledge README, exe identification).
const KnownExe kKnown[] = {
    {"983bd12724739dcb0f876aeb3a4259154b068940f5167f57ede750ca452d8068", 0x5ce7272f, HORIZON_GAME_ESWC,
     "TrackMania Nations ESWC", "0.1.7.9 (unofficial 1.80)"},
    {"e59dfc7eaa5314e85b2b0df6f40ef7ba01a23bd5a9525f84407f5dff301df4fe", 0x5cc8f0cf, HORIZON_GAME_SUNRISE, "TrackMania Sunrise",
     "Win10 community repack"},
    {"3847cf9f20bfc63914450060ed528c12104f743d96ad23d6e76abd178de8c84f", 0x4d431494, HORIZON_GAME_TMNF,
     "TrackMania Nations Forever", "2.11.26"},
    {"4b6a7b31d86766409e94101f1256cd61dffecb23ea497a40b6617506b2ced2d4", 0x4b9e8add, HORIZON_GAME_TMUF,
     "TrackMania United Forever", "2.11.26"},
    // TMLoader's own TmForever builds (database\TmForever\products\TmForever\<version>\TmForever.exe).
    {"8c5325d04a10d5e6921afb9931770279cca8cdfc7f66425b6365d0a870f5694a", 0, HORIZON_GAME_TMNF, "TrackMania Forever (TMLoader)",
     "TMLoader 2.12.0"},
    {"0a885e194131063be9f78d9e5cf81e0f3017dfc4331d176812109b6aefc633bf", 0, HORIZON_GAME_TMNF, "TrackMania Forever (TMLoader)",
     "TMLoader 2.12.0-compat"},
};
} // namespace

GameIdentity identifyGame(bool testMode) {
    GameIdentity g;
    HMODULE exe = GetModuleHandleW(nullptr);
    g.base = reinterpret_cast<uintptr_t>(exe);
    g.exePath = modulePath(nullptr);
    g.exeFolder = folderOf(g.exePath);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(g.base + dos->e_lfanew);
    g.timestamp = nt->FileHeader.TimeDateStamp;
    if (!sha256File(g.exePath, g.sha256)) {
        g.why = "could not read the exe to check its version";
        return g;
    }
    for (const auto& k : kKnown) {
        if (g.sha256 == k.sha256) {
            if (k.timestamp && k.timestamp != g.timestamp) {
                g.why = "exe hash known but PE timestamp differs";
                return g;
            }
            g.supported = true;
            g.game = k.game;
            g.name = k.name;
            g.version = k.version;
            return g;
        }
    }
    if (testMode) {
        g.supported = true;
        g.game = 0;
        g.name = "Test program";
        g.version = "test";
        return g;
    }
    g.why = "unknown exe (not a supported version of ESWC, Sunrise or Forever)";
    return g;
}

const char* gameStateText(int s) {
    switch (s) {
    case HORIZON_STATE_MAIN_MENU: return "Main menu";
    case HORIZON_STATE_SOLO_RACE: return "Solo race";
    case HORIZON_STATE_END_RACE: return "End of race";
    case HORIZON_STATE_NET_RACE: return "Online race";
    case HORIZON_STATE_EDITOR: return "Editor";
    case HORIZON_STATE_REPLAY: return "Replay";
    case HORIZON_STATE_LOADING: return "Loading";
    default: return "Unknown";
    }
}

} // namespace horizon
