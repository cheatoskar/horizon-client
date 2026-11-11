// Toasts (ABI minor 3), see toasts.cpp and toast_queue.h.
#pragma once

#include <string>
#include <vector>

#include "horizon_plugin.h"
#include "host/toast_queue.h"

namespace horizon {

// Any thread. owner: plugin id, "host" or "TM Horizon" (shown small under the text).
void toastsPush(const std::string& owner, const HorizonToast& t);
// Game thread, once a frame: advances timers, moves toasts on screen. hidden: a photo or video is being
// taken (timers paused).
void toastsStep(bool hidden);
bool toastsVisible();
// Inside the ImGui frame.
void toastsDraw();
std::vector<ToastHistoryEntry> toastsHistory();
bool toastsDnd();
bool toastsHold();
void toastsSetDnd(bool on);
void toastsSetHold(bool on);

} // namespace horizon
