// The Direct3D 9 hook: method tables only, never d3d9's code.
//
// 1. IDirect3D9::CreateDevice (slot 16 of the factory's method table). The table is shared by every
//    factory of one implementation (system d3d9.dll, or a wrapper d3d9.dll in the game folder such as
//    the Sunrise repack's proxy or TM Vibrant Shaders), so patching the table of a factory we make
//    ourselves covers the game's. Reached three ways: our own factory at start, the exe's import of
//    Direct3DCreate9 (static import: Sunrise, Forever), and the exe's GetProcAddress import (ESWC
//    loads d3d9 by hand). Windows' compatibility shims and other mods write the slot again: it is
//    checked every 50 ms until the game's device exists, and ours goes in front of whatever is there
//    (TM Vibrant Shaders 1.5.0 does the same after its CenterSpeed clash).
// 2. On the game's device (HAL with a depth buffer): Present (17) and Reset (16) in its method table.
//    Checked every 500 ms; written over by another mod (CenterSpeed rewrites the device table), ours
//    goes in front again. A hook that chains back into ours (re-entry on the same thread) goes on to
//    the slot's first value, so every hook in the chain runs once.
#include "host/host.h"

#include <d3d9.h>
#include <intrin.h>

#include <cstring>

#include "common/util.h"

namespace horizon {
namespace {

using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(APIENTRY*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using PresentFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using GetProcFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);

constexpr int kCreateDevice = 16, kReset = 16, kPresent = 17;

struct FactoryTable {
    void** table = nullptr;
    CreateDeviceFn next = nullptr;
    CreateDeviceFn first = nullptr;
};
struct DeviceTable {
    void** table = nullptr;
    PresentFn nextPresent = nullptr, firstPresent = nullptr;
    ResetFn nextReset = nullptr, firstReset = nullptr;
};
constexpr int kMaxTables = 4;
FactoryTable g_factories[kMaxTables];
volatile LONG g_factoryCount = 0;
DeviceTable g_devices[kMaxTables];
volatile LONG g_deviceCount = 0;
volatile LONG g_rewrites = 0;
Create9Fn g_importCreate9 = nullptr;  // the exe's import
Create9Fn g_dynCreate9 = nullptr;     // what the exe's GetProcAddress returned
GetProcFn g_getProc = nullptr;
HMODULE g_d3d9 = nullptr;
IDirect3DDevice9* g_game = nullptr;
HWND g_window = nullptr;
thread_local int t_inCreate = 0, t_inPresent = 0, t_inReset = 0;
bool g_testMode = false;

FactoryTable* factoryOf(void** table) {
    for (LONG i = 0; i < g_factoryCount; i++)
        if (g_factories[i].table == table) return &g_factories[i];
    return nullptr;
}
DeviceTable* deviceOf(IDirect3DDevice9* dev) {
    void** table = nullptr;
    if (!readPointer(dev, reinterpret_cast<void**>(&table))) return nullptr;
    for (LONG i = 0; i < g_deviceCount; i++)
        if (g_devices[i].table == table) return &g_devices[i];
    return nullptr;
}

bool sehFrame(IDirect3DDevice9* dev) {
    __try {
        overlayFrame(dev);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HRESULT APIENTRY presentDetour(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
    DeviceTable* t = deviceOf(dev);
    if (!t) t = &g_devices[0];
    if (t_inPresent) return (t->firstPresent ? t->firstPresent : t->nextPresent)(dev, src, dst, wnd, dirty);
    t_inPresent++;
    if (dev == g_game) {
        // A plugin that throws is caught where it is called and switched off alone. An exception that
        // still gets here is the host's: close the menu, hand the input back, keep going. Three times:
        // the Horizon UI stays off until the next start (the game keeps running with its input).
        static int s_faults = 0;
        if (!sehFrame(dev) && s_faults < 3) {
            s_faults++;
            overlayAfterFault(s_faults >= 3);
        }
    }
    const HRESULT hr = t->nextPresent(dev, src, dst, wnd, dirty);
    if (dev == g_game) pluginsPostPresent(dev, hr);
    t_inPresent--;
    return hr;
}

HRESULT APIENTRY resetDetour(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* params) {
    DeviceTable* t = deviceOf(dev);
    if (!t) t = &g_devices[0];
    if (t_inReset) return (t->firstReset ? t->firstReset : t->nextReset)(dev, params);
    if (dev != g_game) return t->nextReset(dev, params);
    t_inReset++;
    overlayBeforeReset();
    pluginsBeforeReset(dev);
    if (params) pluginsAdjustParams(params);
    const HRESULT hr = t->nextReset(dev, params);
    overlayAfterReset(SUCCEEDED(hr));
    pluginsAfterReset(dev, hr);
    t_inReset--;
    static int s_lines = 0;
    if (s_lines++ < 20)
        HLOG("d3d: Reset -> 0x%08lx (%ux%u)", static_cast<unsigned long>(hr), params ? params->BackBufferWidth : 0,
             params ? params->BackBufferHeight : 0);
    return hr;
}

void hookDevice(IDirect3DDevice9* dev) {
    void** table = nullptr;
    if (!readPointer(dev, reinterpret_cast<void**>(&table)) || !table) return;
    DeviceTable* t = nullptr;
    for (LONG i = 0; i < g_deviceCount; i++)
        if (g_devices[i].table == table) t = &g_devices[i];
    char where[160], pres[160];
    describeAddress(table, where, sizeof(where));
    describeAddress(table[kPresent], pres, sizeof(pres));
    if (!t) {
        if (g_deviceCount >= kMaxTables) return;
        t = &g_devices[g_deviceCount];
        t->table = table;
        t->firstPresent = t->nextPresent = reinterpret_cast<PresentFn>(table[kPresent]);
        t->firstReset = t->nextReset = reinterpret_cast<ResetFn>(table[kReset]);
        InterlockedIncrement(&g_deviceCount);
    }
    if (table[kPresent] != reinterpret_cast<void*>(&presentDetour)) {
        t->nextPresent = reinterpret_cast<PresentFn>(table[kPresent]);
        writePointer(&table[kPresent], reinterpret_cast<void*>(&presentDetour));
    }
    if (table[kReset] != reinterpret_cast<void*>(&resetDetour)) {
        t->nextReset = reinterpret_cast<ResetFn>(table[kReset]);
        writePointer(&table[kReset], reinterpret_cast<void*>(&resetDetour));
    }
    HLOG("d3d: device %p hooked: Present and Reset in its method table at %s (Present was %s)", static_cast<void*>(dev), where, pres);
}

HRESULT APIENTRY createDeviceDetour(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                    D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** out) {
    void** table = nullptr;
    readPointer(self, reinterpret_cast<void**>(&table));
    FactoryTable* f = factoryOf(table);
    if (!f) f = &g_factories[0];
    if (t_inCreate) return (f->first ? f->first : f->next)(self, adapter, type, window, flags, params, out);
    char from[160];
    describeAddress(_ReturnAddress(), from, sizeof(from));
    const bool gameDevice = type == D3DDEVTYPE_HAL && params && (params->EnableAutoDepthStencil || g_testMode);
    if (gameDevice) pluginsAdjustParams(params);
    t_inCreate++;
    const HRESULT hr = f->next(self, adapter, type, window, flags, params, out);
    t_inCreate--;
    HLOG("d3d: CreateDevice -> 0x%08lx type=%d %ux%u windowed=%d autoDepth=%d (called from %s)%s", static_cast<unsigned long>(hr),
         static_cast<int>(type), params ? params->BackBufferWidth : 0, params ? params->BackBufferHeight : 0, params ? params->Windowed : 0,
         params ? params->EnableAutoDepthStencil : 0, from, gameDevice ? "" : ": not the game's device, left alone");
    if (gameDevice && SUCCEEDED(hr) && out && *out) {
        const bool first = g_game == nullptr;
        g_game = *out;
        g_window = window ? window : params->hDeviceWindow;
        hookDevice(*out);
        overlayDeviceCreated(*out, g_window, params);
        pluginsDeviceCreated(*out, params);
        if (!first) HLOG("d3d: the game made a new device: hooks moved to it");
    }
    return hr;
}

bool patchFactory(IDirect3D9* factory, const char* why) {
    void** table = nullptr;
    if (!factory || !readPointer(factory, reinterpret_cast<void**>(&table)) || !table) return false;
    void* now = nullptr;
    if (!readPointer(&table[kCreateDevice], &now)) return false;
    if (now == reinterpret_cast<void*>(&createDeviceDetour)) return true;
    char where[160], holder[160];
    describeAddress(table, where, sizeof(where));
    describeAddress(now, holder, sizeof(holder));
    FactoryTable* f = factoryOf(table);
    if (!f) {
        if (g_factoryCount >= kMaxTables) return false;
        f = &g_factories[g_factoryCount];
        f->table = table;
        f->first = f->next = reinterpret_cast<CreateDeviceFn>(now);
        if (!writePointer(&table[kCreateDevice], reinterpret_cast<void*>(&createDeviceDetour))) return false;
        InterlockedIncrement(&g_factoryCount);
        HLOG("d3d: CreateDevice hooked in the factory table at %s (%s); the slot held %s", where, why, holder);
        return true;
    }
    f->next = reinterpret_cast<CreateDeviceFn>(now);
    if (!writePointer(&table[kCreateDevice], reinterpret_cast<void*>(&createDeviceDetour))) return false;
    if (InterlockedIncrement(&g_rewrites) <= 5)
        HLOG("d3d: CreateDevice slot at %s was written over by %s (%s): ours in front again, it runs after ours", where, holder, why);
    return true;
}

IDirect3D9* WINAPI importCreate9Detour(UINT sdk) {
    IDirect3D9* f = g_importCreate9 ? g_importCreate9(sdk) : nullptr;
    if (f) patchFactory(f, "the game's Direct3DCreate9 import");
    return f;
}

IDirect3D9* WINAPI dynCreate9Detour(UINT sdk) {
    IDirect3D9* f = g_dynCreate9 ? g_dynCreate9(sdk) : nullptr;
    if (f) patchFactory(f, "the game's Direct3DCreate9 by GetProcAddress");
    return f;
}

FARPROC WINAPI getProcDetour(HMODULE module, LPCSTR name) {
    FARPROC p = g_getProc(module, name);
    if (p && reinterpret_cast<uintptr_t>(name) > 0xffff && strcmp(name, "Direct3DCreate9") == 0) {
        g_dynCreate9 = reinterpret_cast<Create9Fn>(p);
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            char where[160];
            describeAddress(reinterpret_cast<void*>(p), where, sizeof(where));
            HLOG("d3d: the game looked up Direct3DCreate9 (%s): handed our wrapper", where);
        }
        return reinterpret_cast<FARPROC>(&dynCreate9Detour);
    }
    return p;
}

DWORD WINAPI watch(LPVOID) {
    // until the game's device exists (at most 60 s): the factory slots, every 50 ms
    for (int i = 0; i < 1200 && !g_game; i++) {
        Sleep(50);
        for (LONG t = 0; t < g_factoryCount; t++) {
            void** table = g_factories[t].table;
            void* now = nullptr;
            if (!readPointer(&table[kCreateDevice], &now) || now == reinterpret_cast<void*>(&createDeviceDetour)) continue;
            void* standIn = table;
            patchFactory(reinterpret_cast<IDirect3D9*>(&standIn), "found by the 50 ms check");
        }
    }
    if (!g_game) HWARN("d3d: no game device seen in 60 s after the host started: no menu this start (see the lines above)");
    // then the device's slots, every 500 ms
    for (;;) {
        Sleep(500);
        IDirect3DDevice9* dev = g_game;
        if (!dev) continue;
        void** table = nullptr;
        if (!readPointer(dev, reinterpret_cast<void**>(&table)) || !table) continue;
        void* p = nullptr;
        void* r = nullptr;
        if (!readPointer(&table[kPresent], &p) || !readPointer(&table[kReset], &r)) continue;
        if (p == reinterpret_cast<void*>(&presentDetour) && r == reinterpret_cast<void*>(&resetDetour)) continue;
        char holder[160];
        describeAddress(p != reinterpret_cast<void*>(&presentDetour) ? p : r, holder, sizeof(holder));
        if (InterlockedIncrement(&g_rewrites) <= 10) HLOG("d3d: the device's Present/Reset slot was written over (now %s): ours in front again", holder);
        hookDevice(dev);
    }
}

} // namespace

void* gameDevice() { return g_game; }

void d3dInstall() {
    g_testMode = state().loader == LoaderKind::Test;
    HMODULE exe = GetModuleHandleW(nullptr);
    // 1. the exe's imports
    void* orig = nullptr;
    if (patchImport(exe, "d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&importCreate9Detour), &orig)) {
        g_importCreate9 = reinterpret_cast<Create9Fn>(orig);
        char where[160];
        describeAddress(orig, where, sizeof(where));
        HLOG("d3d: the exe's Direct3DCreate9 import patched (it led to %s)", where);
    } else {
        HLOG("d3d: the exe has no Direct3DCreate9 import (it loads d3d9 itself)");
    }
    orig = nullptr;
    if (patchImport(exe, "kernel32.dll", "GetProcAddress", reinterpret_cast<void*>(&getProcDetour), &orig)) {
        g_getProc = reinterpret_cast<GetProcFn>(orig);
    } else {
        HWARN("d3d: the exe's GetProcAddress import not found");
    }
    // 2. our own factory from the d3d9.dll the game uses (a wrapper in the game folder comes first)
    g_d3d9 = GetModuleHandleW(L"d3d9.dll");
    const bool loadedByUs = g_d3d9 == nullptr;
    if (!g_d3d9) g_d3d9 = LoadLibraryW(L"d3d9.dll");
    if (g_d3d9) {
        const std::wstring path = modulePath(g_d3d9);
        HLOG("d3d: d3d9.dll in use: %s%s", utf8(path).c_str(), loadedByUs ? " (loaded by the host, not yet by the game)" : "");
        auto create = reinterpret_cast<Create9Fn>(GetProcAddress(g_d3d9, "Direct3DCreate9"));
        if (IDirect3D9* f = create ? create(D3D_SDK_VERSION) : nullptr) {
            patchFactory(f, "our own factory");
            f->Release();
        } else {
            HWARN("d3d: could not make a Direct3D 9 factory");
        }
    } else {
        HERR("d3d: d3d9.dll could not be loaded (error %lu)", GetLastError());
    }
    HANDLE th = CreateThread(nullptr, 0, &watch, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
}

} // namespace horizon
