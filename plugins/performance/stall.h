// Hang log: when the game's main thread stops presenting frames for a while (a long loading bar, the
// replay list refresh), a watcher thread samples where that thread is waiting: it suspends the thread for a
// few microseconds, copies its registers and the top of its stack, resumes it, and only then turns the
// return addresses into module + offset (and names: exports of system DLLs, TmForever.map for TMNF when it
// sits in the game folder). Nothing is hooked or written in the game. Lines go to <data>\hangs.log.
#pragma once

#include <stdint.h>

namespace perf {
namespace stall {

// folder: the plugin's data folder (UTF-16, trailing backslash). gameFolder: for TmForever.map.
void start(const wchar_t* folder, const wchar_t* gameFolder, uint32_t exeTimestamp);
void stop();
void setEnabled(bool on);
void setThresholdMs(double ms);
// From the game thread, once per frame: marks the thread and the time.
void frame(void* window);
// Game state names for the log ("main menu", "loading"...), and whether sampling is allowed now
// (never while a race runs).
void setState(const char* name, bool raceRunning);
// True from the device's Reset until it is done: no stall is started or sampled meanwhile (Alt+Tab back into the
// game resets the device; suspending the game thread inside the driver only makes that slower).
void setBusy(bool busy);
// Tests: the window counts as in front even when another program has the focus.
void setAssumeFront(bool on);
// Writes one line (game thread or watcher) with the seconds since the process started.
void note(const char* fmt, ...);
// Last finished stall, for the menu: seconds, and where the thread spent most samples.
struct Last {
    double seconds = 0;
    char top[200] = "";
    int count = 0; // stalls this start
};
Last last();

// Turns the address into "module+0xoffset (symbol+0x..)". Exposed for the unit test.
void describe(uintptr_t address, char* out, size_t outSize);

} // namespace stall
} // namespace perf
