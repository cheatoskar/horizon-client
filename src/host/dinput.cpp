// Keeps the game's DirectInput keyboard and mouse reads empty while the Horizon menu is open, and the keyboard
// also while a text field of a free plugin window has it (Enter and Backspace there must not respawn the car).
// Legacy games: keyboard and mouse, installed at start. Forever (host 0.3.2): keyboard only, installed when the
// game's device exists and only if Competition Patch is not loaded (it owns the input then: nothing is hooked).
//
// How: our own DirectInput8 object (system dinput8.dll by full path) makes a keyboard and a mouse
// device; their method tables (shared with the game's devices of the same interface) get our
// GetDeviceState (slot 9) and GetDeviceData (slot 10). The game's call goes through, then the result
// is emptied while blocked. Joysticks/wheels are passed through untouched (an emptied axis would
// steer). Method table slots only; dinput8's code is not changed.
//
// Stuck keys: the key-up of a key the game saw go down always reaches it, also while blocked, and when the
// menu closes the game gets key-ups for what it still holds but the device does not (input_gate.h).
#define DIRECTINPUT_VERSION 0x0800
#include "host/host.h"

#include <dinput.h>

#include <cstring>

#include "host/input_gate.h"

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
volatile LONG g_blockedKeys = 0;  // keyboard reads held back
volatile LONG g_blockedMouse = 0; // mouse reads held back (never in keyboard-only mode)
bool g_keyboardOnly = false;      // Forever: the mouse device is passed through untouched
bool g_installed = false;
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

// The game's view of its keys and mouse buttons (input_gate.h). Game thread only (its input update).
InputGate g_gate;
static_assert(sizeof(GateEvent) == sizeof(DIDEVICEOBJECTDATA), "GateEvent mirrors DIDEVICEOBJECTDATA");

// Down on the device now: keyboard by scan code -> virtual key, mouse buttons 0..4.
bool physDown(int kind, DWORD ofs) {
    if (kind == InputGate::kKeyboard) {
        const UINT sc = ofs >= 0x80 ? (0xE000u | static_cast<UINT>(ofs & 0x7f)) : static_cast<UINT>(ofs);
        const UINT vk = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);
        if (!vk) return false; // unknown key: count it as up, the game gets its release
        return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    }
    static const int kButtons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    const DWORD b = ofs - InputGate::kMouseButton0;
    if (b >= 5) return false;
    int vk = kButtons[b];
    if (GetSystemMetrics(SM_SWAPBUTTON) && b < 2) vk = b == 0 ? VK_RBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Competition Patch loaded, also later than the host (owner's rule): from then on nothing here touches the game's
// input. The first caller that sees it puts the original GetDeviceState/GetDeviceData back into every table whose
// slot still holds our detour (an aligned pointer write; a call already inside a detour finishes as a plain call,
// host.dll is never unloaded). A slot another hook has taken over since stays ours underneath it, and the detour
// is a pure pass-through from then on.
CpWatch g_cp;
bool cpProbe() { return GetModuleHandleW(L"TMCompPatch.dll") != nullptr; } // the name hostcfg.cpp looks for
HRESULT STDMETHODCALLTYPE getStateDetour(IDirectInputDevice8W* dev, DWORD size, LPVOID data);
HRESULT STDMETHODCALLTYPE getDataDetour(IDirectInputDevice8W* dev, DWORD objSize, LPDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags);

void handOver() {
    InterlockedExchange(&g_blockedKeys, 0);
    InterlockedExchange(&g_blockedMouse, 0);
    InterlockedExchange(&g_holdDik, -1);
    int restored = 0, kept = 0;
    for (LONG i = 0; i < g_tableCount; i++) {
        Table& t = g_tables[i];
        if (t.table[kGetState] == reinterpret_cast<void*>(&getStateDetour) && writePointer(&t.table[kGetState], reinterpret_cast<void*>(t.state))) restored++;
        else kept++;
        if (t.table[kGetData] == reinterpret_cast<void*>(&getDataDetour) && writePointer(&t.table[kGetData], reinterpret_cast<void*>(t.data))) restored++;
        else kept++;
    }
    if (g_tableCount == 0) HLOG("input: Competition Patch is loaded: Horizon does nothing with the game's keyboard");
    else if (kept == 0) HLOG("input: Competition Patch is loaded: Horizon leaves the game's input alone, DirectInput slots restored (%d)", restored);
    else HLOG("input: Competition Patch is loaded: Horizon leaves the game's input alone, %d DirectInput slot(s) restored, %d under another hook stay a plain pass-through", restored, kept);
}

bool handsOff() {
    if (!g_cp.present(GetTickCount(), &cpProbe)) return false;
    if (g_cp.takeFirst()) handOver();
    return true;
}

HRESULT STDMETHODCALLTYPE getStateDetour(IDirectInputDevice8W* dev, DWORD size, LPVOID data) {
    Table* t = tableOf(dev);
    if (handsOff()) return t->state(dev, size, data);
    const HRESULT hr = t->state(dev, size, data);
    if (FAILED(hr) || !data) return hr;
    const int kind = kindOf(dev, t);
    if (kind == 3 || (kind == 2 && g_keyboardOnly)) return hr;
    const bool blocked = (kind == 1 ? g_blockedKeys : g_blockedMouse) != 0;
    if (!blocked && kind == 1 && g_holdDik >= 0 && size == 256) static_cast<BYTE*>(data)[g_holdDik] = 0;
    g_gate.filterState(kind == 1 ? InputGate::kKeyboard : InputGate::kMouse, blocked, static_cast<BYTE*>(data), size);
    return hr;
}

HRESULT STDMETHODCALLTYPE getDataDetour(IDirectInputDevice8W* dev, DWORD objSize, LPDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags) {
    Table* t = tableOf(dev);
    if (handsOff()) return t->data(dev, objSize, data, inOut, flags);
    const DWORD capacity = inOut ? *inOut : 0;
    const HRESULT hr = t->data(dev, objSize, data, inOut, flags);
    if (FAILED(hr) || !inOut) return hr;
    const int kind = kindOf(dev, t);
    if (kind == 3 || (kind == 2 && g_keyboardOnly)) return hr;
    const int gateKind = kind == 1 ? InputGate::kKeyboard : InputGate::kMouse;
    const bool blocked = (kind == 1 ? g_blockedKeys : g_blockedMouse) != 0;
    if (!data || (flags & DIGDD_PEEK)) {
        // a flush (no buffer) or a peek: nothing for the game while blocked, no change to what it holds
        if (blocked) *inOut = 0;
        return hr;
    }
    if (blocked) {
        // read and dropped (nothing piles up for after the menu), except the key-up of a key the game holds:
        // without it the key stays down in the game (a car steering on after the menu closed)
        g_gate.filter(gateKind, true, data, objSize, inOut, capacity, &physDown);
        return hr;
    }
    if (kind == 1 && g_holdDik >= 0 && objSize >= sizeof(DIDEVICEOBJECTDATA)) {
        DWORD kept = 0;
        for (DWORD i = 0; i < *inOut; i++) {
            auto* e = reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<BYTE*>(data) + i * objSize);
            if (static_cast<LONG>(e->dwOfs) == g_holdDik) continue;
            if (kept != i) memcpy(reinterpret_cast<BYTE*>(data) + kept * objSize, e, objSize);
            kept++;
        }
        *inOut = kept;
    }
    // what the game now holds; after the menu: key-ups for keys it holds that are up on the device
    g_gate.filter(gateKind, false, data, objSize, inOut, capacity, &physDown);
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

void dinputInstall(bool keyboardOnly) {
    if (g_installed || inputHandsOff()) return;
    g_installed = true;
    g_keyboardOnly = keyboardOnly;
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
    HLOG("input: the game's DirectInput %s is held back while Horizon has it", keyboardOnly ? "keyboard" : "keyboard and mouse");
    HANDLE th = CreateThread(nullptr, 0, &releaseWatch, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
}

bool inputHandsOff() {
    if (state().competitionPatch) g_cp.markPresent(); // found by the module scan at device creation
    return handsOff();
}

void dinputSetBlocked(bool keyboard, bool mouse) {
    if (g_keyboardOnly) mouse = false;
    if (g_cp.seen()) keyboard = mouse = false;
    const LONG k = keyboard ? 1 : 0, m = mouse ? 1 : 0;
    const LONG oldK = InterlockedExchange(&g_blockedKeys, k);
    const LONG oldM = InterlockedExchange(&g_blockedMouse, m);
    if (oldK == k && oldM == m) return;
    if (k && m) HLOG("input: game keyboard and mouse held back");
    else if (k) HLOG("input: game keyboard held back");
    else if (m) HLOG("input: game mouse held back");
    else HLOG("input: game keyboard/mouse back to the game");
}

bool dinputBlocked() { return g_blockedKeys != 0 || g_blockedMouse != 0; }

void dinputHoldUntilReleased(int vk) {
    if (g_cp.seen()) return;
    const UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
    if (!sc) return;
    const LONG dik = static_cast<LONG>((sc & 0xff) | ((sc & 0xff00) ? 0x80 : 0));
    InterlockedExchange(&g_holdDik, dik);
}

} // namespace horizon
