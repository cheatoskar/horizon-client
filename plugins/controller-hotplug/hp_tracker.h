// Controller hot-plug: the decision logic, free of game and Windows calls (unit-tested with a mock game).
//
// Inputs per step: the controllers Windows has now (from a DirectInput enumeration on a worker thread) and
// the pad slots the game knows. Outputs: calls into the game (register a new pad through the game's own
// enumeration callback, or give a slot a fresh DirectInput device after a replug) and notes for toasts/logs.
//
// Rules:
//  - A controller the game does not know is registered once. A failed attempt waits 30 s; at most 3 attempts
//    while it stays plugged in (unplugging and plugging it in again starts over). No storm.
//  - Unplugged: the game keeps the slot and its bindings (it retries Acquire itself). Only a note.
//  - Plugged in again (same instance GUID): note "back". If the game's slot is still lost 2 s later, the slot
//    gets a fresh DirectInput device for that GUID, once per replug.
//  - At most 4 registrations per step.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace hp {

struct Guid {
    uint8_t b[16];
};
bool sameGuid(const Guid& a, const Guid& b);

struct Attached { // one controller Windows reports as attached (DirectInput game controller)
    Guid instance;
    Guid product;
    std::wstring name;    // product name
    uint32_t devType = 0; // DirectInput dwDevType (0 = unknown)
    bool xinput = false;  // its HID path has IG_: the XInput HID device of an Xbox-class pad
};

struct Slot { // one pad the game knows
    int index;         // position in the game's device list
    Guid instance;     // DirectInput instance GUID of its device
    bool lost;         // the game could not acquire it (unplugged or not polled yet)
    std::wstring name; // product name (log only, may be empty)
};

class Game {
public:
    virtual ~Game() {}
    virtual bool ready() = 0;                                   // the game's input port exists and was checked
    virtual void slots(std::vector<Slot>& out) = 0;             // pads only (no keyboard/mouse)
    virtual bool add(const Attached& d) = 0;                    // true when the game has a slot for it afterwards
    virtual bool refresh(int slotIndex, const Attached& d) = 0; // fresh DirectInput device for that slot
    virtual uint32_t devices() { return 0; }                    // all input devices of the game (log only)
};

enum class Note {
    Added,         // new controller registered with the game
    Back,          // a known controller is plugged in again
    Gone,          // a known controller was unplugged
    AddFailed,     // the game did not take it (logged; retried later)
    AddGaveUp,     // three failed attempts: waits until it is plugged in again
    Refreshed,     // replug: the slot got a fresh device
    RefreshFailed, // replug: could not create the fresh device
};

struct Event {
    Note what;
    std::wstring name;
    int slot; // game slot index, -1 if none
};

class Tracker {
public:
    static constexpr uint32_t kRetryMs = 30000;
    static constexpr int kMaxAttempts = 3;
    static constexpr uint32_t kRefreshAfterMs = 2000;
    static constexpr int kMaxAddsPerStep = 4;

    // gen: generation of the attached list, 0 = no enumeration finished yet. It changes with every enumeration
    // the worker ran after a Windows device notification, and when a poll saw a different set. A new gen with
    // the same set (a pad pulled and plugged in again within a moment) still checks lost slots once.
    // Returns true when it wants another step later even without a new list (game not ready, a retry or a
    // replug check is due).
    bool step(Game& game, const std::vector<Attached>& attached, uint32_t gen, uint32_t nowMs, std::vector<Event>& events);

    // what the menu shows
    struct View {
        std::wstring name;
        bool present;
        bool inGame;
    };
    void view(std::vector<View>& out) const;
    void reset();

private:
    struct Known {
        Guid g;
        Guid product;
        std::wstring name;
        uint32_t devType = 0;
        bool xinput = false;
        bool present = false;
        bool inGame = false;
        uint32_t backAt = 0; // replug time, 0 = no replug check pending
        bool backPending = false;
        int attempts = 0;
        uint32_t retryAt = 0;
        bool retryPending = false;
    };
    std::vector<Known> known_;
    uint32_t gen_ = 0;
    bool haveGen_ = false;
};

} // namespace hp
