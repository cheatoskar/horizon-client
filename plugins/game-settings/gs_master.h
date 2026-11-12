// Game settings: master volume. The three games have no master volume of their own, so this is the game's
// Windows audio session (the same slider as the game's entry in the Windows volume mixer; Windows remembers it
// per program). A worker thread owns the COM objects; the game thread only reads and sets plain values.
#pragma once

#include <windows.h>

namespace gs {

class MasterVolume {
public:
    using LogFn = void (*)(void* user, int level, const char* text);

    MasterVolume() = default;
    ~MasterVolume() { stop(); }
    MasterVolume(const MasterVolume&) = delete;
    MasterVolume& operator=(const MasterVolume&) = delete;

    bool start(LogFn log, void* logUser); // idempotent
    void stop();

    // 0..1; -1 while the session is not reachable (no audio device, worker not started yet)
    float get() const;
    // any thread; applied by the worker (the latest value wins)
    void set(float v);

private:
    static unsigned __stdcall threadMain(void* self);
    void run();
    void logf(int level, const char* fmt, ...);

    HANDLE thread_ = nullptr;
    HANDLE wake_ = nullptr;
    volatile LONG stop_ = 0;
    volatile LONG current_ = -1; // value * 10000, -1 unknown
    volatile LONG wanted_ = -1;  // value * 10000, -1 nothing to set
    LogFn log_ = nullptr;
    void* logUser_ = nullptr;
};

} // namespace gs
