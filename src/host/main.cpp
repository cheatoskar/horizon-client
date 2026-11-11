// host.dll: started by the Legacy Loader (HorizonHostInit, game main thread, from the game's first
// DirectInput8Create) or, on TrackMania Forever, by TMLoader (DllMain starts a thread: loader lock).
#include "host/host.h"

#include <shlobj.h>

#include <cstdio>

#include "common/util.h"
#include "host/bridge.h"
#include "host/stats.h"
#include "gamectl/gamectl.h"
#include "reflect/reflect.h"

namespace horizon {
namespace {
HostState g_state;
HMODULE g_module = nullptr;
volatile LONG g_started = 0;

void gamectlLog(const char* line) { logLine(HORIZON_LOG_INFO, "gamectl", "%s", line ? line : ""); }

const char* loaderName(LoaderKind k) {
    switch (k) {
    case LoaderKind::Legacy: return "Legacy Loader (dinput8.dll)";
    case LoaderKind::TMLoader: return "TMLoader";
    default: return "test program";
    }
}
} // namespace

HostState& state() { return g_state; }
void setHostModule(HMODULE m) { g_module = m; }

// Returns 1 when the host runs, 0 when it stays off (the log says why).
int start(LoaderKind loader, bool safeMode, const char* safeWhy) {
    if (InterlockedExchange(&g_started, 1) != 0) return 1;
    HostState& s = g_state;
    s.loader = loader;
    s.mainThread = loader == LoaderKind::TMLoader ? 0 : GetCurrentThreadId(); // Forever: set by the first frame
    s.safeMode = safeMode;
    s.safeWhy = safeWhy ? safeWhy : "";
    s.baseFolder = folderOf(modulePath(g_module));
    if (loader == LoaderKind::TMLoader) {
        // the product folder (...\products\Horizon Host\) above the version folder: settings and logs
        // survive an update of the product
        const std::wstring up = folderOf(s.baseFolder.substr(0, s.baseFolder.size() - 1));
        s.dataRoot = up.empty() ? s.baseFolder : up;
    } else {
        s.dataRoot = s.baseFolder;
    }
    ensureDir(s.dataRoot);
    logOpen(s.dataRoot + L"logs\\", 20);
    HLOG("---- Horizon Host %s (plugin ABI %d.%d, Dear ImGui %s) start via %s, thread %lu ----", kHostVersion, HORIZON_ABI,
         HORIZON_ABI_MINOR, "1.91.5", loaderName(loader), GetCurrentThreadId());
    HLOG("folders: host %s, data %s", utf8(s.baseFolder).c_str(), utf8(s.dataRoot).c_str());

    s.game = identifyGame(loader == LoaderKind::Test);
    HLOG("exe: %s (sha256 %s, PE timestamp %08X, base %08X)", utf8(s.game.exePath).c_str(), s.game.sha256.c_str(),
         static_cast<unsigned>(s.game.timestamp), static_cast<unsigned>(s.game.base));
    if (!s.game.supported) {
        HERR("host stays OFF: %s. Nothing was hooked; the game runs as without the host.", s.game.why.c_str());
        return 0;
    }
    HLOG("game: %s %s", s.game.name.c_str(), s.game.version.c_str());
    s.active = true;

    // An earlier dinput8.dll chained by the Legacy Loader (renamed to dinput8.chain.dll on install)
    if (loader == LoaderKind::Legacy) {
        const std::wstring chain = s.game.exeFolder + L"dinput8.chain.dll";
        std::string bytes;
        if (readFile(chain, bytes)) {
            s.chainPresent = true;
            const wchar_t* mark = L"eswc_autosave.dll";
            const std::string needle(reinterpret_cast<const char*>(mark), wcslen(mark) * sizeof(wchar_t));
            s.chainIsAutosave = bytes.find(needle) != std::string::npos;
            HLOG("chain: dinput8.chain.dll is loaded before the system dinput8 (%s)",
                 s.chainIsAutosave ? "it is the standalone ESWC Autosave: the Autosave plugin pauses itself" : "another mod");
            if (s.chainIsAutosave)
                s.notes.push_back("The standalone ESWC Autosave (dinput8.chain.dll) is active, so the Autosave module is paused. "
                                  "To use the module instead: close the game, delete dinput8.chain.dll.");
        }
    }

    // game info for plugins
    s.infoGameFolder = utf8(s.game.exeFolder);
    s.info.size = sizeof(HorizonGameInfo);
    s.info.game = s.game.game;
    s.info.game_name = s.game.name.c_str();
    s.info.exe_version = s.game.version.c_str();
    s.info.exe_sha256 = s.game.sha256.c_str();
    s.info.exe_timestamp = s.game.timestamp;
    s.info.exe_base = s.game.base;
    s.info.language = s.infoLanguage.c_str();
    s.info.game_folder = s.infoGameFolder.c_str();
    s.info.user_folder = s.infoUserFolder.c_str();
    s.info.host_version = kHostVersion;
    s.info.loader = static_cast<uint32_t>(loader);
    s.info.safe_mode = safeMode ? 1u : 0u;

    gamectl::init(&gamectlLog, false);
    hostConfigLoad();
    pluginsReadProfile();
    if (s.safeMode) HLOG("safe mode: %s", s.safeWhy.c_str());
    statsStart();

    GameAdapter* adapter = nullptr;
    switch (s.game.game) {
    case HORIZON_GAME_ESWC: adapter = makeEswcAdapter(); break;
    case HORIZON_GAME_SUNRISE: adapter = makeSunriseAdapter(); break;
    case HORIZON_GAME_TMNF:
    case HORIZON_GAME_TMUF: adapter = makeForeverAdapter(); break;
    default: adapter = makeNullAdapter(); break;
    }
    setGame(adapter);
    adapter->install();
    eventsInit();
    if (s.game.game == HORIZON_GAME_ESWC) {
        if (hostSettingInt("OnlineFix", 1)) onlineFixInstall(s.game.base);
        else HLOG("online: OnlineFix=0 in host.json, login left as it is");
    }
    if (adapter->usesDirectInputBlock()) dinputInstall();
    else HLOG("input: not blocked while the menu is open on this game (Competition Patch rule)");
    overlayCreateContext();
    d3dInstall();
    bridgeStart();
    pluginsLoadAll();
    hostConfigSave(true);
    HLOG("ready: %zu plugin folder(s) found", pluginCount());
    return 1;
}

} // namespace horizon

namespace horizon {
int start(LoaderKind loader, bool safeMode, const char* safeWhy);

// An exception during start must not reach the game (its own handlers would carry on in a broken state).
int startGuarded(LoaderKind loader, bool safeMode, const char* safeWhy) {
    __try {
        return start(loader, safeMode, safeWhy);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logLine(HORIZON_LOG_ERROR, "host", "exception 0x%08lx during start: the host stops here (nothing more is installed)",
                GetExceptionCode());
        return 0;
    }
}
}

// Must match LoaderInfo in src/loader/loader.cpp.
struct HorizonLoaderInfo {
    unsigned api;
    unsigned size;
    unsigned flags; // 1 = safe mode (Shift held), 2 = test program (unknown exe allowed)
    const wchar_t* loaderPath;
};

extern "C" __declspec(dllexport) int __cdecl HorizonHostInit(const HorizonLoaderInfo* info) {
    if (!info || info->api != 1 || info->size < sizeof(HorizonLoaderInfo)) {
        OutputDebugStringW(L"Horizon Host: unknown loader version, host off\n");
        return 0;
    }
    const bool safe = (info->flags & 1u) != 0;
    const bool test = (info->flags & 2u) != 0;
    return horizon::startGuarded(test ? horizon::LoaderKind::Test : horizon::LoaderKind::Legacy, safe, safe ? "Shift held at start" : "");
}

namespace {
DWORD WINAPI tmloaderBoot(LPVOID) {
    const bool safe = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    horizon::startGuarded(horizon::LoaderKind::TMLoader, safe, safe ? "Shift held at start" : "");
    return 0;
}
} // namespace

// TMLoader calls DllMain through its export (_DllMain@12). The Legacy Loader calls HorizonHostInit instead.
extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* name = n ? wcsrchr(exe, L'\\') : nullptr;
        name = name ? name + 1 : exe;
        horizon::setHostModule(module);
        // Loaded by the Legacy Loader (host.dll in <exe folder>\horizon\): that one calls HorizonHostInit.
        bool legacy = false;
        wchar_t self[MAX_PATH];
        const DWORD m = GetModuleFileNameW(module, self, MAX_PATH);
        const size_t dirLen = static_cast<size_t>(name - exe);
        if (n && m && dirLen + 20 < MAX_PATH) {
            wchar_t expect[MAX_PATH];
            wcsncpy_s(expect, exe, dirLen);
            wcscat_s(expect, L"horizon\\host.dll");
            legacy = _wcsicmp(expect, self) == 0;
        }
        if (!legacy && _wcsicmp(name, L"TmForever.exe") == 0) {
            HANDLE t = CreateThread(nullptr, 0, &tmloaderBoot, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
