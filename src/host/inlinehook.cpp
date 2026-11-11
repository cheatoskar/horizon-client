// Hook helpers: inline hooks on the GAME's code (prologue checked byte for byte, never on d3d9),
// pointer writes into method tables and import slots, address descriptions for the log.
#include "host/host.h"

#include <cstdio>
#include <cstring>

namespace horizon {

uintptr_t rebase(uintptr_t link) { return state().game.base - 0x00400000 + link; }

bool readPointer(const void* address, void** out) {
    __try {
        *out = *static_cast<void* const*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writePointer(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

void describeAddress(const void* address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      static_cast<LPCWSTR>(address), &module) &&
        module) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        _snprintf_s(out, size, _TRUNCATE, "%s+0x%lx", name,
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
    } else {
        _snprintf_s(out, size, _TRUNCATE, "%p (no module)", address);
    }
}

bool installInlineHook(InlineHook& h, void* detour) {
    BYTE* target = reinterpret_cast<BYTE*>(rebase(h.link));
    if (target[0] == 0xE9 && memcmp(target, h.prologue, h.length) != 0) {
        // Another mod hooked this function already (e.g. the eswc-autosave loader in dinput8.chain.dll):
        // ours goes in front of its jump and continues into its detour, which runs the original.
        BYTE* theirs = target + 5 + *reinterpret_cast<int32_t*>(target + 1);
        BYTE* tramp = static_cast<BYTE*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!tramp) return false;
        tramp[0] = 0xE9;
        *reinterpret_cast<int32_t*>(tramp + 1) = static_cast<int32_t>(theirs - (tramp + 5));
        FlushInstructionCache(GetCurrentProcess(), tramp, 16);
        DWORD old = 0;
        if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
        h.trampoline = tramp;
        *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(static_cast<BYTE*>(detour) - (target + 5));
        VirtualProtect(target, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, 5);
        char where[160];
        describeAddress(theirs, where, sizeof(where));
        HLOG("hook: %s at %08X chained onto another mod's jump (to %s)", h.name, static_cast<unsigned>(h.link), where);
        return true;
    }
    if (memcmp(target, h.prologue, h.length) != 0) {
        char seen[64] = {};
        for (size_t i = 0; i < h.length && i < 12; i++) _snprintf_s(seen + i * 3, sizeof(seen) - i * 3, _TRUNCATE, "%02x ", target[i]);
        HWARN("hook: %s at %08X does not match (found %s; another mod hooked it, or another exe): not hooked", h.name,
              static_cast<unsigned>(h.link), seen);
        return false;
    }
    BYTE* tramp = static_cast<BYTE*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, target, h.length);
    tramp[h.length] = 0xE9;
    *reinterpret_cast<int32_t*>(tramp + h.length + 1) = static_cast<int32_t>((target + h.length) - (tramp + h.length + 5));
    FlushInstructionCache(GetCurrentProcess(), tramp, 32);
    DWORD old = 0;
    if (!VirtualProtect(target, h.length, PAGE_EXECUTE_READWRITE, &old)) return false;
    h.trampoline = tramp;
    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(static_cast<BYTE*>(detour) - (target + 5));
    for (size_t i = 5; i < h.length; i++) target[i] = 0x90;
    VirtualProtect(target, h.length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, h.length);
    HLOG("hook: %s at %08X installed", h.name, static_cast<unsigned>(h.link));
    return true;
}

bool patchImport(HMODULE module, const char* dll, const char* function, void* replacement, void** originalOut) {
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        if (!imp->OriginalFirstThunk) {
            // No name table (the Sunrise repack's exe): after loading, the slots hold addresses only.
            // Find the slot by the address the loader put there.
            HMODULE m = GetModuleHandleA(dll);
            void* real = m ? reinterpret_cast<void*>(GetProcAddress(m, function)) : nullptr;
            if (!real) return false;
            for (; slots->u1.Function; slots++) {
                if (reinterpret_cast<void*>(slots->u1.Function) != real) continue;
                *originalOut = real;
                return writePointer(reinterpret_cast<void**>(&slots->u1.Function), replacement);
            }
            return false;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* named = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(named->Name), function) != 0) continue;
            *originalOut = reinterpret_cast<void*>(slots->u1.Function);
            return writePointer(reinterpret_cast<void**>(&slots->u1.Function), replacement);
        }
    }
    return false;
}

} // namespace horizon
