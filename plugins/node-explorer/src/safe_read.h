// Guarded memory reads for the Node Explorer. Never writes, never calls game code.
//
// Every read first checks the page with VirtualQuery (committed, readable, no guard page: touching a
// guard page would steal another thread's stack growth) and then copies inside __try/__except.
// Object reads count against a per-frame budget (beginFrame sets it); class tables use rawRead,
// which checks pages too but does not count.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace nx {

// Starts a frame: forgets the page cache (memory may have been freed) and resets the read budget.
void beginFrame(int readLimit);
// Budgeted read: false when the budget is used up (budgetHit() then returns true) or the memory is not readable.
bool read(uintptr_t addr, void* out, size_t n);
// Unbudgeted read for static data (class and member tables).
bool rawRead(uintptr_t addr, void* out, size_t n);
// True when addr..addr+n is committed, readable memory without a guard page.
bool readable(uintptr_t addr, size_t n);

int readsUsed();
int readLimit();
bool budgetHit();

template <typename T> bool readT(uintptr_t addr, T* out) { return read(addr, out, sizeof(T)); }
template <typename T> bool rawReadT(uintptr_t addr, T* out) { return rawRead(addr, out, sizeof(T)); }

} // namespace nx
