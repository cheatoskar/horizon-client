// Replay files (.Replay.Gbx, TrackMania Nations/United Forever; older games' replays use the same
// ghost chunks where they have checkpoint times): map UID, driver, race time and checkpoint times.
// No game code: the file alone. See replayfile.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ck {

struct Replay {
    std::string mapUid;           // the map's UID (its header)
    std::string driver;           // nickname ("" unknown)
    int raceTime = -1;            // ms
    std::vector<int> checkpoints; // race time at each checkpoint (ms), the last one the finish
};

// Only the header (fast: for file lists). False: not a replay, or unreadable.
bool readReplayHeader(const std::wstring& path, Replay& out, std::string& error);
// Header and body: the times. False with the reason.
bool readReplay(const std::wstring& path, Replay& out, std::string& error);
// The same from bytes in memory (a download through the TM Horizon bridge, the tests).
bool parseReplay(const std::vector<unsigned char>& file, bool body, Replay& out, std::string& error);
// LZO1X decompression (the GBX body). False: corrupt or not exactly `size` bytes out.
bool lzoDecompress(const unsigned char* in, size_t inSize, unsigned char* out, size_t size);

} // namespace ck
