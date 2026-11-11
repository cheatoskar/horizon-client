// dinput8.dll - the Horizon Legacy Loader. A small loader, nothing else.
//
// 1. Every DirectInput export is passed on, unchanged, to the system dinput8.dll.
// 2. The first time the game calls DirectInput8Create (the game's main thread, outside the loader
//    lock, long before a race), it loads horizon\host.dll from the folder this DLL is in (full path,
//    no search order) and calls its HorizonHostInit. Everything else (Direct3D hook, menu, plugins,
//    log) lives in the host.
//
// Keys held at that moment:
//   Shift        safe mode: the host starts, but loads no plugins.
//   Ctrl+Shift   the host is not loaded at all: DirectInput only.
// Chain: if dinput8.chain.dll lies next to this DLL (an earlier dinput8.dll, renamed on install - e.g.
// the eswc-autosave loader), every export goes to that DLL instead of the system one, so the earlier
// mod keeps working (it is loaded even with Shift or Ctrl+Shift held). Without it: the system dinput8.dll.
// Without horizon\host.dll this DLL only passes DirectInput through. It never reads or changes
// input data: DirectInput8Create returns the object of the chained/system DLL.
#include <windows.h>

namespace {
// Version of the call into host.dll (HorizonLoaderInfo); the host refuses a loader it does not know.
constexpr unsigned kLoaderApi = 1;
constexpr unsigned kFlagSafeMode = 1;

// Must match HorizonLoaderInfo in src/host/main.cpp.
struct LoaderInfo {
    unsigned api;
    unsigned size;
    unsigned flags;
    const wchar_t* loaderPath;
};

HMODULE g_real = nullptr;
FARPROC g_procs[6] = {};
const char* const kNames[6] = {"DirectInput8Create", "DllCanUnloadNow",     "DllGetClassObject",
                               "DllRegisterServer",  "DllUnregisterServer", "GetdfDIJoystick"};
HMODULE g_self = nullptr;
volatile LONG g_hostStarted = 0;
wchar_t g_selfPath[MAX_PATH];

void debug(const wchar_t* msg, DWORD err) {
    wchar_t buf[256];
    wsprintfW(buf, L"Horizon Legacy Loader: %s (error %lu)\n", msg, err);
    OutputDebugStringW(buf);
}

// <this DLL's folder>\dinput8.chain.dll, if it is there and is not this very DLL.
HMODULE loadChain() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    wchar_t* slash = nullptr;
    for (wchar_t* p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') slash = p;
    if (!slash || (slash + 1 - path) + 18 >= MAX_PATH) return nullptr;
    lstrcpyW(slash + 1, L"dinput8.chain.dll");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
    HMODULE chain = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!chain) {
        debug(L"dinput8.chain.dll could not be loaded, using the system dinput8.dll", GetLastError());
        return nullptr;
    }
    if (chain == g_self) return nullptr;
    return chain;
}

bool loadReal() {
    if (g_real) return true;
    HMODULE chain = loadChain();
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH); // SysWOW64 for this 32-bit process (WOW64 redirection)
    if (n == 0 || n > MAX_PATH - 16) return false;
    lstrcatW(path, L"\\dinput8.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real) {
        debug(L"could not load the system dinput8.dll", GetLastError());
        return false;
    }
    for (int i = 0; i < 6; i++) {
        FARPROC p = chain ? GetProcAddress(chain, kNames[i]) : nullptr;
        g_procs[i] = p ? p : GetProcAddress(real, kNames[i]);
    }
    g_real = real;
    return true;
}

FARPROC proc(int i) {
    if (!loadReal()) return nullptr;
    return g_procs[i];
}

bool held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Loads <this DLL's folder>\horizon\host.dll once and starts it. Never unloaded: it installs hooks.
void startHost() {
    if (InterlockedExchange(&g_hostStarted, 1) != 0) return;
    const bool shift = held(VK_SHIFT);
    if (shift && held(VK_CONTROL)) {
        debug(L"Ctrl+Shift held: host not loaded, DirectInput only", 0);
        return;
    }
    DWORD n = GetModuleFileNameW(g_self, g_selfPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t path[MAX_PATH];
    lstrcpyW(path, g_selfPath);
    wchar_t* slash = nullptr;
    for (wchar_t* p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') slash = p;
    if (!slash || (slash + 1 - path) + 18 >= MAX_PATH) return;
    lstrcpyW(slash + 1, L"horizon\\host.dll");
    HMODULE host = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!host) {
        debug(L"horizon\\host.dll not loaded, DirectInput only", GetLastError());
        return;
    }
    using InitFn = int(__cdecl*)(const LoaderInfo*);
    InitFn init = reinterpret_cast<InitFn>(GetProcAddress(host, "HorizonHostInit"));
    if (!init) {
        debug(L"horizon\\host.dll has no HorizonHostInit", GetLastError());
        return;
    }
    LoaderInfo info = {kLoaderApi, sizeof(LoaderInfo), shift ? kFlagSafeMode : 0u, g_selfPath};
    init(&info);
}
} // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, void* outer) {
    // The chained DLL first (an earlier loader starts its own mod in this call, on the game's original
    // code), then the host, which hooks after it and chains onto its jumps.
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
    Fn fn = reinterpret_cast<Fn>(proc(0));
    const HRESULT hr = fn ? fn(inst, version, riid, out, outer) : E_FAIL;
    startHost();
    return hr;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    using Fn = HRESULT(WINAPI*)();
    Fn fn = reinterpret_cast<Fn>(proc(1));
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    Fn fn = reinterpret_cast<Fn>(proc(2));
    return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
    using Fn = HRESULT(WINAPI*)();
    Fn fn = reinterpret_cast<Fn>(proc(3));
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
    using Fn = HRESULT(WINAPI*)();
    Fn fn = reinterpret_cast<Fn>(proc(4));
    return fn ? fn() : E_FAIL;
}

LPCVOID WINAPI Proxy_GetdfDIJoystick() {
    using Fn = LPCVOID(WINAPI*)();
    Fn fn = reinterpret_cast<Fn>(proc(5));
    return fn ? fn() : nullptr;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
