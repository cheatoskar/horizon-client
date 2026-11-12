// Controller fix (controller-hotplug): the glue used by the Horizon module and the standalone DLL alike.
// tick() runs on the game's main thread only (module: run_on_game_thread; standalone: a posted message seen by
// a WH_GETMESSAGE hook of the game thread). The worker thread only calls kick().
#pragma once

#include <windows.h>

#include <stdint.h>

#include <string>
#include <vector>

#include "hp_game.h"
#include "hp_tracker.h"
#include "hp_watch.h"

namespace hp {

struct ServiceCallbacks {
    void* user;
    void (*log)(void* user, int level, const char* text); // game thread (also from start/stop)
    void (*toast)(void* user, const char* title);         // game thread, UTF-8
    void (*kick)(void* user);                             // any thread: call tick() on the game thread soon
};

class Service {
public:
    bool start(Game* game, const ServiceCallbacks& cb, bool fakeWatch);
    void stop();
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    void tick(); // game thread
    void view(std::vector<Tracker::View>& out) const { tracker_.view(out); }
    bool watching() const { return watcher_.running(); }
    bool notifications() const { return watcher_.notifications(); }
    Hint hint() const { return watcher_.hint(); }

private:
    static void onKick(void* self);
    void logf(int level, const char* fmt, ...);

    Game* game_ = nullptr;
    ServiceCallbacks cb_ = {};
    Tracker tracker_;
    Watcher watcher_;
    bool enabled_ = true;
    bool waitingLogged_ = false;
    bool gameLogged_ = false;
    uint32_t devices_ = 0;
};

// One owner per process: the module and the standalone DLL claim it; the second one stays passive.
bool claimOwner(HANDLE* out);         // false: someone else in this process owns controller hot-plug
bool ownerClaimedElsewhere();         // the claim exists (module checks this before starting)
void releaseOwner(HANDLE h);

} // namespace hp
