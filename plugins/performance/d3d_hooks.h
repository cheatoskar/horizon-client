// Counting (and optionally filtering) Direct3D 9 calls through the method tables of the game's device and
// of its resources (vertex/index buffers, textures, surfaces, queries, state blocks). Only d3d9's method
// tables are changed, never code of the game or of d3d9; every slot is put back on detach if it still holds
// our function. Game thread only, except where noted.
#pragma once

#include <stdint.h>

#include "perf_core.h"

struct IDirect3DDevice9;

namespace perf {
namespace d3d {

using LogFn = void (*)(int level, const char* fmt, ...);

// Hooks the device's method table (once per table). Returns false (and logs why) if it could not.
bool attach(IDirect3DDevice9* dev, LogFn log);
// Puts every slot back that still holds our function. Safe to call twice.
void detach();
bool attached();

// count: fill CallCounts. filter: skip state calls that repeat the current value (needs count or not).
void setMode(bool count, bool filter);
// Why filtering is not allowed on this device ("" = allowed): another hook sits in a slot before ours,
// or a wrapper d3d9.dll (TM Vibrant Shaders, a repack proxy) is in use.
const char* filterBlockedWhy();

// Called at pre_present: hands out this frame's counts, starts the next frame. While `overlay` is true
// (between pre_present and the end of Present: Horizon's own menu) calls are not counted.
void endFrame(CallCounts& out);
void setOverlay(bool overlay);
// Every ~60 frames: put our functions back in front if another mod wrote a slot over.
void recheck();
// Around Reset: the device forgets its states, so does the cache.
void onReset();

// Device facts for the log and the menu (valid after attach).
struct DeviceFacts {
    uint32_t behaviorFlags = 0; // D3DCREATE_*
    bool pure = false, mixed = false, software = false, multithreaded = false;
    bool foreignHooks = false;  // a slot did not point into the d3d9.dll that made the device
    bool wrapper = false;       // another d3d9.dll (game folder proxy) is loaded besides the one that made the device
    char d3d9Path[260] = "";
};
const DeviceFacts& facts();
// Measured cost of one call through a detour with counting and filtering off, in microseconds (-1: not
// measured, e.g. a pure device). Times CallCounts::hookCalls = what the hooks cost a frame.
double perCallUs();

} // namespace d3d
} // namespace perf
