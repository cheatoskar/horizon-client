// Controller hot-plug: the worker thread. It never touches the game.
//
// It owns its own IDirectInput8W (system dinput8.dll by full path) and a message-only window registered for
// HID device notifications. After an arrival or removal it enumerates the attached game controllers twice
// (after 0.5 s and 2.5 s; some drivers show up late), stores the list and kicks the game thread. Without
// notifications it enumerates once per second instead. While the game side asks for ticks (a retry or a
// replug check is due) it kicks once per second.
//
// Test mode (fake): no DirectInput; the list comes from the environment variable HORIZON_TEST_HOTPLUG_DEVICES
// ("1:Pad A;2:Pad B", number = instance id), read every 100 ms.
#pragma once

#include <windows.h>

#include <stdint.h>

#include <string>
#include <vector>

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
    bool takeNotes(std::vector<std::string>& out); // log lines from the worker, for the game thread

    // internal
    void onDeviceChange();

private:
    static DWORD WINAPI threadMain(LPVOID self);
    void run();
    bool enumerate(bool fromNotification);
    bool fakeEnumerate();
    void note(const char* fmt, ...);

    KickFn kick_ = nullptr;
    void* user_ = nullptr;
    bool fake_ = false;
    HANDLE thread_ = nullptr;
    HANDLE stop_ = nullptr;
    volatile LONG ticks_ = 0;
    volatile LONG notify_ = 0;
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<Attached> list_;
    uint32_t gen_ = 0;
    std::vector<std::string> notes_;
    // worker-only state
    void* di_ = nullptr; // IDirectInput8W*
    uint32_t enumAt_[2] = {0, 0};
    bool enumPending_[2] = {false, false};
    std::wstring fakeText_;
    bool fakeFirst_ = true;
};

} // namespace hp
