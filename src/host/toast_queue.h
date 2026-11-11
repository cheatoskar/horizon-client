// Toasts (ABI minor 3), the logic only: what is waiting, what is on screen, what went to the history.
// No ImGui, no Windows: unit tested in tests/unit_tests.cpp. toasts.cpp draws and locks.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace horizon {

struct ToastItem {
    std::string owner;      // plugin id, "host" or "TM Horizon"
    std::string title, text;
    uint32_t kind = 0;      // HORIZON_TOAST_*
    uint32_t icon = 0;
    uint32_t durationMs = 5000;
    bool important = false;
    // on screen
    double shownMs = 0;     // time it has been visible (paused while hidden)
    uint64_t id = 0;
};

struct ToastHistoryEntry {
    std::string owner, title, text;
    uint32_t kind = 0;
    bool missed = false;    // never shown (do not disturb)
};

// Frame conditions.
struct ToastConditions {
    bool hidden = false;          // a photo/video is being taken: nothing drawn, timers paused
    bool dnd = false;             // do not disturb: only important toasts show, the rest go to the history
    bool driving = false;         // a run is going
    bool holdWhileDriving = false; // keep non-important toasts back while driving
};

class ToastQueue {
public:
    static constexpr size_t kMaxVisible = 3;
    static constexpr size_t kMaxPending = 24;
    static constexpr size_t kMaxHistory = 20;
    static constexpr double kFadeMs = 250.0;

    void push(ToastItem t) {
        if (t.durationMs == 0) t.durationMs = 5000;
        if (t.durationMs > 30000) t.durationMs = 30000;
        if (t.title.size() > 120) t.title.resize(120);
        if (t.text.size() > 240) t.text.resize(240);
        t.id = ++next_;
        if (pending_.size() >= kMaxPending) {
            remember(pending_.front(), true);
            pending_.pop_front();
        }
        pending_.push_back(std::move(t));
    }

    // Advances by dtMs and moves toasts between pending, visible and history.
    void step(double dtMs, const ToastConditions& c) {
        if (c.hidden) return;
        for (auto& v : visible_) v.shownMs += dtMs;
        for (size_t i = 0; i < visible_.size();) {
            if (visible_[i].shownMs >= visible_[i].durationMs + kFadeMs) {
                remember(visible_[i], false);
                visible_.erase(visible_.begin() + static_cast<long>(i));
            } else {
                ++i;
            }
        }
        for (size_t i = 0; i < pending_.size();) {
            ToastItem& t = pending_[i];
            if (c.dnd && !t.important) {
                remember(t, true);
                pending_.erase(pending_.begin() + static_cast<long>(i));
                continue;
            }
            if (c.holdWhileDriving && c.driving && !t.important) {
                ++i;
                continue;
            }
            if (visible_.size() >= kMaxVisible) break;
            visible_.push_back(std::move(t));
            pending_.erase(pending_.begin() + static_cast<long>(i));
        }
    }

    // 0..1: how far in (fade in/out), for drawing.
    static float alpha(const ToastItem& t) {
        const double in = t.shownMs / kFadeMs;
        const double out = (t.durationMs + kFadeMs - t.shownMs) / kFadeMs;
        double a = in < out ? in : out;
        if (a > 1.0) a = 1.0;
        if (a < 0.0) a = 0.0;
        return static_cast<float>(a);
    }

    const std::deque<ToastItem>& visible() const { return visible_; }
    const std::deque<ToastItem>& pending() const { return pending_; }
    const std::deque<ToastHistoryEntry>& history() const { return history_; }
    void dismiss(uint64_t id) {
        for (auto& v : visible_)
            if (v.id == id && v.shownMs < v.durationMs) v.shownMs = v.durationMs;
    }
    bool busy() const { return !visible_.empty() || !pending_.empty(); }

private:
    void remember(const ToastItem& t, bool missed) {
        history_.push_front(ToastHistoryEntry{t.owner, t.title, t.text, t.kind, missed});
        while (history_.size() > kMaxHistory) history_.pop_back();
    }
    std::deque<ToastItem> pending_, visible_;
    std::deque<ToastHistoryEntry> history_;
    uint64_t next_ = 0;
};

} // namespace horizon
