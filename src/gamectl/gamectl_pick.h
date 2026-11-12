// Which challenge info is the map play_map_file just copied into the game's Tracks folder (header-only, unit
// tested in tests/unit_tests.cpp).
//
// The game's RefreshFromDisk does not only append: it can rebuild infos of files that changed, so after one
// copy the list can grow by one and still hold several pointers that were not there before (TM Sunrise:
// "207 -> 208, 2 new"). Counting new pointers is therefore wrong. The copy is found among the new pointers by
// the gold time in the map file's header; with no gold time (map never validated) only a single new pointer
// counts.
#pragma once

#include <cstddef>
#include <functional>
#include <vector>

namespace gamectl {

constexpr int kPickNoNew = -1;     // the rescan brought no new info
constexpr int kPickAmbiguous = -2; // new infos, none matches (or several and no gold time to tell them apart)

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

} // namespace gamectl
