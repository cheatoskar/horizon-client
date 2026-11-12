// Which challenge info is the map play_map_file just copied into the game's Tracks folder (header-only, unit
// tested in tests/unit_tests.cpp).
//
// CGameCtnChallengeInfo::s_ChallengeInfos holds one info per challenge file the game found (RefreshFromDisk only
// builds infos for files of the challenge class 0x24003000; replays have their own info class and list). A rescan
// is not an append: it destroys infos whose file is gone (or that have no file, except Kind 7) and builds them
// again, and the heap hands out freed addresses again. So after one copy the list can grow by one and still hold
// several pointers that were not there before (Sunrise "207 -> 208, 2 new", "209 -> 210, 16 new"), and the copy's
// info can even sit at an address the list had before (Build also reuses an old info with the same map UID that
// has no file). Pointers and counts are therefore no proof.
//
// The proof is the file: info+0x14 is the info's CSystemFidFile, whose FileName is the file's name. The copy has a
// unique name (HZ_<tick>.Challenge.Gbx), so exactly one info must carry it. Only when no info's file name can be
// read at all (unexpected layout) does the old gold-time pick among the new pointers decide.
#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <vector>

namespace gamectl {

constexpr int kPickNoNew = -1;     // the copy is not in the list (no info carries its file name / no new info)
constexpr int kPickAmbiguous = -2; // several infos carry the name, or (fallback) several new infos and no way to tell

enum class PickMethod { None = 0, FileName = 1, GoldTime = 2 };

struct PickReport {
    PickMethod method = PickMethod::None;
    int newCount = 0; // pointers in `after` that were not in `before`
    int named = 0;    // infos whose file name could be read
    int matches = 0;  // infos whose file name is the copy's
};

// before/after: the info pointers before and after the rescan. goldOf(info, &ms) reads an info's gold time
// (false when unreadable). wantGold: the file header's gold time, <= 0 unknown. Returns the index in `after`.
inline int pickNewChallenge(const std::vector<void*>& before, const std::vector<void*>& after, const std::function<bool(void*, int*)>& goldOf,
                            int wantGold, int* newCount = nullptr) {
    std::vector<size_t> fresh;
    for (size_t i = 0; i < after.size(); i++) {
        bool old = false;
        for (void* b : before)
            if (b == after[i]) {
                old = true;
                break;
            }
        if (!old) fresh.push_back(i);
    }
    if (newCount) *newCount = static_cast<int>(fresh.size());
    if (fresh.empty()) return kPickNoNew;
    if (wantGold > 0 && goldOf) {
        for (size_t i : fresh) {
            int g = 0;
            if (goldOf(after[i], &g) && g == wantGold) return static_cast<int>(i);
        }
    }
    if (fresh.size() == 1) return static_cast<int>(fresh[0]);
    return kPickAmbiguous;
}

// True when the game's file name `have` is the copy's name `want` (ASCII, case-insensitive), alone or as the last
// part of a path.
inline bool sameFileName(const char* have, const char* want) {
    if (!have || !want || !*want) return false;
    const size_t hn = strlen(have), wn = strlen(want);
    if (hn < wn) return false;
    const char* tail = have + (hn - wn);
    if (tail != have && tail[-1] != '\\' && tail[-1] != '/') return false;
    for (size_t i = 0; i < wn; i++) {
        char a = tail[i], b = want[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// The info of the copied file: by file name over the whole list (nameOf(info, buf, cap) reads an info's file name,
// false when it has none or it is unreadable); only if no name at all could be read, by gold time among the new
// pointers (pickNewChallenge). Returns the index in `after` or kPickNoNew / kPickAmbiguous.
inline int pickCopiedChallenge(const std::vector<void*>& before, const std::vector<void*>& after,
                               const std::function<bool(void*, char*, size_t)>& nameOf, const char* wantName,
                               const std::function<bool(void*, int*)>& goldOf, int wantGold, PickReport* report = nullptr) {
    PickReport r;
    int found = kPickNoNew;
    if (nameOf && wantName && *wantName) {
        char buf[300];
        for (size_t i = 0; i < after.size(); i++) {
            buf[0] = 0;
            if (!nameOf(after[i], buf, sizeof(buf))) continue;
            buf[sizeof(buf) - 1] = 0;
            r.named++;
            if (sameFileName(buf, wantName)) {
                r.matches++;
                found = static_cast<int>(i);
            }
        }
    }
    int fresh = 0;
    const int byGold = pickNewChallenge(before, after, goldOf, wantGold, &fresh);
    r.newCount = fresh;
    int pick;
    if (r.named > 0) {
        r.method = PickMethod::FileName;
        pick = r.matches == 1 ? found : r.matches > 1 ? kPickAmbiguous : kPickNoNew;
    } else {
        r.method = PickMethod::GoldTime;
        pick = byGold;
    }
    if (report) *report = r;
    return pick;
}

} // namespace gamectl
