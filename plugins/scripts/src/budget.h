// Execution time budget for one script callback. The engine's line callback asks expired() on every
// script line; the clock is read every 32nd call only (QueryPerformanceCounter costs ~20 ns, a script
// line less). Tests pass their own clock.
#pragma once

#include <stdint.h>

namespace hs {

class Budget {
public:
    using Clock = uint64_t (*)(); // microseconds
    explicit Budget(Clock clock = nullptr);

    void start(double limitMs);
    // True once the limit is reached (stays true until the next start).
    bool expired();
    double elapsedMs() const;
    double limitMs() const { return limitMs_; }

    static uint64_t qpcMicros();

private:
    Clock clock_;
    uint64_t startUs_ = 0;
    double limitMs_ = 0;
    uint32_t calls_ = 0;
    bool expired_ = false;
};

} // namespace hs
