// dinput8.dll - the Controller Hotplug Loader (standalone build of controller-hotplug). A small loader, nothing else.
//
// 1. Every DirectInput export goes to dinput8.chain.dll if that file lies next to this DLL and is not this DLL
//    (an earlier dinput8.dll of another mod, renamed by the install script), else to the system dinput8.dll.
// 2. After the game's first DirectInput8Create returned (main thread, outside the loader lock), it loads
//    controller_hotplug.dll from this DLL's folder (full path) and calls its ControllerHotplugInit.
// Under the Horizon Legacy Loader this DLL is dinput8.chain.dll itself; its own chain lookup then finds itself
// and uses the system DLL. It never reads or changes input data.
#include <windows.h>

namespace {
constexpr unsigned kLoaderApi = 1;

HMODULE g_real = nullptr;
FARPROC g_procs[6] = {};
const char* const kNames[6] = {"DirectInput8Create", "DllCanUnloadNow",     "DllGetClassObject",
                               "DllRegisterServer",  "DllUnregisterServer", "GetdfDIJoystick"};
HMODULE g_self = nullptr;
volatile LONG g_modStarted = 0;

void debug(const wchar_t* msg, DWORD err) {
    wchar_t buf[256];
    wsprintfW(buf, L"Controller Hotplug Loader: %s (error %lu)\n", msg, err);
    OutputDebugStringW(buf);
}

// path of a file next to this DLL; false if it does not fit
bool sibling(const wchar_t* name, wchar_t* path) {
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    wchar_t* slash = nullptr;
    for (wchar_t* p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') slash = p;
    if (!slash || (slash + 1 - path) + lstrlenW(name) + 1 >= MAX_PATH) return false;
    lstrcpyW(slash + 1, name);
    return true;
}

HMODULE loadChain() {
    wchar_t path[MAX_PATH];
    if (!sibling(L"dinput8.chain.dll", path) || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
    HMODULE chain = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!chain) {
        debug(L"dinput8.chain.dll could not be loaded, using the system dinput8.dll", GetLastError());
        return nullptr;
    }
    if (chain == g_self) return nullptr; // we are the chain of the Horizon Legacy Loader
    return chain;
}

bool loadReal() {
    if (g_real) return true;
    HMODULE chain = loadChain();
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
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

void startMod() {
    if (InterlockedExchange(&g_modStarted, 1) != 0) return;
    wchar_t path[MAX_PATH];
    if (!sibling(L"controller_hotplug.dll", path)) return;
    HMODULE mod = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        debug(L"controller_hotplug.dll not loaded, DirectInput only", GetLastError());
        return;
    }
    using InitFn = int(__cdecl*)(unsigned);
    InitFn init = reinterpret_cast<InitFn>(GetProcAddress(mod, "ControllerHotplugInit"));
    if (!init) {
        debug(L"controller_hotplug.dll has no ControllerHotplugInit", GetLastError());
        return;
    }
    init(kLoaderApi);
}
} // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, void* outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
    Fn fn = reinterpret_cast<Fn>(proc(0));
    const HRESULT hr = fn ? fn(inst, version, riid, out, outer) : E_FAIL;
    startMod();
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
