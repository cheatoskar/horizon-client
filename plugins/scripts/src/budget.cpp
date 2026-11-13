#include "budget.h"

#include <windows.h>

namespace hs {

uint64_t Budget::qpcMicros() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart / freq.QuadPart) * 1000000ull +
           static_cast<uint64_t>(now.QuadPart % freq.QuadPart) * 1000000ull / static_cast<uint64_t>(freq.QuadPart);
}

Budget::Budget(Clock clock) : clock_(clock ? clock : &Budget::qpcMicros) {}

void Budget::start(double limitMs) {
    limitMs_ = limitMs;
    calls_ = 0;
    expired_ = false;
    startUs_ = clock_();
}

bool Budget::expired() {
    if (expired_) return true;
    if ((++calls_ & 31u) != 0) return false;
    expired_ = elapsedMs() >= limitMs_;
    return expired_;
}

double Budget::elapsedMs() const { return static_cast<double>(clock_() - startUs_) / 1000.0; }

} // namespace hs
