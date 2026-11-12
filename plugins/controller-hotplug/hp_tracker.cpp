#include "hp_tracker.h"

#include <string.h>

namespace hp {

bool sameGuid(const Guid& a, const Guid& b) { return memcmp(a.b, b.b, sizeof(a.b)) == 0; }

namespace {
const Slot* slotOf(const std::vector<Slot>& slots, const Guid& g) {
    for (const auto& s : slots)
        if (sameGuid(s.instance, g)) return &s;
    return nullptr;
}
const Attached* attachedOf(const std::vector<Attached>& list, const Guid& g) {
    for (const auto& a : list)
        if (sameGuid(a.instance, g)) return &a;
    return nullptr;
}
bool due(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }
} // namespace

void Tracker::reset() {
    known_.clear();
    gen_ = 0;
    haveGen_ = false;
}

void Tracker::view(std::vector<View>& out) const {
    out.clear();
    for (const auto& k : known_) out.push_back(View{k.name, k.present, k.inGame});
}

bool Tracker::step(Game& game, const std::vector<Attached>& attached, uint32_t gen, uint32_t nowMs, std::vector<Event>& events) {
    if (gen == 0) return false; // no enumeration yet: the worker kicks again when it has one
    if (!game.ready()) return true;
    std::vector<Slot> slots;
    game.slots(slots);
    bool more = false;

    const bool firstList = !haveGen_;
    const bool newList = firstList || gen != gen_;
    haveGen_ = true;
    gen_ = gen;

    if (newList) {
        bool setChanged = false;
        for (auto& k : known_) {
            const Attached* a = attachedOf(attached, k.g);
            const Slot* s = slotOf(slots, k.g);
            if (k.present && !a) {
                setChanged = true;
                k.present = false;
                k.backPending = false;
                k.retryPending = false;
                if (s) events.push_back(Event{Note::Gone, k.name, s->index});
            } else if (!k.present && a) {
                setChanged = true;
                k.present = true;
                k.attempts = 0; // plugged in again: registration starts over
                k.retryPending = false;
                if (s) {
                    events.push_back(Event{Note::Back, k.name, s->index});
                    k.backPending = true;
                    k.backAt = nowMs;
                }
            }
            if (a) {
                k.name = a->name;
                k.product = a->product;
                k.devType = a->devType;
                k.xinput = a->xinput;
            }
        }
        for (const auto& a : attached) {
            bool have = false;
            for (const auto& k : known_)
                if (sameGuid(k.g, a.instance)) have = true;
            if (have) continue;
            setChanged = true;
            Known k;
            k.g = a.instance;
            k.product = a.product;
            k.name = a.name;
            k.devType = a.devType;
            k.xinput = a.xinput;
            k.present = true;
            known_.push_back(k);
        }
        if (!setChanged && !firstList) {
            // a device notification but the same set: a pad pulled and plugged in again within a moment.
            // Check its lost slot once.
            for (auto& k : known_) {
                const Slot* s = slotOf(slots, k.g);
                if (k.present && s && s->lost && !k.backPending) {
                    k.backPending = true;
                    k.backAt = nowMs;
                }
            }
        }
    }

    // New controllers: through the game's own enumeration callback.
    int adds = 0;
    for (auto& k : known_) {
        const Slot* s = slotOf(slots, k.g);
        k.inGame = s != nullptr;
        if (!k.present || k.inGame) continue;
        if (k.attempts >= kMaxAttempts) continue; // gave up until it is plugged in again
        if (k.retryPending && !due(nowMs, k.retryAt)) {
            more = true;
            continue;
        }
        if (adds >= kMaxAddsPerStep) {
            more = true;
            continue;
        }
        adds++;
        k.attempts++;
        Attached a{k.g, k.product, k.name, k.devType, k.xinput};
        bool ok = game.add(a);
        if (ok) {
            game.slots(slots);
            s = slotOf(slots, k.g);
            ok = s != nullptr;
        }
        if (ok) {
            k.inGame = true;
            k.attempts = 0;
            k.retryPending = false;
            events.push_back(Event{Note::Added, k.name, s->index});
        } else if (k.attempts >= kMaxAttempts) {
            k.retryPending = false;
            events.push_back(Event{Note::AddGaveUp, k.name, -1});
        } else {
            k.retryPending = true;
            k.retryAt = nowMs + kRetryMs;
            events.push_back(Event{Note::AddFailed, k.name, -1});
            more = true;
        }
    }

    // Replugged controllers the game did not get back by itself.
    for (auto& k : known_) {
        if (!k.backPending) continue;
        const Slot* s = slotOf(slots, k.g);
        if (!k.present || !s || !s->lost) { // gone again, or the game reacquired it
            k.backPending = false;
            continue;
        }
        if (!due(nowMs, k.backAt + kRefreshAfterMs)) {
            more = true;
            continue;
        }
        k.backPending = false;
        const int index = s->index;
        Attached a{k.g, k.product, k.name, k.devType, k.xinput};
        const bool ok = game.refresh(index, a);
        events.push_back(Event{ok ? Note::Refreshed : Note::RefreshFailed, k.name, index});
    }
    return more;
}

} // namespace hp
