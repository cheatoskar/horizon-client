// Game settings: the game side. What the game does and why: gs_game.h.
#include "gs_game.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace gs {
namespace {

// field offsets (the same in the three games; the reflection entries in each table confirm them)
constexpr uint32_t kEngineArray = 0x28; // engine manager -> engine array
constexpr uint32_t kAudioEngine = 0x40; // engine 0x10 (audio)
constexpr uint32_t kAudioPort = 0x2c;   // audio engine -> CAudioPort
constexpr uint32_t kAppConfig = 0x18;   // application -> CSystemConfig
constexpr uint32_t kCfgDisplay = 0x20;  // CSystemConfig Display
constexpr uint32_t kFullScreen = 0x34;  // CSystemConfigDisplay FullScreen
constexpr uint32_t kPortSound = 0x14;   // CAudioPort SoundVolume
constexpr uint32_t kPortMusic = 0x18;   // CAudioPort MusicVolume

// shared byte runs
// the init's audio step: mov ecx,[eax+28]; mov edx,[ecx+40]; mov ecx,[edx+2c]; mov edx,[esi+18]; mov eax,[ecx];
// push edx; call [eax+8c]  =  engines->audio engine->port->ApplySystemConfig(app->config)
const uint8_t kMovEax[] = {0xa1};
const uint8_t kAudioApply[] = {0x8b, 0x48, 0x28, 0x8b, 0x51, 0x40, 0x8b, 0x4a, 0x2c, 0x8b, 0x56, 0x18, 0x8b, 0x01, 0x52, 0xff, 0x90, 0x8c, 0x00, 0x00, 0x00};
// the window procedure's developer toggle: movzx ecx,word [esp+38]; mov edx,[app]; cmp ecx,[edx+..]
const uint8_t kWndProcApp[] = {0x0f, 0xb7, 0x4c, 0x24, 0x38, 0x8b, 0x15};
const uint8_t kCmpEswc[] = {0x3b, 0x4a, 0x74};
const uint8_t kCmpTms[] = {0x3b, 0x4a, 0x7c};
// the init makes the input port (DirectInput8Create runs in there) ...: mov ecx,[ecx+4c]; push edx; call ...
const uint8_t kInputPort[] = {0x8b, 0x49, 0x4c, 0x52, 0xe8};
const uint8_t kAfterInput[] = {0x8b, 0x4e, 0x18}; // mov ecx,[esi+18] (the config, for the input port)
// ... before the viewport reads the config: mov ecx,[esi+vp]; mov edx,[ecx]; push eax; mov eax,[esi+18]; push eax;
// call [edx+init]
const uint8_t kViewportEswc[] = {0x8b, 0x4e, 0x3c, 0x8b, 0x11, 0x50, 0x8b, 0x46, 0x18, 0x50, 0xff, 0x92, 0x1c, 0x01, 0x00, 0x00};
const uint8_t kViewportTms[] = {0x8b, 0x4e, 0x44, 0x8b, 0x11, 0x50, 0x8b, 0x46, 0x18, 0x50, 0xff, 0x92, 0x10, 0x01, 0x00, 0x00};
// StartFullscreen / StopFullscreen after their SEH prologue (+0x18): push; push esi; mov esi,ecx;
// mov eax,[esi+260] (fullscreen now); ...
const uint8_t kStartFsEswc[] = {0x55, 0x56, 0x8b, 0xf1, 0x8b, 0x86, 0x60, 0x02, 0x00, 0x00, 0x33, 0xed, 0x3b, 0xc5, 0x74, 0x13};
const uint8_t kStartFs[] = {0x53, 0x56, 0x8b, 0xf1, 0x8b, 0x86, 0x60, 0x02, 0x00, 0x00, 0x33, 0xdb, 0x3b, 0xc3, 0x74, 0x13};
const uint8_t kStopFs[] = {0x53, 0x56, 0x8b, 0xf1, 0x8b, 0x86, 0x60, 0x02, 0x00, 0x00, 0x33, 0xdb, 0x3b, 0xc3,
                           0x0f, 0x84, 0x15, 0x01, 0x00, 0x00, 0x89, 0x9e, 0x60, 0x02, 0x00, 0x00};
constexpr uint32_t kFsNow = 0x260; // viewport: fullscreen now

// ---- ESWC 1.80 (TmNationsESWC.exe, timestamp 5ce7272f)
const CodeSig kEswcAudioCode[] = {
    {0x004252ed, sizeof(kMovEax), kMovEax},
    {0x004252f2, sizeof(kAudioApply), kAudioApply},
    {0x004258bd, sizeof(kWndProcApp), kWndProcApp},
    {0x004258c8, sizeof(kCmpEswc), kCmpEswc},
};
const PtrSig kEswcPtrs[] = {
    {0x004252ee, 0x00c96140}, // engine manager
    {0x004258c4, 0x00c945e0}, // application
};
const ParamSig kEswcAudioParams[] = {
    {0x00c08fac, 0x24, 0x0b005023, 0xec}, // CSystemConfig AudioSoundVolume
    {0x00c08fd8, 0x24, 0x0b005024, 0xf0}, // CSystemConfig AudioMusicVolume
    {0x00c712cc, 0x24, 0x10001005, kPortSound},
    {0x00c712f8, 0x24, 0x10001006, kPortMusic},
};
const CodeSig kEswcDisplayCode[] = {
    {0x004258bd, sizeof(kWndProcApp), kWndProcApp},
    {0x004258c8, sizeof(kCmpEswc), kCmpEswc},
    {0x00425275, sizeof(kInputPort), kInputPort},
    {0x0042527e, sizeof(kAfterInput), kAfterInput},
    {0x004252d3, sizeof(kViewportEswc), kViewportEswc},
};
const ParamSig kEswcDisplayParams[] = {
    {0x00c08a5c, 0x05, 0x0b005001, kCfgDisplay},
    {0x00c0a7c0, 0x01, 0x0b013000, kFullScreen},
};
// StartFullscreen 0x007d3f70 ("[Dx9] StartFullscreen %dx%d in %db (%s)"), StopFullscreen 0x007d41b0, vtable 0x00af5cd8
const CodeSig kEswcToggleCode[] = {
    {0x007d3f88, sizeof(kStartFsEswc), kStartFsEswc},
    {0x007d41c8, sizeof(kStopFs), kStopFs},
};
const PtrSig kEswcTogglePtrs[] = {
    {0x00af5d74, 0x007d3f70}, // vtable +0x9c
    {0x00af5d78, 0x007d41b0}, // vtable +0xa0
};
const GameTable kEswc = {
    "TmNationsESWC 1.80", 0x5ce7272f, 0x00c96140, 0x00c945e0, 0xec, 0xf0,
    {kEswcAudioCode, _countof(kEswcAudioCode), kEswcPtrs, _countof(kEswcPtrs), kEswcAudioParams, _countof(kEswcAudioParams)},
    {kEswcDisplayCode, _countof(kEswcDisplayCode), kEswcPtrs + 1, 1, kEswcDisplayParams, _countof(kEswcDisplayParams)},
    0x3c, 0x00af5cd8,
    {kEswcToggleCode, _countof(kEswcToggleCode), kEswcTogglePtrs, _countof(kEswcTogglePtrs), nullptr, 0},
};

// ---- Sunrise (TmSunrise.exe Win10 community repack, timestamp 5cc8f0cf)
const CodeSig kTmsAudioCode[] = {
    {0x004115b2, sizeof(kMovEax), kMovEax},
    {0x004115b7, sizeof(kAudioApply), kAudioApply},
    {0x0043d49d, sizeof(kWndProcApp), kWndProcApp},
    {0x0043d4a8, sizeof(kCmpTms), kCmpTms},
};
const PtrSig kTmsPtrs[] = {
    {0x004115b3, 0x00b8b494},
    {0x0043d4a4, 0x00b8ab60},
};
const ParamSig kTmsAudioParams[] = {
    {0x00ac135c, 0x24, 0x0b005022, 0xe0},
    {0x00ac1388, 0x24, 0x0b005023, 0xe4},
    {0x00acb724, 0x24, 0x10001005, kPortSound},
    {0x00acb750, 0x24, 0x10001006, kPortMusic},
};
const CodeSig kTmsDisplayCode[] = {
    {0x0043d49d, sizeof(kWndProcApp), kWndProcApp},
    {0x0043d4a8, sizeof(kCmpTms), kCmpTms},
    {0x0041146e, sizeof(kInputPort), kInputPort},
    {0x00411477, sizeof(kAfterInput), kAfterInput},
    {0x00411598, sizeof(kViewportTms), kViewportTms},
};
const ParamSig kTmsDisplayParams[] = {
    {0x00ac0e3c, 0x05, 0x0b005001, kCfgDisplay},
    {0x00ac3de8, 0x01, 0x0b013000, kFullScreen},
};
// StartFullscreen 0x005e4d80, StopFullscreen 0x005e5040, vtable 0x00a31b68
const CodeSig kTmsToggleCode[] = {
    {0x005e4d98, sizeof(kStartFs), kStartFs},
    {0x005e5058, sizeof(kStopFs), kStopFs},
};
const PtrSig kTmsTogglePtrs[] = {
    {0x00a31c04, 0x005e4d80},
    {0x00a31c08, 0x005e5040},
};
const GameTable kTms = {
    "TmSunrise (Win10 repack)", 0x5cc8f0cf, 0x00b8b494, 0x00b8ab60, 0xe0, 0xe4,
    {kTmsAudioCode, _countof(kTmsAudioCode), kTmsPtrs, _countof(kTmsPtrs), kTmsAudioParams, _countof(kTmsAudioParams)},
    {kTmsDisplayCode, _countof(kTmsDisplayCode), kTmsPtrs + 1, 1, kTmsDisplayParams, _countof(kTmsDisplayParams)},
    0x44, 0x00a31b68,
    {kTmsToggleCode, _countof(kTmsToggleCode), kTmsTogglePtrs, _countof(kTmsTogglePtrs), nullptr, 0},
};

// ---- Original (TMORIGINAL.EXE Online Patch v2 community exe, timestamp 5d337477)
const CodeSig kTmoAudioCode[] = {
    {0x004115e2, sizeof(kMovEax), kMovEax},
    {0x004115e7, sizeof(kAudioApply), kAudioApply},
    {0x0043d67d, sizeof(kWndProcApp), kWndProcApp},
    {0x0043d688, sizeof(kCmpTms), kCmpTms},
};
const PtrSig kTmoPtrs[] = {
    {0x004115e3, 0x00b866f4},
    {0x0043d684, 0x00b85dc0},
};
const ParamSig kTmoAudioParams[] = {
    {0x00abd35c, 0x24, 0x0b005022, 0xe0},
    {0x00abd388, 0x24, 0x0b005023, 0xe4},
    {0x00ac7714, 0x24, 0x10001005, kPortSound},
    {0x00ac7740, 0x24, 0x10001006, kPortMusic},
};
const CodeSig kTmoDisplayCode[] = {
    {0x0043d67d, sizeof(kWndProcApp), kWndProcApp},
    {0x0043d688, sizeof(kCmpTms), kCmpTms},
    {0x0041149e, sizeof(kInputPort), kInputPort},
    {0x004114a7, sizeof(kAfterInput), kAfterInput},
    {0x004115c8, sizeof(kViewportTms), kViewportTms},
};
const ParamSig kTmoDisplayParams[] = {
    {0x00abce3c, 0x05, 0x0b005001, kCfgDisplay},
    {0x00abfde8, 0x01, 0x0b013000, kFullScreen},
};
// StartFullscreen 0x005e48e0, StopFullscreen 0x005e4ba0, vtable 0x00a2db28
const CodeSig kTmoToggleCode[] = {
    {0x005e48f8, sizeof(kStartFs), kStartFs},
    {0x005e4bb8, sizeof(kStopFs), kStopFs},
};
const PtrSig kTmoTogglePtrs[] = {
    {0x00a2dbc4, 0x005e48e0},
    {0x00a2dbc8, 0x005e4ba0},
};
const GameTable kTmo = {
    "TMORIGINAL (Online Patch v2)", 0x5d337477, 0x00b866f4, 0x00b85dc0, 0xe0, 0xe4,
    {kTmoAudioCode, _countof(kTmoAudioCode), kTmoPtrs, _countof(kTmoPtrs), kTmoAudioParams, _countof(kTmoAudioParams)},
    {kTmoDisplayCode, _countof(kTmoDisplayCode), kTmoPtrs + 1, 1, kTmoDisplayParams, _countof(kTmoDisplayParams)},
    0x44, 0x00a2db28,
    {kTmoToggleCode, _countof(kTmoToggleCode), kTmoTogglePtrs, _countof(kTmoTogglePtrs), nullptr, 0},
};

bool rd32(uintptr_t addr, uint32_t* out) { return addr && safeRead(reinterpret_cast<const void*>(addr), out, 4); }

bool volumeLooksRight(float v) { return _finite(v) && v >= 0.0f && v <= 1.0f; }

} // namespace

const GameTable& eswcTable() { return kEswc; }
const GameTable& sunriseTable() { return kTms; }
const GameTable& originalTable() { return kTmo; }

const GameTable* tableForTimestamp(uint32_t ts) {
    if (ts == kEswc.timestamp) return &kEswc;
    if (ts == kTms.timestamp) return &kTms;
    if (ts == kTmo.timestamp) return &kTmo;
    return nullptr;
}

bool safeRead(const void* p, void* out, size_t n) {
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safeWrite(void* p, const void* in, size_t n) {
    __try {
        memcpy(p, in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int toPercent(float v) {
    if (!_finite(v) || v <= 0.0f) return 0;
    if (v >= 1.0f) return 100;
    return static_cast<int>(floorf(v * 100.0f + 0.5f));
}

float fromPercent(int pct) {
    if (pct <= 0) return 0.0f;
    if (pct >= 100) return 1.0f;
    return static_cast<float>(pct) / 100.0f;
}

Game::Game(const GameTable* table, uintptr_t exeBase, LogFn log, void* logUser)
    : table_(table), base_(exeBase ? exeBase : 0x00400000), log_(log), logUser_(logUser) {}

void Game::logf(int level, const char* fmt, ...) {
    if (!log_) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    log_(logUser_, level, buf);
}

bool Game::verify(const Part& part, const char* what, bool* done, bool* ok) {
    if (*done) return *ok;
    *done = true;
    *ok = false;
    if (!table_) return false;
    for (int i = 0; i < part.codeCount; i++) {
        const CodeSig& s = part.code[i];
        uint8_t buf[64];
        if (s.len > sizeof(buf) || !safeRead(reinterpret_cast<const void*>(rb(s.addr)), buf, s.len) || memcmp(buf, s.bytes, s.len) != 0) {
            logf(2, "%s: game code at %08X is not as expected: stays off", what, static_cast<unsigned>(s.addr));
            return false;
        }
    }
    for (int i = 0; i < part.ptrCount; i++) {
        const PtrSig& s = part.ptrs[i];
        uint32_t v = 0;
        if (!rd32(rb(s.addr), &v) || v != rb(s.value)) {
            logf(2, "%s: game code at %08X uses %08X, not %08X: stays off", what, static_cast<unsigned>(s.addr), v,
                 static_cast<unsigned>(s.value));
            return false;
        }
    }
    for (int i = 0; i < part.paramCount; i++) {
        const ParamSig& s = part.params[i];
        uint32_t w[4] = {};
        if (!safeRead(reinterpret_cast<const void*>(rb(s.addr)), w, sizeof(w)) || w[0] != s.type || w[1] != s.id || w[3] != s.offset) {
            logf(2, "%s: the game's member %08X at %08X is not as expected: stays off", what, s.id, static_cast<unsigned>(s.addr));
            return false;
        }
    }
    *ok = true;
    logf(0, "%s: %s game code checked (%d code, %d pointer, %d member signatures)", what, table_->name, part.codeCount, part.ptrCount,
         part.paramCount);
    return true;
}

bool Game::toggleOk() {
    return displayOk() && verify(table_ ? table_->toggle : Part{}, "window mode switch", &toggleDone_, &toggleOk_);
}

bool Game::audioOk() { return verify(table_ ? table_->audio : Part{}, "music and sound volume", &audioDone_, &audioOk_); }
bool Game::displayOk() { return verify(table_ ? table_->display : Part{}, "window mode", &displayDone_, &displayOk_); }

uint8_t* Game::config() {
    uint32_t app = 0, cfg = 0;
    if (!rd32(rb(table_->app), &app) || !app || !rd32(app + kAppConfig, &cfg) || !cfg) return nullptr;
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(cfg));
}

uint8_t* Game::display() {
    uint8_t* cfg = config();
    uint32_t d = 0;
    if (!cfg || !rd32(reinterpret_cast<uintptr_t>(cfg + kCfgDisplay), &d) || !d) return nullptr;
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(d));
}

uint8_t* Game::port() {
    uint32_t mgr = 0, arr = 0, eng = 0, p = 0;
    if (!rd32(rb(table_->engines), &mgr) || !mgr || !rd32(mgr + kEngineArray, &arr) || !arr || !rd32(arr + kAudioEngine, &eng) || !eng ||
        !rd32(eng + kAudioPort, &p) || !p)
        return nullptr;
    float s = -1.0f, m = -1.0f;
    if (!safeRead(reinterpret_cast<const void*>(static_cast<uintptr_t>(p + kPortSound)), &s, 4) ||
        !safeRead(reinterpret_cast<const void*>(static_cast<uintptr_t>(p + kPortMusic)), &m, 4) || !volumeLooksRight(s) ||
        !volumeLooksRight(m)) {
        if (!loggedPort_) logf(1, "audio port %08X: volumes not readable as 0..1: left alone", p);
        loggedPort_ = true;
        return nullptr;
    }
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(p));
}

bool Game::readVolume(bool music, float* v) {
    if (!audioOk()) return false;
    float x = -1.0f;
    if (uint8_t* p = port()) {
        if (safeRead(p + (music ? kPortMusic : kPortSound), &x, 4) && volumeLooksRight(x)) {
            *v = x;
            return true;
        }
    }
    uint8_t* cfg = config();
    if (cfg && safeRead(cfg + (music ? table_->cfgMusic : table_->cfgSound), &x, 4) && volumeLooksRight(x)) {
        *v = x;
        return true;
    }
    return false;
}

bool Game::setVolume(bool music, float v) {
    if (!audioOk() || !_finite(v)) return false;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    bool any = false;
    uint8_t* cfg = config();
    float old = -1.0f;
    const uint32_t cfgOff = music ? table_->cfgMusic : table_->cfgSound;
    if (cfg && safeRead(cfg + cfgOff, &old, 4) && volumeLooksRight(old)) any = safeWrite(cfg + cfgOff, &v, 4);
    if (uint8_t* p = port()) any = safeWrite(p + (music ? kPortMusic : kPortSound), &v, 4) || any;
    return any;
}

bool Game::readMusic(float* v) { return readVolume(true, v); }
bool Game::readSounds(float* v) { return readVolume(false, v); }
bool Game::setMusic(float v) { return setVolume(true, v); }
bool Game::setSounds(float v) { return setVolume(false, v); }

bool Game::readFullscreen(bool* fs) {
    if (!displayOk()) return false;
    uint8_t* d = display();
    uint32_t x = 2;
    if (!d || !safeRead(d + kFullScreen, &x, 4) || x > 1) return false;
    *fs = x != 0;
    return true;
}

uint8_t* Game::viewport() {
    uint32_t app = 0, vp = 0, vt = 0;
    if (!rd32(rb(table_->app), &app) || !app || !rd32(app + table_->vpOffset, &vp) || !vp || !rd32(vp, &vt)) return nullptr;
    if (vt != rb(table_->vpVtable)) return nullptr; // another class (or not made yet): never call into it
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(vp));
}

bool Game::fullscreenNow(bool* fs) {
    if (!toggleOk()) return false;
    uint8_t* vp = viewport();
    uint32_t x = 2;
    if (!vp || !safeRead(vp + kFsNow, &x, 4) || x > 1) return false;
    *fs = x != 0;
    return true;
}

namespace {
using ViewportFn = int(__fastcall*)(void* self, void* unused); // thiscall without arguments
int callViewport(uintptr_t fn, void* vp, bool* faulted) {
    __try {
        return reinterpret_cast<ViewportFn>(fn)(vp, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return 0;
    }
}
} // namespace

bool Game::switchWindowMode(bool fullscreen) {
    bool now = false;
    if (!fullscreenNow(&now)) return false;
    if (now == fullscreen) return true;
    uint8_t* vp = viewport();
    uint32_t vt = 0, fn = 0;
    // the object's own vtable slot, checked once more right before the call
    if (!vp || !rd32(reinterpret_cast<uintptr_t>(vp), &vt) || !rd32(vt + (fullscreen ? 0x9c : 0xa0), &fn) ||
        fn != rb(fullscreen ? table_->toggle.ptrs[0].value : table_->toggle.ptrs[1].value))
        return false;
    logf(0, "window mode: %s (the game's own %s)", fullscreen ? "fullscreen" : "windowed", fullscreen ? "StartFullscreen" : "StopFullscreen");
    bool faulted = false;
    callViewport(fn, vp, &faulted);
    if (faulted) {
        logf(2, "window mode: the game's switch raised an exception: window mode off for this start");
        toggleOk_ = false;
        return false;
    }
    bool after = !fullscreen;
    fullscreenNow(&after);
    if (after != fullscreen) {
        logf(1, "window mode: the game stayed %s (see the game's own log line \"StartFullscreen ... -> Fail\")", after ? "fullscreen" : "windowed");
        return false;
    }
    setFullscreen(fullscreen); // the next start the same
    return true;
}

bool Game::setFullscreen(bool fs) {
    bool cur = false;
    if (!readFullscreen(&cur)) return false;
    const uint32_t x = fs ? 1u : 0u;
    return safeWrite(display() + kFullScreen, &x, 4);
}

bool Game::applyStart(const Prefs& p) {
    bool ok = true;
    if (p.music >= 0 || p.sounds >= 0) {
        if (!audioOk() || !config()) {
            if (audioOk()) logf(1, "start: no game settings object yet: music and sound volume not applied");
            ok = false;
        } else {
            if (p.music >= 0) {
                float before = -1.0f;
                readMusic(&before);
                const bool w = setMusic(fromPercent(p.music));
                logf(w ? 0 : 1, "start: music %d%% %s (game's own %d%%)", p.music, w ? "applied" : "NOT applied", toPercent(before));
                ok = ok && w;
            }
            if (p.sounds >= 0) {
                float before = -1.0f;
                readSounds(&before);
                const bool w = setSounds(fromPercent(p.sounds));
                logf(w ? 0 : 1, "start: sounds %d%% %s (game's own %d%%)", p.sounds, w ? "applied" : "NOT applied", toPercent(before));
                ok = ok && w;
            }
        }
    }
    if (p.window >= 0) {
        bool before = false;
        if (!readFullscreen(&before)) {
            logf(1, "start: window mode not applied (no checked game settings object)");
            ok = false;
        } else if ((p.window == 1) == before) {
            logf(0, "start: window mode %s (as the game's own)", before ? "fullscreen" : "windowed");
        } else {
            const bool w = setFullscreen(p.window == 1);
            logf(w ? 0 : 1, "start: window mode %s %s (game's own %s)", p.window == 1 ? "fullscreen" : "windowed", w ? "applied" : "NOT applied",
                 before ? "fullscreen" : "windowed");
            ok = ok && w;
        }
    }
    return ok;
}

// ---------------------------------------------------------------- between frames
namespace {
constexpr WPARAM kMarker = 0x47534d57; // 'GSMW'
HHOOK g_hook = nullptr;
DWORD g_thread = 0;
HWND g_wnd = nullptr;
void (*volatile g_fn)(void*) = nullptr;
void* volatile g_user = nullptr;
DWORD g_postedAt = 0;

LRESULT CALLBACK getMessageHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == PM_REMOVE) {
        MSG* m = reinterpret_cast<MSG*>(lp);
        if (m && m->message == WM_NULL && m->wParam == kMarker) {
            void (*fn)(void*) = g_fn;
            void* user = g_user;
            g_fn = nullptr;
            if (fn) fn(user);
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

BOOL CALLBACK pickWindow(HWND h, LPARAM lp) {
    HWND* out = reinterpret_cast<HWND*>(lp);
    if (!*out) *out = h;
    if (IsWindowVisible(h)) {
        *out = h;
        return FALSE;
    }
    return TRUE;
}
} // namespace

bool runBetweenFrames(void (*fn)(void* user), void* user) {
    if (!fn || (g_fn && GetTickCount() - g_postedAt < 2000)) return false; // a lost post expires
    if (!g_hook) {
        g_thread = GetCurrentThreadId();
        g_hook = SetWindowsHookExW(WH_GETMESSAGE, &getMessageHook, nullptr, g_thread);
        if (!g_hook) return false;
    }
    if (!g_wnd || !IsWindow(g_wnd)) {
        HWND w = nullptr;
        EnumThreadWindows(g_thread, &pickWindow, reinterpret_cast<LPARAM>(&w));
        g_wnd = w;
    }
    g_user = user;
    g_fn = fn;
    g_postedAt = GetTickCount();
    BOOL ok = g_wnd ? PostMessageW(g_wnd, WM_NULL, kMarker, 0) : FALSE;
    if (!ok) ok = PostThreadMessageW(g_thread, WM_NULL, kMarker, 0);
    if (!ok) g_fn = nullptr;
    return ok != FALSE;
}

void stopBetweenFrames() {
    g_fn = nullptr;
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
}

} // namespace gs
