#include "hp_service.h"

#include <stdarg.h>
#include <stdio.h>

namespace hp {

namespace {
void claimName(wchar_t* out, size_t n) { swprintf_s(out, n, L"Local\\HorizonControllerHotplug.%lu", GetCurrentProcessId()); }
} // namespace

bool claimOwner(HANDLE* out) {
    wchar_t name[64];
    claimName(name, 64);
    HANDLE h = CreateMutexW(nullptr, FALSE, name);
    if (!h) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return false;
    }
    *out = h;
    return true;
}

bool ownerClaimedElsewhere() {
    wchar_t name[64];
    claimName(name, 64);
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

void releaseOwner(HANDLE h) {
    if (h) CloseHandle(h);
}

void Service::logf(int level, const char* fmt, ...) {
    if (!cb_.log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    cb_.log(cb_.user, level, buf);
}

void Service::onKick(void* self) {
    auto* s = static_cast<Service*>(self);
    if (s->cb_.kick) s->cb_.kick(s->cb_.user);
}

bool Service::start(Game* game, const ServiceCallbacks& cb, bool fakeWatch) {
    game_ = game;
    cb_ = cb;
    if (!watcher_.start(&Service::onKick, this, fakeWatch)) {
        logf(2, "worker thread could not be started");
        return false;
    }
    return true;
}

void Service::stop() { watcher_.stop(); }

void Service::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    logf(0, on ? "on: controllers plugged in from now on are registered" : "off: controllers plugged in from now on are not registered");
    if (on && cb_.kick) cb_.kick(cb_.user); // pick up what was plugged in meanwhile
}

void Service::tick() {
    std::vector<std::string> notes;
    if (watcher_.takeNotes(notes))
        for (const auto& n : notes) logf(0, "%s", n.c_str());
    if (!enabled_ || !game_) {
        watcher_.wantTicks(false);
        return;
    }
    std::vector<Attached> list;
    const uint32_t gen = watcher_.list(list);
    std::vector<Event> events;
    const bool ready = gen && game_->ready();
    if (ready && !gameLogged_) {
        // what the game found by itself at start (its own DirectInput enumeration)
        gameLogged_ = true;
        std::vector<Slot> pads;
        game_->slots(pads);
        devices_ = game_->devices();
        logf(0, "game: %u input device(s), %u pad(s)%s", devices_, static_cast<unsigned>(pads.size()), pads.empty() ? "" : ":");
        for (const auto& p : pads)
            logf(0, "  device %d: %s%s", p.index, p.name.empty() ? "(no name)" : toUtf8(p.name).c_str(), p.lost ? " (not acquired yet)" : "");
    }
    const bool more = tracker_.step(*game_, list, gen, GetTickCount(), events);
    if (gen && !ready) {
        if (!waitingLogged_) logf(0, "waiting for the game's input port");
        waitingLogged_ = true;
    }
    watcher_.wantTicks(more);
    for (const auto& e : events) {
        const std::string name = toUtf8(e.name);
        switch (e.what) {
        case Note::Added: {
            const uint32_t before = devices_;
            devices_ = game_->devices();
            logf(0, "registered: %s (device %d; game input devices %u -> %u)", name.c_str(), e.slot, before, devices_);
            if (cb_.toast) cb_.toast(cb_.user, ("Controller added: " + name).c_str());
            break;
        }
        case Note::Back:
            logf(0, "plugged in again: %s (device %d, same bindings)", name.c_str(), e.slot);
            if (cb_.toast) cb_.toast(cb_.user, ("Controller back: " + name).c_str());
            break;
        case Note::Gone: logf(0, "unplugged: %s (device %d stays, the game keeps its bindings)", name.c_str(), e.slot); break;
        case Note::AddFailed: logf(1, "not registered yet: %s (next try in 30 s)", name.c_str()); break;
        case Note::AddGaveUp: logf(1, "not registered after 3 tries: %s (plug it in again to retry)", name.c_str()); break;
        case Note::Refreshed: logf(0, "replug: device %d (%s) got a fresh DirectInput device", e.slot, name.c_str()); break;
        case Note::RefreshFailed: logf(1, "replug: device %d (%s) could not be renewed", e.slot, name.c_str()); break;
        }
    }
}

} // namespace hp
