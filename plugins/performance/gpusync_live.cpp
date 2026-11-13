// Cpu/Gpu synchro in the running game (gpusync_live.h). Addresses are link-time VAs (image base 0x00400000) of the
// exes in trackmania-internals rendering/gpu-sync.md, rebased to where the exe is loaded (TMLoader may move it).
// Checked statically 2026-11-13 (capstone over the five exes): the method table slot ApplySystemConfig calls on the
// viewport (`push time-out; push half; push count; call [vtable+slot]`) holds SyncGpuInit, which stores half and
// time-out next to the ring, frees the queries of entries beyond the new size, reallocates the ring and wraps both
// indices: calling it again between frames is what the game itself would do on a config change.
#include "gpusync_live.h"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include "gpusync.h"
#include "horizon_plugin.h"

namespace perf {
namespace gpusync {
namespace {

struct Exe {
    uint32_t game;
    uintptr_t vtable;  // CVisionViewportDx9's method table
    uint32_t slot;     // SyncGpuInit's slot (byte offset)
    uintptr_t fn;      // SyncGpuInit
    uint32_t sigOff;   // the signature below sits at fn + sigOff (no absolute address in it: relocation-proof)
    uint32_t ring;     // viewport: ring entry count (CFastBuffer count); half flag and time-out follow at half, half + 4
    uint32_t half;
    uint8_t sig[31];   // mov eax,[esp+a]; mov ecx,[esp+b]; push ebp; mov ebp,[esp+c]; cmp ebp,0x100; mov [this+half],eax; mov [this+half+4],ecx
};

const Exe kExes[] = {
    {HORIZON_GAME_TMNF, 0x00BD10BC, 0x134, 0x0098BAB0, 0x2B, 0x930, 0x944,
     {0x8b, 0x44, 0x24, 0x10, 0x8b, 0x4c, 0x24, 0x14, 0x55, 0x8b, 0x6c, 0x24, 0x10, 0x81, 0xfd, 0x00, 0x01, 0x00, 0x00, 0x89, 0x87, 0x44,
      0x09, 0x00, 0x00, 0x89, 0x8f, 0x48, 0x09, 0x00, 0x00}},
    {HORIZON_GAME_TMUF, 0x00BD109C, 0x134, 0x0098B790, 0x2B, 0x930, 0x944,
     {0x8b, 0x44, 0x24, 0x10, 0x8b, 0x4c, 0x24, 0x14, 0x55, 0x8b, 0x6c, 0x24, 0x10, 0x81, 0xfd, 0x00, 0x01, 0x00, 0x00, 0x89, 0x87, 0x44,
      0x09, 0x00, 0x00, 0x89, 0x8f, 0x48, 0x09, 0x00, 0x00}},
    {HORIZON_GAME_ESWC, 0x00AF5CD8, 0x104, 0x007F21E0, 0x27, 0x844, 0x858,
     {0x8b, 0x44, 0x24, 0x0c, 0x8b, 0x4c, 0x24, 0x10, 0x55, 0x8b, 0x6c, 0x24, 0x0c, 0x81, 0xfd, 0x00, 0x01, 0x00, 0x00, 0x89, 0x83, 0x58,
      0x08, 0x00, 0x00, 0x89, 0x8b, 0x5c, 0x08, 0x00, 0x00}},
    {HORIZON_GAME_SUNRISE, 0x00A31B68, 0x0F8, 0x006DCD50, 0x27, 0x87C, 0x890,
     {0x8b, 0x44, 0x24, 0x0c, 0x8b, 0x4c, 0x24, 0x10, 0x55, 0x8b, 0x6c, 0x24, 0x0c, 0x81, 0xfd, 0x00, 0x01, 0x00, 0x00, 0x89, 0x83, 0x90,
      0x08, 0x00, 0x00, 0x89, 0x8b, 0x94, 0x08, 0x00, 0x00}},
    {HORIZON_GAME_ORIGINAL, 0x00A2DB28, 0x0F8, 0x006DC0E0, 0x27, 0x87C, 0x890,
     {0x8b, 0x44, 0x24, 0x0c, 0x8b, 0x4c, 0x24, 0x10, 0x55, 0x8b, 0x6c, 0x24, 0x0c, 0x81, 0xfd, 0x00, 0x01, 0x00, 0x00, 0x89, 0x83, 0x90,
      0x08, 0x00, 0x00, 0x89, 0x8b, 0x94, 0x08, 0x00, 0x00}},
};

const Exe* exeOf(uint32_t game) {
    for (const Exe& e : kExes)
        if (e.game == game) return &e;
    return nullptr;
}

uintptr_t rebase(uintptr_t linkVa) { return linkVa - 0x00400000u + reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// Guarded reads (no C++ objects in these frames: __try).
bool read32(const void* p, uint32_t* out) {
    __try {
        *out = *static_cast<const volatile uint32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool readBytes(const void* p, uint8_t* out, size_t n) {
    __try {
        for (size_t i = 0; i < n; i++) out[i] = static_cast<const volatile uint8_t*>(p)[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool write32(void* p, uint32_t v) {
    __try {
        *static_cast<volatile uint32_t*>(p) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using InitFn = void(__thiscall*)(void* viewport, uint32_t count, uint32_t half, uint32_t timeout);
bool callInit(InitFn fn, void* viewport, uint32_t count, uint32_t half, uint32_t timeout) {
    __try {
        fn(viewport, count, half, timeout);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every check for this exe; *fn: SyncGpuInit as loaded.
bool verify(void* viewport, const Exe* e, InitFn* fn, std::string* why) {
    if (!e) {
        *why = "not known for this game";
        return false;
    }
    if (!viewport) {
        *why = "no viewport yet";
        return false;
    }
    uint32_t table = 0, slot = 0;
    if (!read32(viewport, &table) || table != rebase(e->vtable)) {
        *why = "viewport not the known class (other exe build)";
        return false;
    }
    if (!read32(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(table)) + e->slot, &slot) || slot != rebase(e->fn)) {
        *why = "SyncGpuInit not where expected (hooked or other build)";
        return false;
    }
    uint8_t got[sizeof(e->sig)];
    if (!readBytes(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(slot)) + e->sigOff, got, sizeof(got)) || memcmp(got, e->sig, sizeof(got)) != 0) {
        *why = "SyncGpuInit's code differs";
        return false;
    }
    if (fn) *fn = reinterpret_cast<InitFn>(static_cast<uintptr_t>(slot));
    return true;
}

int liveValue(void* viewport, const Exe* e) {
    uint32_t entries = 0, half = 0;
    const uint8_t* v = static_cast<const uint8_t*>(viewport);
    if (!read32(v + e->ring, &entries) || !read32(v + e->half, &half)) return -1;
    return fromRing(entries, half != 0);
}

} // namespace

Live readLive(void* viewport, uint32_t game) {
    Live l;
    const Exe* e = exeOf(game);
    l.verified = verify(viewport, e, nullptr, &l.why);
    if (l.verified) l.value = liveValue(viewport, e);
    return l;
}

bool setLive(void* viewport, uint32_t game, int value, std::string* why) {
    const Exe* e = exeOf(game);
    InitFn fn = nullptr;
    std::string w;
    if (!verify(viewport, e, &fn, &w)) {
        if (why) *why = w;
        return false;
    }
    uint32_t count = 0, half = 0, timeout = 0;
    initArgs(value, &count, &half);
    if (!read32(static_cast<const uint8_t*>(viewport) + e->half + 4, &timeout) || timeout == 0 || timeout > 600000) timeout = 10000; // the game's default
    if (!callInit(fn, viewport, count, half, timeout)) {
        if (why) *why = "SyncGpuInit failed";
        return false;
    }
    if (liveValue(viewport, e) != value) {
        if (why) *why = "the game kept its value (no device or no event queries)";
        return false;
    }
    return true;
}

bool setDisplayValue(void* display, int32_t offset, int value) {
    if (!display || offset < 0 || value < 0 || value > 7) return false;
    if (readDisplayValue(display, offset) < 0) return false; // not a value 0..7 there: not the field
    return write32(static_cast<uint8_t*>(display) + offset, static_cast<uint32_t>(value));
}

int readDisplayValue(void* display, int32_t offset) {
    uint32_t v = 0;
    if (!display || offset < 0 || !read32(static_cast<const uint8_t*>(display) + offset, &v) || v > 7) return -1;
    return static_cast<int>(v);
}

bool persist(const std::wstring& file, int value, int* old, std::string* message) {
    std::string in;
    {
        std::ifstream f(file, std::ios::binary);
        if (!f) {
            if (message) *message = "no Default.SystemConfig.Gbx";
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        in = ss.str();
    }
    std::string out;
    const Edit r = setInConfig(in, value, &out, old);
    if (message) *message = editText(r);
    if (r == Edit::Unchanged) return true;
    if (r != Edit::Ok) return false;
    const std::wstring backup = file + L".horizon-backup";
    if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES && !CopyFileW(file.c_str(), backup.c_str(), TRUE)) {
        if (message) *message = "no backup possible, not changed";
        return false;
    }
    const std::wstring tmp = file + L".horizon-tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f) {
            if (message) *message = "could not write";
            DeleteFileW(tmp.c_str());
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (message) *message = "could not write";
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace gpusync
} // namespace perf
