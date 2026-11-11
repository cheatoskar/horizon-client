// Paths by member NAMES ("App.CurrentPlayground.GameTerminals[0]"), history and pins. No memory reads.
//
// A path starts at one of the host's engine pointers (App, Race, Challenge) and names one member per
// step, with an index for list members. The explorer re-walks a path by names on every refresh, so a
// path stays valid when the engine replaces objects (new race, new map).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace nx {

struct Step {
    std::string member;
    int index = -1;     // >= 0: item of a list member
    uintptr_t obj = 0;  // object this step led to the last time it was walked (0 unknown)
};

struct Path {
    std::string root;   // "App", "Race", "Challenge"
    uintptr_t rootObj = 0;
    std::vector<Step> steps;

    bool empty() const { return root.empty(); }
    // "App.CurrentPlayground.GameTerminals[0]"; only the first `depth` steps when depth >= 0.
    std::string text(int depth = -1) const;
    // The same path cut after `depth` steps (0 = root only).
    Path prefix(size_t depth) const;
    Path child(const std::string& member, int index, uintptr_t obj) const;
    uintptr_t object() const { return steps.empty() ? rootObj : steps.back().obj; }
    // Same names and indexes (objects ignored).
    bool sameAs(const Path& o) const;
    // Label of one breadcrumb: the root for -1, else "Member" or "Member[3]".
    std::string crumb(int step) const;
};

// Root names the explorer knows, and their HORIZON_ENGINE_* number (0 = not a root).
uint32_t rootEngineId(const std::string& root);
// Parses "Race.Players[2].Name"; false on bad syntax or an unknown root (objects stay 0).
bool parsePath(const std::string& text, Path* out);

// Back/forward history of opened paths and the pinned paths.
class Navigator {
public:
    void open(const Path& p);       // becomes current; forward entries are dropped; same path = no new entry
    bool back();
    bool forward();
    bool canBack() const { return pos_ > 0; }
    bool canForward() const { return pos_ + 1 < history_.size(); }
    Path* current() { return history_.empty() ? nullptr : &history_[pos_]; }
    const Path* current() const { return history_.empty() ? nullptr : &history_[pos_]; }
    size_t historySize() const { return history_.size(); }

    bool pin(const Path& p);        // false when already pinned
    void unpin(size_t i);
    bool isPinned(const Path& p) const;
    const std::vector<Path>& pins() const { return pins_; }
    // Pins as text, one path per line (settings), and back.
    std::string pinsText() const;
    void setPinsText(const std::string& text);

    static const size_t kMaxHistory = 64;
    static const size_t kMaxPins = 32;

private:
    std::vector<Path> history_;
    size_t pos_ = 0;
    std::vector<Path> pins_;
};

} // namespace nx
