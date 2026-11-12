// Controller hot-plug: the game side (TrackMania Nations ESWC 1.80, TrackMania Sunrise and TrackMania Original, same
// input engine).
//
// What the game does (tm-modding-knowledge games/tmn-eswc/input.md):
//   CInputPortDx8::Init creates IDirectInput8W (port+0x94), the mouse and the keyboard, then calls
//   EnumDevices(DI8DEVCLASS_GAMECTRL, callback, port, DIEDFL_ATTACHEDONLY) once. The callback creates the
//   device (c_dfDIJoystick2, buffer 256, the game's cooperative level), wraps it in a CInputDeviceDx8Pad and
//   appends it to the port's device list (port+0x2c count, +0x30 array). Nothing enumerates again later.
//   Bindings are stored per device id (hash of the instance GUID) and turned into binding tables at race
//   start; a table has one (start, count) range per device index (table+0x14 {count, ptr, cap}).
// Hot-plug: call that same callback for a new controller, then give every existing binding table an empty
// range for the new index (what the game's own table code does when the device count differs), so no lookup
// reads past the ranges. Replug: the game keeps the slot (+0x14 = lost) and retries Acquire every frame; if
// that cannot work any more, the slot gets a fresh device for the same GUID (+0x34), set up like the game does.
#pragma once

#include <windows.h>

#include <stdint.h>

#include <string>
#include <vector>

#include "hp_bind.h"
#include "hp_tracker.h"

namespace hp {

struct CodeSig { // bytes at a link address
    uintptr_t addr;
    uint32_t len;
    const uint8_t* bytes;
};
struct PtrSig { // a dword at a link address holds a link address (vtable slot, global store)
    uintptr_t addr;
    uintptr_t value;
};

struct GameTable {
    const char* name;
    uint32_t timestamp;     // PE TimeDateStamp
    uintptr_t portGlobal;   // CInputPortDx8* (set right after the port's Init returned)
    uintptr_t portVtable;
    uintptr_t padVtable;    // CInputDeviceDx8Pad
    uintptr_t enumCallback; // BOOL CALLBACK (const DIDEVICEINSTANCEW*, void* port): the game's EnumDevices callback
    uintptr_t reserve8;     // thiscall void Reserve(uint n) of a {count, ptr, cap} array of 8-byte items
    uintptr_t joyFormat;    // the game's DIDATAFORMAT for pads (c_dfDIJoystick2)
    const CodeSig* code;
    int codeCount;
    const PtrSig* ptrs;
    int ptrCount;
    const BindTable& (*bind)(); // bindings for a late pad (hp_bind.h); null = none
};

const GameTable& eswcTable();
const GameTable& sunriseTable();
const GameTable& originalTable();
const GameTable* tableForTimestamp(uint32_t timestamp);

using LogFn = void (*)(void* user, int level, const char* text); // level 0 info, 1 warn, 2 error

class GameBackend : public Game {
public:
    static constexpr uint32_t kMaxDevices = 32; // the game keeps the device index in one byte; 32 is plenty

    GameBackend(const GameTable* table, uintptr_t exeBase, LogFn log, void* logUser);
    ~GameBackend();
    GameBackend(const GameBackend&) = delete;
    GameBackend& operator=(const GameBackend&) = delete;
    bool verify();   // checks every code signature once; false = wrong exe or patched code: stay off
    bool ready() override;
    void slots(std::vector<Slot>& out) override;
    bool add(const Attached& d) override;
    bool refresh(int slotIndex, const Attached& d) override;

    bool failed() const { return failed_; }
    const Binder* binder() const { return binder_; }
    const GameTable* table() const { return table_; }
    uint32_t deviceCount(); // all devices of the port (keyboard, mouse, pads), 0 without a port
    uint32_t devices() override { return deviceCount(); }

private:
    uintptr_t rb(uintptr_t link) const { return base_ - 0x400000 + link; }
    uint8_t* port();
    void extendTables(uint8_t* port, uint32_t devices);
    bool guidOf(void* dev, void* iface, Guid& out, std::wstring& name);
    void logf(int level, const char* fmt, ...);

    const GameTable* table_;
    uintptr_t base_;
    LogFn log_;
    void* logUser_;
    bool verified_ = false;
    bool failed_ = false;
    bool loggedPortVtable_ = false;
    struct CacheEntry {
        void* dev;
        void* iface;
        Guid g;
        std::wstring name;
    };
    std::vector<CacheEntry> cache_;
    Binder* binder_ = nullptr;
};

// For tests and the module's test mode: a game in memory.
class FakeGame : public Game {
public:
    bool readyFlag = true;
    bool failAdds = false;
    std::vector<Slot> pads;
    int adds = 0, refreshes = 0;
    uint32_t lastAddType = 0;
    bool ready() override { return readyFlag; }
    uint32_t devices() override { return static_cast<uint32_t>(pads.size()) + 2; }
    void slots(std::vector<Slot>& out) override { out = pads; }
    bool add(const Attached& d) override {
        adds++;
        lastAddType = d.devType;
        if (failAdds) return false;
        pads.push_back(Slot{static_cast<int>(pads.size()) + 2, d.instance, true, d.name}); // after mouse and keyboard
        return true;
    }
    bool refresh(int slotIndex, const Attached&) override {
        refreshes++;
        for (auto& p : pads)
            if (p.index == slotIndex) p.lost = false;
        return true;
    }
};

std::string toUtf8(const std::wstring& w);

} // namespace hp
