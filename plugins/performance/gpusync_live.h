// Cpu/Gpu synchro in the running game: read it from the engine's viewport and switch it live through the game's own
// CVisionViewportDx9::SyncGpuInit (it releases the old ring's queries and builds the new ring; the game calls it the
// same way from ApplySystemConfig). Only where every fact is checked for the running exe: the viewport's method table
// is the known one, its SyncGpuInit slot holds the known function and that function's bytes store the half flag and
// time-out at the known offsets. Anything else: read-only or nothing. Facts and addresses: gpusync_live.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace perf {
namespace gpusync {

struct Live {
    bool verified = false; // every check passed: reading and switching are safe on this exe
    int value = -1;        // GpuSync value the viewport runs with now (-1 unknown)
    std::string why;       // not verified: why (short)
};

// viewport: CGameApp.Viewport (a CHmsViewport); game: HORIZON_GAME_*. Any thread (guarded reads only).
Live readLive(void* viewport, uint32_t game);
// Game thread, between frames (pre_present). True when the viewport runs with value afterwards.
bool setLive(void* viewport, uint32_t game, int value, std::string* why);
// The engine's CSystemConfigDisplay.GpuSync (what ApplySystemConfig and the game's own config save use).
bool setDisplayValue(void* display, int32_t offset, int value);
int readDisplayValue(void* display, int32_t offset); // -1 unreadable / out of range

// Default.SystemConfig.Gbx next to the exe (ESWC, Sunrise, Original): only GpuSync changes, a copy
// "Default.SystemConfig.Gbx.horizon-backup" is made once before the first change. *old: the value the file had.
// message: for the log/page ("saved", "unknown layout", "could not write").
bool persist(const std::wstring& file, int value, int* old, std::string* message);

} // namespace gpusync
} // namespace perf
