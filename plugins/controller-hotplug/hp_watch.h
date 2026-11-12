// Controller fix: the worker thread. It never touches the game.
//
// It owns its own IDirectInput8W (system dinput8.dll by full path) and a message-only window registered for
// HID device notifications. After an arrival or removal it lists the attached game controllers 4 times (0.5, 2,
// 5 and 10 s later: a pad's HID device can arrive before DirectInput lists it, and drivers differ), stores the
// list and kicks the game thread. It also asks XInput once per second which pads are connected (connection
// state only, never input values): a change there starts the same series, in case a notification got lost.
// Without notifications it lists once every 3 s instead. While the game side asks for ticks (a retry or a
// replug check is due) it kicks once per second.
//
// Everything it sees goes to the log (DirectInput list with name, kind, VID/PID, XInput yes/no; XInput slots;
// HID arrivals/removals; why the game cannot see a pad), so a report can be read from the log alone.
//
// Test mode (fake): no DirectInput, no XInput; the list comes from the environment variable
// HORIZON_TEST_HOTPLUG_DEVICES ("1:Pad A;2:Pad B", number = instance id), read every 100 ms.
#pragma once

#include <windows.h>

#include <stdint.h>

#include <string>
#include <vector>

#include "hp_enum.h"
#include "hp_tracker.h"

namespace hp {

class Watcher {
public:
    using KickFn = void (*)(void* user); // any thread: ask for a step on the game thread

    bool start(KickFn kick, void* user, bool fake);
    void stop();
    bool running() const { return thread_ != nullptr; }

    uint32_t list(std::vector<Attached>& out); // copy of the last enumeration, returns its generation (0 = none)
    void wantTicks(bool on) { InterlockedExchange(&ticks_, on ? 1 : 0); }
    bool notifications() const { return notify_ != 0; }
    Hint hint() const { return static_cast<Hint>(hint_); }
    bool takeNotes(std::vector<std::string>& out); // log lines from the worker, for the game thread

    // internal (worker thread, from the window procedure)
    void onDeviceChange(WPARAM what, LPARAM data);

private:
    static DWORD WINAPI threadMain(LPVOID self);
    void run();
    bool enumerate();
    bool fakeEnumerate();
    void pollXInput(bool first);
    void countXbox();
    void updateHint();
    bool xinputOf(const GUID& instance);
    void note(const char* fmt, ...);

    KickFn kick_ = nullptr;
    void* user_ = nullptr;
    bool fake_ = false;
    HANDLE thread_ = nullptr;
    HANDLE stop_ = nullptr;
    volatile LONG ticks_ = 0;
    volatile LONG notify_ = 0;
    volatile LONG hint_ = 0;
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<Attached> list_;
    uint32_t gen_ = 0;
    std::vector<std::string> notes_;
    // worker-only state
    void* di_ = nullptr; // IDirectInput8W*
    Schedule schedule_;
    bool changed_ = false; // a device change came: the next list bumps the generation even if the set is the same
    using CapsFn = DWORD(WINAPI*)(DWORD, DWORD, void*);
    CapsFn xinputCaps_ = nullptr;
    uint32_t xinputMask_ = 0;
    int xboxUsb_ = 0;
    struct PathEntry {
        Guid g;
        bool xinput;
    };
    std::vector<PathEntry> paths_;
    uint32_t logWindow_ = 0;
    int logCount_ = 0;
    std::wstring fakeText_;
    bool fakeFirst_ = true;
};

} // namespace hp
