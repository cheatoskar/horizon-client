// Process-wide options (priority, CPU affinity, Windows power throttling, timer resolution) and memory
// facts. Nothing here touches the game's code or memory; each option can be put back to what it was.
#pragma once

#include <stdint.h>

namespace perf {
namespace sys {

void captureOriginals(); // once at Init: priority class, affinity mask, timer resolution

// 0 = leave as the game set it, 1 = above normal, 2 = high. Returns false if Windows refused.
bool setPriority(int level);
// 0 = leave, 1 = all cores but CPU 0, 2 = performance cores only (hybrid CPUs; same as 0 elsewhere)
bool setAffinity(int mode);
const char* affinityNote(); // what mode 2 found ("8 performance cores of 16" ...)
// true: Windows must not slow the game down in the background (EcoQoS) nor ignore its timer requests
bool setPowerThrottlingOff(bool off);
// true: ask for 0.5 ms timer resolution (the game itself asks for 1 ms); false: give it back
bool setFineTimer(bool on);
double currentTimerMs(); // the system's current timer resolution in ms (0 unknown)

struct Memory {
    bool largeAddressAware = false;
    double privateMB = 0, addressSpaceUsedMB = 0, addressSpaceTotalMB = 0;
};
Memory memory();
double secondsSinceProcessStart();

// Waits until `deadlineMs` (QueryPerformanceCounter clock in ms): a high-resolution waitable timer for most
// of it, the last ~0.7 ms spinning. Returns the ms actually waited.
double waitUntil(double deadlineMs);
double nowMs();

} // namespace sys
} // namespace perf
