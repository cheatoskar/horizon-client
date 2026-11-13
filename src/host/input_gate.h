// What the game saw of the keyboard and the mouse buttons, so the menu's hold-back never leaves a key down in
// the game: a key the game saw go down gets its key-up even while the menu is open, and when the menu closes,
// a key the game still holds but that is no longer down on the device gets a key-up added to the game's next
// buffered read. Only releases are ever let through or added (no press is made up): online-safe.
//
// Header only, no DirectInput calls: dinput.cpp feeds it the game's GetDeviceData buffers, the unit tests feed
// it fake ones. Keyboard: dwOfs = DIK code 0..255, dwData bit 7 = down. Mouse buttons: dwOfs 12..19
// (DIMOFS_BUTTON0..7), the same bit; axes are never touched.
//
// KeyMsgGate (below) does the same for the window's key messages (WM_KEYDOWN/WM_CHAR...): while Horizon has the
// keyboard (menu, layout mode, a focused text field) no press and no character reaches the game, the release of
// a key the game saw go down always does.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace horizon {

// DIDEVICEOBJECTDATA as dinput.h lays it out (repeated here so the tests need no dinput.h)
struct GateEvent {
    DWORD dwOfs;
    DWORD dwData;
    DWORD dwTimeStamp;
    DWORD dwSequence;
    UINT_PTR uAppData;
};

class InputGate {
public:
    enum Kind { kKeyboard = 0, kMouse = 1 };
    static constexpr DWORD kMouseButton0 = 12, kMouseButtons = 8;

    // Which keys/buttons are down on the device now (true: down). Set by dinput.cpp (GetAsyncKeyState).
    using PhysFn = bool (*)(int kind, DWORD ofs);

    // One buffered read of the game, after the real GetDeviceData: filters *count events in place (stride
    // objSize) and may append releases (up to capacity). blocked: the menu is open.
    void filter(int kind, bool blocked, void* data, DWORD objSize, DWORD* count, DWORD capacity, PhysFn phys) {
        if (!count || kind < 0 || kind > 1) return;
        BYTE* base = static_cast<BYTE*>(data);
        // menu just closed (or a resync asked for): what the game held before this read and the device no
        // longer holds gets a release (unless this read brings an event for that key itself)
        if (!blocked && (wasBlocked_[kind] || resync_[kind])) {
            wasBlocked_[kind] = false;
            resync_[kind] = false;
            for (int s = 0; s < slots(kind); s++)
                if (seen_[kind][s] && !(phys && phys(kind, ofsOf(kind, s)))) pending_[kind][s] = true;
        }
        DWORD kept = 0;
        for (DWORD i = 0; base && objSize >= sizeof(GateEvent) && i < *count; i++) {
            GateEvent* e = reinterpret_cast<GateEvent*>(base + i * objSize);
            int slot = -1;
            const bool button = slotOf(kind, e->dwOfs, &slot);
            const bool down = (e->dwData & 0x80) != 0;
            bool keep;
            if (!blocked) keep = true;
            else keep = button && !down && seen_[kind][slot]; // only the release of a key the game holds
            if (!keep) continue;
            if (button) {
                seen_[kind][slot] = down;
                pending_[kind][slot] = false; // the game hears about this key from the device itself
            }
            if (kept != i) memmove(base + kept * objSize, e, objSize);
            kept++;
        }
        if (!base || objSize < sizeof(GateEvent)) kept = blocked ? 0 : *count;
        *count = kept;
        if (blocked) {
            wasBlocked_[kind] = true;
            return;
        }
        for (int s = 0; s < slots(kind) && base && objSize >= sizeof(GateEvent); s++) {
            if (!pending_[kind][s]) continue;
            if (!seen_[kind][s]) {
                pending_[kind][s] = false; // released by a real event meanwhile
                continue;
            }
            if (*count >= capacity) break; // no room this time: next read
            GateEvent* e = reinterpret_cast<GateEvent*>(base + *count * objSize);
            memset(e, 0, objSize);
            e->dwOfs = ofsOf(kind, s);
            e->dwData = 0;
            e->dwTimeStamp = GetTickCount();
            e->dwSequence = lastSeq(base, objSize, *count);
            (*count)++;
            seen_[kind][s] = false;
            pending_[kind][s] = false;
        }
    }

    // A full-state read (GetDeviceState), after the real call: blocked empties it (no key or button down for the
    // game, which is also the release of whatever it held), then what the game sees is what it holds.
    void filterState(int kind, bool blocked, BYTE* state, DWORD size) {
        if (kind < 0 || kind > 1 || !state) return;
        if (blocked) memset(state, 0, size);
        sawState(kind, blocked, state, size);
    }

    void sawState(int kind, bool blocked, const BYTE* state, DWORD size) {
        if (kind < 0 || kind > 1 || !state) return;
        if (kind == kKeyboard && size >= 256)
            for (int s = 0; s < 256; s++) seen_[kind][s] = !blocked && (state[s] & 0x80) != 0;
        if (kind == kMouse && size >= kMouseButton0 + 4)
            for (DWORD s = 0; s < kMouseButtons && kMouseButton0 + s < size; s++) seen_[kind][s] = !blocked && (state[kMouseButton0 + s] & 0x80) != 0;
    }

    // Check the game's held keys against the device at the next read (focus back, a key held across a reset).
    void resync() { resync_[0] = resync_[1] = true; }

    bool held(int kind, DWORD ofs) const {
        int slot = -1;
        return kind >= 0 && kind <= 1 && slotOf(kind, ofs, &slot) && seen_[kind][slot];
    }

private:
    static int slots(int kind) { return kind == kKeyboard ? 256 : static_cast<int>(kMouseButtons); }
    static DWORD ofsOf(int kind, int slot) { return kind == kKeyboard ? static_cast<DWORD>(slot) : kMouseButton0 + static_cast<DWORD>(slot); }
    static bool slotOf(int kind, DWORD ofs, int* slot) {
        if (kind == kKeyboard && ofs < 256) {
            *slot = static_cast<int>(ofs);
            return true;
        }
        if (kind == kMouse && ofs >= kMouseButton0 && ofs < kMouseButton0 + kMouseButtons) {
            *slot = static_cast<int>(ofs - kMouseButton0);
            return true;
        }
        return false;
    }
    static DWORD lastSeq(const BYTE* base, DWORD objSize, DWORD count) {
        return count ? reinterpret_cast<const GateEvent*>(base + (count - 1) * objSize)->dwSequence : 0;
    }

    bool seen_[2][256] = {};
    bool pending_[2][256] = {};
    bool wasBlocked_[2] = {};
    bool resync_[2] = {};
};

// The window's key messages while Horizon has the keyboard (captured): presses (WM_KEYDOWN, WM_SYSKEYDOWN) and
// characters (WM_CHAR, WM_SYSCHAR, WM_DEADCHAR, WM_SYSDEADCHAR) stay away from the game; a key-up passes when the
// game saw that key go down (else it would stay down in the game), and every key-up passes while not captured.
// pass() answers whether the game's window procedure gets the message. Game thread (window procedure) only.
class KeyMsgGate {
public:
    static bool isKeyMsg(UINT m) {
        return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SYSCHAR ||
               m == WM_DEADCHAR || m == WM_SYSDEADCHAR;
    }

    bool pass(UINT msg, WPARAM vk, bool captured) {
        const unsigned k = static_cast<unsigned>(vk & 0xff);
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            if (captured) return false;
            down_[k] = true;
            return true;
        }
        if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
            const bool held = down_[k];
            down_[k] = false;
            return !captured || held;
        }
        if (isKeyMsg(msg)) return !captured; // characters
        return true;
    }

    bool held(WPARAM vk) const { return down_[vk & 0xff]; }

private:
    bool down_[256] = {};
};

} // namespace horizon
