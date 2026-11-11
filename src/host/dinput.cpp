// Keeps the game's DirectInput keyboard and mouse reads empty while the Horizon menu is open
// (legacy games only; never on Forever, where Competition Patch owns the input).
//
// How: our own DirectInput8 object (system dinput8.dll by full path) makes a keyboard and a mouse
// device; their method tables (shared with the game's devices of the same interface) get our
// GetDeviceState (slot 9) and GetDeviceData (slot 10). The game's call goes through, then the result
// is emptied while blocked. Joysticks/wheels are passed through untouched (an emptied axis would
// steer). Method table slots only; dinput8's code is not changed.
#define DIRECTINPUT_VERSION 0x0800
#include "host/host.h"

#include <dinput.h>

#include <cstring>

namespace horizon {
namespace {

using GetStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using CapsFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, LPDIDEVCAPS);
using Create8Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

constexpr int kGetCaps = 3, kGetState = 9, kGetData = 10;

struct Table {
    void** table = nullptr;
    GetStateFn state = nullptr;
    GetDataFn data = nullptr;
    CapsFn caps = nullptr;
};
Table g_tables[4];
LONG g_tableCount = 0;
volatile LONG g_blocked = 0;
volatile LONG g_holdDik = -1;

struct Kind {
    void* dev;
    int kind; // 1 keyboard, 2 mouse, 3 other
};
Kind g_kinds[32];
LONG g_kindCount = 0;

Table* tableOf(IDirectInputDevice8W* dev) {
    void** t = *reinterpret_cast<void***>(dev);
    for (LONG i = 0; i < g_tableCount; i++)
        if (g_tables[i].table == t) return &g_tables[i];
    return &g_tables[0];
}

int kindOf(IDirectInputDevice8W* dev, Table* t) {
    for (LONG i = 0; i < g_kindCount; i++)
        if (g_kinds[i].dev == dev) return g_kinds[i].kind;
    DIDEVCAPS caps = {};
    caps.dwSize = sizeof(caps);
    int kind = 3;
    if (t->caps && SUCCEEDED(t->caps(dev, &caps))) {
        const DWORD type = GET_DIDEVICE_TYPE(caps.dwDevType);
        if (type == DI8DEVTYPE_KEYBOARD) kind = 1;
        else if (type == DI8DEVTYPE_MOUSE) kind = 2;
    }
    if (g_kindCount < 32) g_kinds[g_kindCount++] = Kind{dev, kind};
    return kind;
}

HRESULT STDMETHODCALLTYPE getStateDetour(IDirectInputDevice8W* dev, DWORD size, LPVOID data) {
    Table* t = tableOf(dev);
    const HRESULT hr = t->state(dev, size, data);
    if (FAILED(hr) || !data) return hr;
    const int kind = kindOf(dev, t);
    if (kind == 3) return hr;
    if (g_blocked) {
        memset(data, 0, size);
    } else if (kind == 1 && g_holdDik >= 0 && size == 256) {
        static_cast<BYTE*>(data)[g_holdDik] = 0;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE getDataDetour(IDirectInputDevice8W* dev, DWORD objSize, LPDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags) {
    Table* t = tableOf(dev);
    const HRESULT hr = t->data(dev, objSize, data, inOut, flags);
    if (FAILED(hr) || !inOut) return hr;
    const int kind = kindOf(dev, t);
    if (kind == 3) return hr;
    if (g_blocked) {
        *inOut = 0; // read and dropped: nothing piles up for after the menu
    } else if (kind == 1 && g_holdDik >= 0 && data && objSize >= sizeof(DIDEVICEOBJECTDATA)) {
        DWORD kept = 0;
        for (DWORD i = 0; i < *inOut; i++) {
            auto* e = reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<BYTE*>(data) + i * objSize);
            if (static_cast<LONG>(e->dwOfs) == g_holdDik) continue;
            if (kept != i) memcpy(reinterpret_cast<BYTE*>(data) + kept * objSize, e, objSize);
            kept++;
        }
        *inOut = kept;
    }
    return hr;
}

void hookTable(IDirectInputDevice8W* dev, const char* what) {
    void** table = *reinterpret_cast<void***>(dev);
    for (LONG i = 0; i < g_tableCount; i++)
        if (g_tables[i].table == table) return;
    if (g_tableCount >= 4) return;
    Table& t = g_tables[g_tableCount];
    t.table = table;
    t.state = reinterpret_cast<GetStateFn>(table[kGetState]);
    t.data = reinterpret_cast<GetDataFn>(table[kGetData]);
    t.caps = reinterpret_cast<CapsFn>(table[kGetCaps]);
    g_tableCount++;
    writePointer(&table[kGetState], reinterpret_cast<void*>(&getStateDetour));
    writePointer(&table[kGetData], reinterpret_cast<void*>(&getDataDetour));
    char where[160];
    describeAddress(table, where, sizeof(where));
    HLOG("input: DirectInput %s device table at %s hooked (GetDeviceState, GetDeviceData)", what, where);
}

DWORD WINAPI releaseWatch(LPVOID) {
    for (;;) {
        Sleep(20);
        const LONG dik = g_holdDik;
        if (dik < 0) continue;
        // scan code -> virtual key, extended keys (dik >= 0x80) with the E0 prefix
        const UINT sc = dik >= 0x80 ? (0xE000u | static_cast<UINT>(dik & 0x7f)) : static_cast<UINT>(dik);
        const UINT vk = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);
        if (!vk || !(GetAsyncKeyState(static_cast<int>(vk)) & 0x8000)) InterlockedExchange(&g_holdDik, -1);
    }
}

} // namespace

void dinputInstall() {
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16) return;
    wcscat_s(path, L"\\dinput8.dll");
    HMODULE sys = LoadLibraryW(path);
    auto create = sys ? reinterpret_cast<Create8Fn>(GetProcAddress(sys, "DirectInput8Create")) : nullptr;
    if (!create) {
        HWARN("input: system dinput8.dll not usable: the game's input is not blocked while the menu is open");
        return;
    }
    const IID* iids[2] = {&IID_IDirectInput8W, &IID_IDirectInput8A};
    for (int k = 0; k < 2; k++) {
        IDirectInput8W* di = nullptr;
        if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, *iids[k], reinterpret_cast<void**>(&di), nullptr)) || !di) continue;
        const GUID* guids[2] = {&GUID_SysKeyboard, &GUID_SysMouse};
        for (int g = 0; g < 2; g++) {
            IDirectInputDevice8W* dev = nullptr;
            if (SUCCEEDED(di->CreateDevice(*guids[g], &dev, nullptr)) && dev) {
                hookTable(dev, k == 0 ? (g == 0 ? "keyboard (W)" : "mouse (W)") : (g == 0 ? "keyboard (A)" : "mouse (A)"));
                dev->Release();
            }
        }
        di->Release();
    }
    HANDLE th = CreateThread(nullptr, 0, &releaseWatch, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
}

void dinputSetBlocked(bool blocked) {
    const LONG v = blocked ? 1 : 0;
    if (InterlockedExchange(&g_blocked, v) != v) HLOG("input: game keyboard/mouse %s", blocked ? "held back (menu open)" : "back to the game");
}

void dinputHoldUntilReleased(int vk) {
    const UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
    if (!sc) return;
    const LONG dik = static_cast<LONG>((sc & 0xff) | ((sc & 0xff00) ? 0x80 : 0));
    InterlockedExchange(&g_holdDik, dik);
}

} // namespace horizon
