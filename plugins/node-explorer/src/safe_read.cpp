#include "safe_read.h"

#include <windows.h>
#include <string.h>

namespace nx {
namespace {

struct Region {
    uintptr_t begin, end; // [begin, end) readable
};
const int kCache = 32;
Region g_cache[kCache];
int g_cacheCount = 0, g_cacheNext = 0;
int g_limit = 2000, g_used = 0;
bool g_hit = false;

bool protectReadable(DWORD p) {
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD base = p & 0xff;
    return base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
           base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

bool cached(uintptr_t a, uintptr_t e) {
    for (int i = 0; i < g_cacheCount; i++)
        if (a >= g_cache[i].begin && e <= g_cache[i].end) return true;
    return false;
}

void remember(uintptr_t b, uintptr_t e) {
    g_cache[g_cacheNext] = {b, e};
    g_cacheNext = (g_cacheNext + 1) % kCache;
    if (g_cacheCount < kCache) g_cacheCount++;
}

bool copyGuarded(uintptr_t addr, void* out, size_t n) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool readable(uintptr_t addr, size_t n) {
    if (addr < 0x10000 || n == 0 || addr + n < addr) return false;
    const uintptr_t end = addr + n;
    if (cached(addr, end)) return true;
    uintptr_t a = addr;
    int guard = 0;
    while (a < end && guard++ < 8) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT || !protectReadable(mbi.Protect)) return false;
        const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t re = rb + mbi.RegionSize;
        remember(rb, re);
        a = re;
    }
    return a >= end;
}

void beginFrame(int limit) {
    g_cacheCount = 0;
    g_cacheNext = 0;
    g_limit = limit < 1 ? 1 : limit;
    g_used = 0;
    g_hit = false;
}

bool rawRead(uintptr_t addr, void* out, size_t n) {
    if (!readable(addr, n)) return false;
    return copyGuarded(addr, out, n);
}

bool read(uintptr_t addr, void* out, size_t n) {
    if (g_used >= g_limit) {
        g_hit = true;
        return false;
    }
    g_used++;
    return rawRead(addr, out, n);
}

int readsUsed() { return g_used; }
int readLimit() { return g_limit; }
bool budgetHit() { return g_hit; }

} // namespace nx
