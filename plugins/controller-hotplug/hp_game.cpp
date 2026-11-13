#define DIRECTINPUT_VERSION 0x0800
#include "hp_game.h"

#include <dinput.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace hp {

namespace {
// CInputPortDx8 (same in ESWC, Sunrise and Original)
constexpr size_t kPortTableCount = 0x14; // binding tables {count, ptr} the port owns
constexpr size_t kPortTableArray = 0x18;
constexpr size_t kPortDevCount = 0x2c;
constexpr size_t kPortDevArray = 0x30;
constexpr size_t kPortDirectInput = 0x94; // IDirectInput8W*
constexpr size_t kPortConfig = 0x98;      // +0x84 != 0: exclusive
constexpr size_t kPortWindow = 0x9c;      // HWND
constexpr size_t kConfigExclusive = 0x84;
// CInputDevice / CInputDeviceDx8Pad
constexpr size_t kDevLost = 0x14;  // 1 = not acquired, the port retries Acquire every update
constexpr size_t kDevIface = 0x34; // IDirectInputDevice8W*
// binding table
constexpr size_t kTableRanges = 0x14;   // {count, ptr, cap} of (start, count) per device index
constexpr size_t kTableBindCount = 0x20;

// ---- ESWC 1.80 (TmNationsESWC.exe 0.1.7.9, sha256 983bd127..., timestamp 5ce7272f)
const uint8_t kEswcEnumCb[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x4c, 0x24, 0x08, 0x50, 0xe8, 0x92, 0xf8, 0xff, 0xff, 0xc2, 0x08, 0x00};
const uint8_t kEswcAddPad[] = {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0x7b, 0xa5, 0xa8, 0x00, 0x50};
const uint8_t kReserve[] = {0x53, 0x8b, 0x5c, 0x24, 0x08, 0x56, 0x8b, 0xf1, 0x8b, 0x4e, 0x08, 0x8b, 0xc3, 0x2b, 0xc1, 0x85, 0xc0};
const uint8_t kEswcStorePort[] = {0x89, 0x15, 0x84, 0x51, 0xc9, 0x00};
const uint8_t kEswcTableResize[] = {0x57, 0x8b, 0xce, 0xe8, 0x36, 0x18, 0x0f, 0x00, 0x3b, 0xdf, 0x89, 0x3e, 0x73, 0x1d, 0x8b, 0x4e};
const uint8_t kJoyFormat[] = {0x18, 0, 0, 0, 0x10, 0, 0, 0, 0x01, 0, 0, 0, 0x10, 0x01, 0, 0, 0xa4, 0, 0, 0};
const CodeSig kEswcCode[] = {
    {0x004647d0, sizeof(kEswcEnumCb), kEswcEnumCb},     // enum callback -> add pad 0x00464070
    {0x00464070, sizeof(kEswcAddPad), kEswcAddPad},     // add pad (CreateDevice ... append)
    {0x00551be0, sizeof(kReserve), kReserve},           // Reserve of 8-byte arrays
    {0x004610d5, sizeof(kEswcStorePort), kEswcStorePort}, // mov [0x00c95184], edx (port global)
    {0x004603a2, sizeof(kEswcTableResize), kEswcTableResize}, // table creation: Reserve(&table->ranges, devices)
    {0x00b53c5c, sizeof(kJoyFormat), kJoyFormat},
};
const PtrSig kEswcPtrs[] = {
    {0x00acfe40 + 0xb0, 0x004647f0}, // port vtable: update (reads all devices)
    {0x00acfe40 + 0x90, 0x00460300}, // port vtable: create binding table
    {0x00acfe40 + 0x94, 0x004604e0}, // port vtable: add binding
    {0x00ad0038 + 0x00, 0x004673e0}, // pad vtable: destructor
    {0x00ad0038 + 0x88, 0x00543500}, // pad vtable: kind (2 = pad)
};
const GameTable kEswc = {"TrackMania Nations ESWC 1.80", 0x5ce7272f, 0x00c95184, 0x00acfe40, 0x00ad0038, 0x004647d0,
                         0x00551be0, 0x00b53c5c, kEswcCode, 6, kEswcPtrs, 5, &eswcBind};

// ---- Sunrise (TmSunrise.exe Win10 community repack, sha256 e59dfc7e..., timestamp 5cc8f0cf)
const uint8_t kTmsEnumCb[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x4c, 0x24, 0x08, 0x50, 0xe8, 0x92, 0xf3, 0xff, 0xff, 0xc2, 0x08, 0x00};
const uint8_t kTmsAddPad[] = {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0x3b, 0x84, 0x9e, 0x00, 0x50};
const uint8_t kTmsStorePort[] = {0x89, 0x15, 0x88, 0xbb, 0xb8, 0x00};
const uint8_t kTmsTableResize[] = {0x57, 0x8b, 0xce, 0xe8, 0xf6, 0x38, 0x17, 0x00, 0x3b, 0xdf, 0x89, 0x3e, 0x73, 0x1d, 0x8b, 0x4e};
const CodeSig kTmsCode[] = {
    {0x004ce7c0, sizeof(kTmsEnumCb), kTmsEnumCb},
    {0x004cdb60, sizeof(kTmsAddPad), kTmsAddPad},
    {0x0060a070, sizeof(kReserve), kReserve},
    {0x0043f175, sizeof(kTmsStorePort), kTmsStorePort},
    {0x00496772, sizeof(kTmsTableResize), kTmsTableResize},
    {0x00a6cc9c, sizeof(kJoyFormat), kJoyFormat},
};
const PtrSig kTmsPtrs[] = {
    {0x00a23d18 + 0xb0, 0x004ce7e0},
    {0x00a23d18 + 0x90, 0x004966d0},
    {0x00a23d18 + 0x94, 0x004968b0},
    {0x00a40af8 + 0x00, 0x0069b200},
    {0x00a40af8 + 0x88, 0x005f9fb0},
};
const GameTable kTms = {"TrackMania Sunrise (Win10 repack)", 0x5cc8f0cf, 0x00b8bb88, 0x00a23d18, 0x00a40af8, 0x004ce7c0,
                        0x0060a070, 0x00a6cc9c, kTmsCode, 6, kTmsPtrs, 5, &sunriseBind};

// ---- Original (TMORIGINAL.EXE Online Patch v2 community exe, sha256 75e129d7..., timestamp 5d337477)
// Every address matched to Sunrise's (masked byte match, the same instruction in the matched function, or the
// vtable that holds the matched functions); bytes compared 2026-11-11, static only (trackmania-internals
// games/tm-original/addresses.md).
const uint8_t kTmoAddPad[] = {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0x8b, 0x49, 0x9e, 0x00, 0x50};
const uint8_t kTmoStorePort[] = {0x89, 0x15, 0xe8, 0x6d, 0xb8, 0x00};
const uint8_t kTmoTableResize[] = {0x57, 0x8b, 0xce, 0xe8, 0xb6, 0xfa, 0x41, 0x00, 0x3b, 0xdf, 0x89, 0x3e, 0x73, 0x1d, 0x8b, 0x4e};
const CodeSig kTmoCode[] = {
    {0x004ce870, sizeof(kTmsEnumCb), kTmsEnumCb}, // same bytes as Sunrise (the call's distance is the same)
    {0x004cdc10, sizeof(kTmoAddPad), kTmoAddPad},
    {0x008b64d0, sizeof(kReserve), kReserve},     // the Reserve that table creation calls (0x496a15)
    {0x0043f355, sizeof(kTmoStorePort), kTmoStorePort},
    {0x00496a12, sizeof(kTmoTableResize), kTmoTableResize},
    {0x00a6895c, sizeof(kJoyFormat), kJoyFormat},
};
const PtrSig kTmoPtrs[] = {
    {0x00a1fcd8 + 0xb0, 0x004ce890},
    {0x00a1fcd8 + 0x90, 0x00496970},
    {0x00a1fcd8 + 0x94, 0x00496b50},
    {0x00a3c7f8 + 0x00, 0x0069a920},
    {0x00a3c7f8 + 0x88, 0x005f98c0},
};
const GameTable kTmo = {"TrackMania Original (Online Patch v2)", 0x5d337477, 0x00b86de8, 0x00a1fcd8, 0x00a3c7f8, 0x004ce870,
                        0x008b64d0, 0x00a6895c, kTmoCode, 6, kTmoPtrs, 5, &originalBind};

using EnumCbFn = BOOL(CALLBACK*)(const DIDEVICEINSTANCEW*, void*);
using ReserveFn = void(__fastcall*)(void* array, void* edx, uint32_t n); // thiscall, ret 4

// ---- SEH helpers (no C++ objects in here)
bool safeRead(const void* p, void* out, size_t n) {
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <typename T> bool rd(uintptr_t addr, T& out) { return safeRead(reinterpret_cast<const void*>(addr), &out, sizeof(T)); }

int callEnum(uintptr_t fn, const DIDEVICEINSTANCEW* inst, void* port) {
    __try {
        reinterpret_cast<EnumCbFn>(fn)(inst, port);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Every binding table of the port gets ranges up to `devices`: new ranges start at the table's binding count
// and are empty (exactly what the game's table code writes when it creates a table for more devices).
int extendRaw(uint8_t* port, uint32_t devices, uintptr_t reserve, int* extended) {
    __try {
        const uint32_t tables = *reinterpret_cast<uint32_t*>(port + kPortTableCount);
        uint8_t** list = *reinterpret_cast<uint8_t***>(port + kPortTableArray);
        if (tables > 256 || (tables && !list)) return 0;
        for (uint32_t t = 0; t < tables; t++) {
            uint8_t* table = list[t];
            if (!table) continue;
            uint32_t* ranges = reinterpret_cast<uint32_t*>(table + kTableRanges); // {count, ptr, cap}
            const uint32_t have = ranges[0];
            if (have >= devices) continue;
            reinterpret_cast<ReserveFn>(reserve)(ranges, nullptr, devices);
            uint32_t* items = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(ranges[1]));
            const uint32_t start = *reinterpret_cast<uint32_t*>(table + kTableBindCount);
            for (uint32_t k = have; k < devices; k++) {
                items[k * 2] = start;
                items[k * 2 + 1] = 0;
            }
            ranges[0] = devices;
            (*extended)++;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int deviceInfo(void* iface, DIDEVICEINSTANCEW* out) {
    __try {
        out->dwSize = sizeof(*out);
        return SUCCEEDED(static_cast<IDirectInputDevice8W*>(iface)->GetDeviceInfo(out)) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Creates and sets up a pad device like the game's callback does. Returns the HRESULT step that failed.
int makeDevice(IDirectInput8W* di, const GUID& g, const DIDATAFORMAT* fmt, HWND wnd, DWORD coop, IDirectInputDevice8W** out) {
    __try {
        IDirectInputDevice8W* d = nullptr;
        if (FAILED(di->CreateDevice(g, &d, nullptr)) || !d) return 1;
        if (FAILED(d->SetDataFormat(fmt))) {
            d->Release();
            return 2;
        }
        if (FAILED(d->SetCooperativeLevel(wnd, coop))) {
            d->Release();
            return 3;
        }
        DIPROPDWORD p = {};
        p.diph.dwSize = sizeof(DIPROPDWORD);
        p.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        p.diph.dwObj = 0;
        p.diph.dwHow = DIPH_DEVICE;
        p.dwData = 0x100;
        if (FAILED(d->SetProperty(DIPROP_BUFFERSIZE, &p.diph))) {
            d->Release();
            return 4;
        }
        *out = d;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 5;
    }
}

int swapIface(uint8_t* dev, IDirectInputDevice8W* fresh) {
    __try {
        IDirectInputDevice8W* old = *reinterpret_cast<IDirectInputDevice8W**>(dev + kDevIface);
        *reinterpret_cast<IDirectInputDevice8W**>(dev + kDevIface) = fresh;
        *reinterpret_cast<uint32_t*>(dev + kDevLost) = 1; // the port acquires it on its next update
        if (old) {
            old->Unacquire();
            old->Release();
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

Guid toGuid(const GUID& g) {
    Guid r;
    memcpy(r.b, &g, 16);
    return r;
}
GUID fromGuid(const Guid& g) {
    GUID r;
    memcpy(&r, g.b, 16);
    return r;
}
} // namespace

const GameTable& eswcTable() { return kEswc; }
const GameTable& sunriseTable() { return kTms; }
const GameTable& originalTable() { return kTmo; }
const GameTable* tableForTimestamp(uint32_t ts) {
    if (ts == kEswc.timestamp) return &kEswc;
    if (ts == kTms.timestamp) return &kTms;
    if (ts == kTmo.timestamp) return &kTmo;
    return nullptr;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

GameBackend::GameBackend(const GameTable* table, uintptr_t exeBase, LogFn log, void* logUser)
    : table_(table), base_(exeBase), log_(log), logUser_(logUser) {
    if (table_ && table_->bind) binder_ = new Binder(&table_->bind(), base_, rb(table_->reserve8), log_, logUser_);
}

GameBackend::~GameBackend() { delete binder_; }

bool GameBackend::poll() {
    uint8_t* p = verified_ ? port() : nullptr;
    return p && binder_ ? binder_->raceCheck(p) : false;
}

void GameBackend::logf(int level, const char* fmt, ...) {
    if (!log_) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    log_(logUser_, level, buf);
}

bool GameBackend::verify() {
    if (verified_) return true;
    if (failed_ || !table_) return false;
    for (int i = 0; i < table_->codeCount; i++) {
        const CodeSig& s = table_->code[i];
        uint8_t buf[64];
        if (s.len > sizeof(buf) || !safeRead(reinterpret_cast<const void*>(rb(s.addr)), buf, s.len) || memcmp(buf, s.bytes, s.len) != 0) {
            failed_ = true;
            logf(2, "game code at %08X is not as expected: controller hot-plug stays off", static_cast<unsigned>(s.addr));
            return false;
        }
    }
    for (int i = 0; i < table_->ptrCount; i++) {
        const PtrSig& s = table_->ptrs[i];
        uintptr_t v = 0;
        if (!rd(rb(s.addr), v) || v != rb(s.value)) {
            failed_ = true;
            logf(2, "game table at %08X is not as expected (%08X): controller hot-plug stays off", static_cast<unsigned>(s.addr),
                 static_cast<unsigned>(v));
            return false;
        }
    }
    verified_ = true;
    logf(0, "%s: game code checked (%d code signatures, %d table entries)", table_->name, table_->codeCount, table_->ptrCount);
    if (binder_) binder_->verify(); // logs its own result; a mismatch only turns the bindings off
    return true;
}

uint8_t* GameBackend::port() {
    uintptr_t p = 0;
    if (!rd(rb(table_->portGlobal), p) || !p) return nullptr;
    uintptr_t vt = 0;
    if (!rd(p, vt) || vt != rb(table_->portVtable)) {
        if (!loggedPortVtable_) logf(1, "input port %08X has another class (%08X): waiting", static_cast<unsigned>(p), static_cast<unsigned>(vt));
        loggedPortVtable_ = true;
        return nullptr;
    }
    return reinterpret_cast<uint8_t*>(p);
}

bool GameBackend::ready() { return verify() && port() != nullptr; }

uint32_t GameBackend::deviceCount() {
    uint8_t* p = verified_ ? port() : nullptr;
    uint32_t n = 0;
    if (p) rd(reinterpret_cast<uintptr_t>(p + kPortDevCount), n);
    return n;
}

bool GameBackend::guidOf(void* dev, void* iface, Guid& out, std::wstring& name) {
    for (const auto& c : cache_)
        if (c.dev == dev && c.iface == iface) {
            out = c.g;
            name = c.name;
            return true;
        }
    DIDEVICEINSTANCEW inst;
    memset(&inst, 0, sizeof(inst));
    if (!iface || !deviceInfo(iface, &inst)) return false;
    out = toGuid(inst.guidInstance);
    inst.tszProductName[MAX_PATH - 1] = 0;
    name = inst.tszProductName;
    for (auto& c : cache_)
        if (c.dev == dev) {
            c.iface = iface;
            c.g = out;
            c.name = name;
            return true;
        }
    cache_.push_back(CacheEntry{dev, iface, out, name});
    return true;
}

void GameBackend::slots(std::vector<Slot>& out) {
    out.clear();
    uint8_t* p = verify() ? port() : nullptr;
    if (!p) return;
    uint32_t n = 0;
    uintptr_t arr = 0;
    if (!rd(reinterpret_cast<uintptr_t>(p + kPortDevCount), n) || !rd(reinterpret_cast<uintptr_t>(p + kPortDevArray), arr) || !arr ||
        n > 256)
        return;
    for (uint32_t i = 0; i < n; i++) {
        uintptr_t dev = 0, vt = 0, iface = 0;
        uint32_t lost = 0;
        if (!rd(arr + i * 4, dev) || !dev || !rd(dev, vt) || vt != rb(table_->padVtable)) continue;
        if (!rd(dev + kDevIface, iface) || !rd(dev + kDevLost, lost)) continue;
        Guid g;
        std::wstring name;
        if (!guidOf(reinterpret_cast<void*>(dev), reinterpret_cast<void*>(iface), g, name)) continue;
        out.push_back(Slot{static_cast<int>(i), g, lost != 0, name});
    }
}

void GameBackend::extendTables(uint8_t* p, uint32_t devices) {
    int extended = 0;
    if (!extendRaw(p, devices, rb(table_->reserve8), &extended))
        logf(2, "binding tables could not be extended to %u devices", devices);
    else if (extended)
        logf(0, "%d binding table(s) got an empty range for the new device", extended);
}

bool GameBackend::add(const Attached& d) {
    uint8_t* p = verify() ? port() : nullptr;
    if (!p) return false;
    uint32_t before = 0;
    rd(reinterpret_cast<uintptr_t>(p + kPortDevCount), before);
    if (before >= kMaxDevices) {
        logf(1, "the game already has %u input devices: %s not added", before, toUtf8(d.name).c_str());
        return false;
    }
    DIDEVICEINSTANCEW inst;
    memset(&inst, 0, sizeof(inst));
    inst.dwSize = sizeof(inst);
    inst.guidInstance = fromGuid(d.instance);
    inst.guidProduct = fromGuid(d.product);
    // what DirectInput reported for it (the game's callback does not look at it today; a pad if unknown)
    inst.dwDevType = d.devType ? d.devType : (DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8) | DIDEVTYPE_HID);
    wcsncpy_s(inst.tszInstanceName, d.name.c_str(), _TRUNCATE);
    wcsncpy_s(inst.tszProductName, d.name.c_str(), _TRUNCATE);
    if (!callEnum(rb(table_->enumCallback), &inst, p)) {
        logf(2, "the game's enumeration callback crashed for %s", toUtf8(d.name).c_str());
        return false;
    }
    uint32_t after = 0;
    rd(reinterpret_cast<uintptr_t>(p + kPortDevCount), after);
    if (after > before) extendTables(p, after); // always, even if something unexpected was added
    if (after != before + 1) {
        logf(1, "the game did not add %s (devices %u -> %u; the game's own log names the DirectInput error)",
             toUtf8(d.name).c_str(), before, after);
        return false;
    }
    // the bindings a pad gets when it is there at start (saved ones from the profile, else the game's defaults)
    if (binder_) binder_->apply(p, after - 1, d.name);
    return true;
}

bool GameBackend::refresh(int slotIndex, const Attached& d) {
    uint8_t* p = verify() ? port() : nullptr;
    if (!p) return false;
    uint32_t n = 0;
    uintptr_t arr = 0, dev = 0, vt = 0, di = 0, cfg = 0;
    HWND wnd = nullptr;
    if (!rd(reinterpret_cast<uintptr_t>(p + kPortDevCount), n) || slotIndex < 0 || static_cast<uint32_t>(slotIndex) >= n) return false;
    if (!rd(reinterpret_cast<uintptr_t>(p + kPortDevArray), arr) || !rd(arr + slotIndex * 4, dev) || !dev || !rd(dev, vt) ||
        vt != rb(table_->padVtable))
        return false;
    if (!rd(reinterpret_cast<uintptr_t>(p + kPortDirectInput), di) || !di) return false;
    rd(reinterpret_cast<uintptr_t>(p + kPortConfig), cfg);
    rd(reinterpret_cast<uintptr_t>(p + kPortWindow), wnd);
    uint32_t exclusive = 0;
    if (cfg) rd(cfg + kConfigExclusive, exclusive);
    // the game's flags: exclusive per its config; foreground on NT (the game asks GetVersion: always NT today)
    const DWORD coop = (exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND;
    IDirectInputDevice8W* fresh = nullptr;
    const int step = makeDevice(reinterpret_cast<IDirectInput8W*>(di), fromGuid(d.instance),
                                reinterpret_cast<const DIDATAFORMAT*>(rb(table_->joyFormat)), wnd, coop, &fresh);
    if (step != 0) {
        logf(1, "replug: fresh device for %s failed at step %d", toUtf8(d.name).c_str(), step);
        return false;
    }
    if (!swapIface(reinterpret_cast<uint8_t*>(dev), fresh)) return false;
    for (auto it = cache_.begin(); it != cache_.end(); ++it)
        if (it->dev == reinterpret_cast<void*>(dev)) {
            cache_.erase(it);
            break;
        }
    return true;
}

} // namespace hp
