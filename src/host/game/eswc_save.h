// save_replay on ESWC: the game's save without the Save-As dialog (see eswc_save.cpp).
#pragma once

#include <string>

namespace horizon {

struct EswcSaveResult {
    int ok;          // 1 saved, 0 failed, -1 skipped (why)
    const char* why;
    wchar_t name[300];   // file name as written
    wchar_t folder[300]; // folder, ends with a backslash
    int time;            // best ghost time (ms) or stunt score, -1 unknown
};

// solo: the current replay (solo end-race screen); else the previous race's copy (menu, LAN/online).
// stem: file name without ".Replay.Gbx", null/"" = the game's default "<map>(mm'ss''cc)". Game thread.
void eswcSaveReplay(void* app, bool solo, const wchar_t* stem, EswcSaveResult* r);

} // namespace horizon
