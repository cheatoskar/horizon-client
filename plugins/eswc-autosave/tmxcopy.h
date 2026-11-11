#pragma once
// TMX copy: turns a network replay with several ghosts into the structure the game itself writes
// for a solo LAN Net-TimeAttack race (only the local player's ghost), so that the header says
// validable="1" and TM-Exchange accepts it. Port of the reference converter strip_ghosts.py.
//
// Pure file work: no game memory, no logging, no exceptions. Every read is bounds-checked.
#include <stddef.h>
#include <stdint.h>

namespace autosave {
namespace tmxcopy {

enum class Status {
    Converted, // output written and verified
    NotNeeded, // replay has a single ghost: nothing to do
    Refused,   // input not convertible (reason says why), nothing written
    Error,     // I/O problem or failed verification, nothing kept
};

struct Report {
    Status status = Status::Error;
    char reason[256] = "";
    uint32_t ghostsBefore = 0;
    int keptIndex = -1;          // index of the kept ghost in the input's ghost list
    int32_t time = 0;            // race time of the kept ghost (ms)
    uint32_t eventsDuration = 0; // chunk 0x2403F00D, unchanged
    uint32_t nodesBefore = 0, nodesAfter = 0;
    int validable = -1;          // recomputed from the OUTPUT with the game's rule
    double ms = 0;               // conversion + write + verification time
    wchar_t outPath[1024] = L"";
};

// "<stem>.Replay.Gbx" for attempt 1, "<stem> (n).Replay.Gbx" after that (the naming the
// autosave uses so that nothing is ever overwritten). false if it does not fit.
bool replayFileName(wchar_t* out, size_t cap, const wchar_t* stem, int attempt);

// Reads srcPath, converts, writes "<src stem> TMX.Replay.Gbx" (or " (2)" ... " (20)") into outDir
// (nullptr = the folder of srcPath), never overwriting. The output is written to a temp file,
// read back and verified, and only then renamed to its final name.
// login: UTF-8 login the kept ghost must carry (nullptr/"" = not checked).
// expectedTime: race time (ms) the kept ghost must have (< 0 = not checked), e.g. the local
// player's finish time that triggered the save.
void makeCopy(const wchar_t* srcPath, const wchar_t* outDir, const char* login, int expectedTime, Report* r);

// Short summary of a replay file for tests and logs: ghost count, ghost times, EventsDuration,
// header validable, game rule result, RaceSettings string. Returns false if it cannot be parsed.
bool describe(const wchar_t* path, char* out, size_t cap);

} // namespace tmxcopy
} // namespace autosave
