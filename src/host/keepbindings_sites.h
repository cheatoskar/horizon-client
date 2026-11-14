#pragma once
// Keep the saved input bindings on TM Sunrise and TM Original (keepbindings.cpp). Header only and without
// dependencies, so the unit tests (here and in Legacy Essentials, which vendors this file) test the same code.
//
// Finding (static, both exes; trackmania-internals): when the game loads the player profiles at start (CTrackMania
// start -> Sunrise 0x8211F0 / Original 0x81F6D0), for each CConstructionPlayerProfile it checks profile+0x128 and,
// while that is 0, sets it to 1 and calls "reset the bindings" (Sunrise 0x821D20 / Original 0x820200): every binding
// of every device is removed (CInputBindingsConfig 0x4CFF80 with id -1), then the defaults pass gives the devices
// that are plugged in the game's defaults. profile+0x128 is never written by the profile's archive, so it is 0 after
// every load: the saved bindings (keyboard and pads) are thrown away at every start, and the next save writes the
// defaults. ESWC has no such check (load, resolve the action names, defaults pass for devices without bindings).
// The fix: that one call goes to the defaults pass (Sunrise 0x821D50 / Original 0x820230, same thiscall(app;
// config), ret 4) instead, which is what ESWC runs: saved bindings stay, devices without bindings get the defaults.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace horizon {
namespace keepbind {

struct Site {
    const char* game;
    uintptr_t call;        // link address of the call (e8 rel32)
    uintptr_t resetFn;     // what it calls in the exe: reset all bindings, then the defaults pass
    uintptr_t defaultsFn;  // the defaults pass alone
    uintptr_t appGlobal;   // CTrackMania* (null before the app exists)
    uint32_t appProfiles;  // app field: {count, ptr} of the loaded profiles (filled by the profile load)
};

// The 10 bytes before the call, the same in both exes: mov eax,[esp+10h] / mov eax,[eax+68h] / push eax / mov ecx,esi
constexpr uint8_t kBefore[10] = {0x8b, 0x44, 0x24, 0x10, 0x8b, 0x40, 0x68, 0x50, 0x8b, 0xce};

inline const Site& sunrise() {
    static const Site s = {"Sunrise", 0x00821335, 0x00821d20, 0x00821d50, 0x00bcce50, 0x178};
    return s;
}
inline const Site& original() {
    static const Site s = {"Original", 0x0081f815, 0x00820200, 0x00820230, 0x00bc8084, 0x178};
    return s;
}

// e8 rel32 at `at` (link address) calling `target`
inline void callBytes(uintptr_t at, uintptr_t target, uint8_t out[5]) {
    const int32_t rel = static_cast<int32_t>(target - (at + 5));
    out[0] = 0xe8;
    memcpy(out + 1, &rel, 4);
}

enum class State { Original, Kept, Other };

// code: the 10 bytes before the call and the 5 of the call, as in memory (an image at its link address)
inline State check(const Site& s, const uint8_t code[15]) {
    if (memcmp(code, kBefore, sizeof(kBefore)) != 0) return State::Other;
    uint8_t want[5];
    callBytes(s.call, s.resetFn, want);
    if (memcmp(code + 10, want, 5) == 0) return State::Original;
    callBytes(s.call, s.defaultsFn, want);
    if (memcmp(code + 10, want, 5) == 0) return State::Kept;
    return State::Other;
}

} // namespace keepbind
} // namespace horizon
