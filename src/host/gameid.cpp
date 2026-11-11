// Which game runs: the exe is pinned by SHA-256 (and PE timestamp, and file size where given; table and rule in
// exe_known.h). Unknown exe -> host stays off.
#include "host/host.h"

#include "common/util.h"
#include "host/exe_known.h"

namespace horizon {

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
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    const uint64_t size = GetFileAttributesExW(g.exePath.c_str(), GetFileExInfoStandard, &fa)
                              ? (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow
                              : 0;
    const std::string name = utf8(g.exePath.substr(g.exeFolder.size()));
    const ExeMatch m = matchExe(g.sha256, g.timestamp, size, name.c_str());
    if (m.exe) {
        g.supported = true;
        g.game = m.exe->game;
        g.name = m.exe->name;
        g.version = m.exe->version;
        return g;
    }
    if (testMode) {
        g.supported = true;
        g.game = 0;
        g.name = "Test program";
        g.version = "test";
        return g;
    }
    g.why = m.why;
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
