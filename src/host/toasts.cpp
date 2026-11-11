// Toasts (ABI minor 3): plugins and the TM Horizon bridge push them from any thread, the overlay steps
// and draws them once a frame. Settings: "toasts.dnd", "toasts.hold" in horizon\settings\host.json.
#include "host/toasts.h"

#include <mutex>

#include "host/host.h"
#include "host/toast_queue.h"
#include "host/ui/ui.h"

namespace horizon {
namespace {

std::mutex g_lock;
ToastQueue g_queue;
uint64_t g_lastTick = 0;

} // namespace

void toastsPush(const std::string& owner, const HorizonToast& t) {
    if (!t.title || !*t.title) return;
    ToastItem it;
    it.owner = owner;
    it.title = t.title;
    it.text = t.text ? t.text : "";
    it.kind = t.kind <= HORIZON_TOAST_ERROR ? t.kind : HORIZON_TOAST_INFO;
    it.icon = t.icon;
    it.durationMs = t.duration_ms;
    it.important = (t.flags & HORIZON_TOAST_IMPORTANT) != 0;
    logLine(HORIZON_LOG_INFO, owner.c_str(), "toast: %s%s%s", it.title.c_str(), it.text.empty() ? "" : " - ", it.text.c_str());
    std::lock_guard<std::mutex> lock(g_lock);
    g_queue.push(std::move(it));
}

bool toastsDnd() { return hostSettingInt("toasts.dnd", 0) != 0; }
bool toastsHold() { return hostSettingInt("toasts.hold", 1) != 0; }
void toastsSetDnd(bool on) {
    hostSetSettingInt("toasts.dnd", on ? 1 : 0);
    HLOG("menu: do not disturb %s", on ? "on" : "off");
}
void toastsSetHold(bool on) {
    hostSetSettingInt("toasts.hold", on ? 1 : 0);
    HLOG("menu: notifications while driving %s", on ? "held back" : "shown");
}

void toastsStep(bool hidden) {
    const uint64_t now = GetTickCount64();
    const double dt = g_lastTick ? static_cast<double>(now - g_lastTick) : 0.0;
    g_lastTick = now;
    ToastConditions c;
    c.hidden = hidden;
    c.dnd = toastsDnd();
    c.holdWhileDriving = toastsHold();
    const int st = game().gameState();
    c.driving = st == HORIZON_STATE_SOLO_RACE || st == HORIZON_STATE_NET_RACE;
    std::lock_guard<std::mutex> lock(g_lock);
    g_queue.step(dt > 250.0 ? 250.0 : dt, c);
}

bool toastsVisible() {
    std::lock_guard<std::mutex> lock(g_lock);
    return !g_queue.visible().empty();
}

void toastsDraw() {
    std::vector<ui::ToastView> views;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        for (const ToastItem& t : g_queue.visible())
            views.push_back(ui::ToastView{t.owner, t.title, t.text, t.kind, t.icon, ToastQueue::alpha(t)});
    }
    ui::drawToasts(views);
}

std::vector<ToastHistoryEntry> toastsHistory() {
    std::lock_guard<std::mutex> lock(g_lock);
    return std::vector<ToastHistoryEntry>(g_queue.history().begin(), g_queue.history().end());
}

} // namespace horizon
