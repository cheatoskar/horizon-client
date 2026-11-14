// Keep the saved input bindings on TM Sunrise and TM Original (host setting KeepBindings, default on). The finding and
// the change are described in keepbindings_sites.h. One call in the exe's profile load is redirected, before the
// game loads the profiles: the host starts in the game's first DirectInput8Create (the input engine's init), which
// runs before the app's start loads the profiles. If the profiles are loaded already, nothing is changed (logged).
// Also used by Legacy Essentials (vendored unchanged; src/shim/host/host.h gives it the log).
#include <windows.h>
#include <cstdint>
#include <string.h>

#include "host/host.h"
#include "keepbindings_sites.h"

namespace horizon {
namespace {

constexpr uintptr_t kPreferredBase = 0x00400000;

bool readBytes(uintptr_t at, void* out, size_t n) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(at), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool keepBindingsInstall(uintptr_t exeBase, bool original) {
    const keepbind::Site& s = original ? keepbind::original() : keepbind::sunrise();
    const uintptr_t delta = exeBase - kPreferredBase;
    const uintptr_t at = s.call + delta;
    uint8_t code[15];
    if (!readBytes(at - 10, code, sizeof(code))) {
        HLOG("bindings: %s: code at %08X not readable, nothing changed", s.game, static_cast<unsigned>(s.call));
        return false;
    }
    switch (keepbind::check(s, code)) {
    case keepbind::State::Kept:
        HLOG("bindings: %s: the call at %08X already runs only the defaults pass (another copy of this fix), nothing changed",
             s.game, static_cast<unsigned>(s.call));
        return true;
    case keepbind::State::Other:
        HLOG("bindings: %s: the code at %08X is not as expected, nothing changed (the game still resets the bindings at start)",
             s.game, static_cast<unsigned>(s.call));
        return false;
    case keepbind::State::Original: break;
    }
    // Too late if the app has loaded its profiles already (then the reset has happened for this start)
    uintptr_t app = 0;
    uint32_t profiles = 0;
    if (readBytes(s.appGlobal + delta, &app, sizeof(app)) && app && readBytes(app + s.appProfiles, &profiles, sizeof(profiles)) &&
        profiles > 0) {
        HLOG("bindings: %s: too late, %u profile(s) are loaded already (their bindings were reset for this start), nothing changed",
             s.game, profiles);
        return false;
    }
    uint8_t call[5];
    keepbind::callBytes(s.call, s.defaultsFn, call); // rel32: link addresses, the same after a relocation
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), 5, PAGE_EXECUTE_READWRITE, &old)) {
        HLOG("bindings: %s: code at %08X not writable (%lu), nothing changed", s.game, static_cast<unsigned>(s.call), GetLastError());
        return false;
    }
    memcpy(reinterpret_cast<void*>(at + 1), call + 1, 4);
    VirtualProtect(reinterpret_cast<void*>(at), 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), 5);
    HLOG("bindings: %s resets all bindings at every start (game bug) -> kept (call at %08X now runs only the defaults pass)", s.game,
         static_cast<unsigned>(s.call));
    return true;
}

} // namespace horizon
