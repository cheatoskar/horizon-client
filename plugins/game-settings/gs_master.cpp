// Game settings: master volume through the process's Windows audio session (gs_master.h).
#include "gs_master.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <process.h>
#include <stdarg.h>
#include <stdio.h>

namespace gs {
namespace {

template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// the default output device's volume control for this process's default session (DirectSound plays there)
ISimpleAudioVolume* openSession(HRESULT* hrOut) {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioSessionManager* mgr = nullptr;
    ISimpleAudioVolume* vol = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en));
    if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr));
    if (SUCCEEDED(hr)) hr = mgr->GetSimpleAudioVolume(nullptr, FALSE, &vol);
    release(mgr);
    release(dev);
    release(en);
    *hrOut = hr;
    return SUCCEEDED(hr) ? vol : nullptr;
}

} // namespace

void MasterVolume::logf(int level, const char* fmt, ...) {
    if (!log_) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    log_(logUser_, level, buf);
}

bool MasterVolume::start(LogFn log, void* logUser) {
    if (thread_) return true;
    log_ = log;
    logUser_ = logUser;
    InterlockedExchange(&stop_, 0);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_) return false;
    const uintptr_t h = _beginthreadex(nullptr, 0, &MasterVolume::threadMain, this, 0, nullptr);
    if (!h) {
        CloseHandle(wake_);
        wake_ = nullptr;
        logf(2, "volume: worker thread not started");
        return false;
    }
    thread_ = reinterpret_cast<HANDLE>(h);
    return true;
}

void MasterVolume::stop() {
    if (!thread_) return;
    InterlockedExchange(&stop_, 1);
    SetEvent(wake_);
    WaitForSingleObject(thread_, 3000);
    CloseHandle(thread_);
    CloseHandle(wake_);
    thread_ = nullptr;
    wake_ = nullptr;
}

float MasterVolume::get() const {
    const DWORD now = GetTickCount();
    const DWORD prev = static_cast<DWORD>(InterlockedExchange(&lastGet_, static_cast<LONG>(now)));
    if (now - prev >= 1500 && wake_) SetEvent(wake_); // the menu opened again: read the mixer's value now
    const LONG c = current_;
    return c < 0 ? -1.0f : static_cast<float>(c) / 10000.0f;
}

void MasterVolume::set(float v) {
    if (!(v >= 0.0f)) v = 0.0f; // also NaN
    if (v > 1.0f) v = 1.0f;
    const LONG x = static_cast<LONG>(v * 10000.0f + 0.5f);
    InterlockedExchange(&wanted_, x);
    InterlockedExchange(&current_, x); // the menu shows the new value at once
    if (wake_) SetEvent(wake_);
}

unsigned __stdcall MasterVolume::threadMain(void* self) {
    static_cast<MasterVolume*>(self)->run();
    return 0;
}

void MasterVolume::run() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ISimpleAudioVolume* vol = nullptr;
    bool loggedFail = false;
    DWORD nextOpen = 0;
    bool first = true;
    while (!stop_) {
        // only while someone looks (the menu shows the row) or after a set: no Windows calls otherwise
        const bool inUse = first || wanted_ >= 0 || GetTickCount() - static_cast<DWORD>(lastGet_) < 1500;
        first = false;
        if (inUse && !vol && (nextOpen == 0 || static_cast<LONG>(GetTickCount() - nextOpen) >= 0)) {
            HRESULT hr = S_OK;
            vol = openSession(&hr);
            if (vol) {
                logf(0, "volume: Windows audio session of the game found");
                loggedFail = false;
            } else {
                if (!loggedFail) logf(1, "volume: no Windows audio session (0x%08lX): retrying every 5 s", static_cast<unsigned long>(hr));
                loggedFail = true;
                InterlockedExchange(&current_, -1);
                nextOpen = GetTickCount() + 5000;
                if (nextOpen == 0) nextOpen = 1;
            }
        }
        if (inUse && vol) {
            const LONG want = InterlockedExchange(&wanted_, -1);
            if (want >= 0 && FAILED(vol->SetMasterVolume(static_cast<float>(want) / 10000.0f, nullptr))) release(vol);
            float now = 0.0f;
            if (vol && SUCCEEDED(vol->GetMasterVolume(&now))) {
                if (wanted_ < 0) InterlockedExchange(&current_, static_cast<LONG>(now * 10000.0f + 0.5f));
            } else {
                release(vol); // the device went away: open the new default one
                InterlockedExchange(&current_, -1);
            }
        }
        // a set wakes it at once; while in use it follows the Windows mixer twice a second
        WaitForSingleObject(wake_, inUse ? 500 : 2000);
    }
    release(vol);
    if (SUCCEEDED(co)) CoUninitialize();
}

} // namespace gs
