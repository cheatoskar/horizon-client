// Online fix for ESWC and TM Original (host setting OnlineFix, default on). Moved here unchanged from my
// eswc-autosave (src/online.cpp, MIT): the game's login must keep working when this host replaces that loader.
//
// TM Original (Online Patch v2 exe): the patch's readme asks for the same two things system-wide (Internet
// Options: "Use TLS 1.0" on, "Check for server certificate revocation" off). The same in-process change is used
// there, at the exe's own HttpOpenRequestA import slot (0x00a162d4); nothing in the game's login code is touched.
// Not tested against the server yet.
//
// Finding: the game logs in at https://game.trackmanianations.com/online_game/... (host and path
// set in FUN_00401150, ports 443 at DAT_00c07368/6c). FUN_004e8710 opens every request with
// HttpOpenRequestA and, for port 443, INTERNET_FLAG_SECURE | IGNORE_CERT_CN_INVALID |
// IGNORE_CERT_DATE_INVALID | IGNORE_REDIRECT_*; on 12045/12037/12038 it retries with
// SECURITY_FLAGS |= 0xf380 (code at 0x004e7c6f). The server (Apache, PHP 5.2.6) still answers but
// only with TLS 1.0 (AES256-SHA, expired certificate for player.trackmania.com, no OCSP/CRL that
// works). WinINet takes its allowed protocols from "SecureProtocols" (HKCU Internet Settings,
// here 0x2800 = TLS 1.2|1.3) -> handshake fails with 12157 (ERROR_INTERNET_SECURITY_CHANNEL_ERROR).
// INTERNET_OPTION_SECURE_PROTOCOLS is rejected by this WinINet (12009/12018).
//
// Fix, only inside this process and only these two things:
//  1. When WinINet reads the DWORD "SecureProtocols", TLS 1.0 (0x80) is added. Nothing else changes,
//     the registry itself is untouched.
//  2. The game's HttpOpenRequestA (import slot 0x00aca340) additionally sets
//     SECURITY_FLAG_IGNORE_REVOCATION on HTTPS requests; the game already ignores name and date of
//     the certificate, and the revocation check of that expired certificate fails (12057), which
//     the game's retry does not handle.
// No URL, host, port, request or answer is changed.
#include <windows.h>
#include <wininet.h>
#include <cstdint>
#include <string.h>

#include "host/host.h"

namespace horizon {
namespace {

constexpr uintptr_t kPreferredBase = 0x00400000;
constexpr DWORD kTls10Client = 0x80; // SP_PROT_TLS1_0_CLIENT as used by "SecureProtocols"

using FnRegQuery = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using FnOpenRequest = HINTERNET(WINAPI*)(HINTERNET, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR*, DWORD, DWORD_PTR);

FnRegQuery g_regQuery = nullptr;
FnOpenRequest g_openRequest = nullptr;
volatile LONG g_loggedProtocols = 0;
volatile LONG g_loggedRequests = 0;

LSTATUS WINAPI regQueryDetour(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size) {
    const LSTATUS s = g_regQuery(key, name, reserved, type, data, size);
    if (s == ERROR_SUCCESS && name && data && size && *size >= sizeof(DWORD) && (!type || *type == REG_DWORD) &&
        _wcsicmp(name, L"SecureProtocols") == 0) {
        DWORD* v = reinterpret_cast<DWORD*>(data);
        if (!(*v & kTls10Client)) {
            const DWORD before = *v;
            *v |= kTls10Client;
            if (InterlockedExchange(&g_loggedProtocols, 1) == 0)
                HLOG("online: WinINet SecureProtocols 0x%lX -> 0x%lX for this process (TLS 1.0 allowed)", before, *v);
        }
    }
    return s;
}

HINTERNET WINAPI openRequestDetour(HINTERNET connect, LPCSTR verb, LPCSTR object, LPCSTR version, LPCSTR referrer,
                                   LPCSTR* accept, DWORD flags, DWORD_PTR context) {
    HINTERNET r = g_openRequest(connect, verb, object, version, referrer, accept, flags, context);
    if (r && (flags & INTERNET_FLAG_SECURE)) {
        DWORD sf = 0, len = sizeof(sf);
        if (InternetQueryOptionA(r, INTERNET_OPTION_SECURITY_FLAGS, &sf, &len)) {
            sf |= SECURITY_FLAG_IGNORE_REVOCATION;
            const BOOL ok = InternetSetOptionA(r, INTERNET_OPTION_SECURITY_FLAGS, &sf, sizeof(sf));
            if (g_loggedRequests < 20) {
                InterlockedIncrement(&g_loggedRequests);
                // Path only: the query string can hold login and password.
                char path[96] = "";
                if (object) {
                    size_t i = 0;
                    for (; object[i] && object[i] != '?' && i < sizeof(path) - 1; i++) path[i] = object[i];
                    path[i] = 0;
                }
                HLOG("online: HTTPS %s %s -> revocation check off %s", verb ? verb : "GET", path, ok ? "" : "(FAILED)");
            }
        }
    }
    return r;
}

bool hookRegQuery() {
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    BYTE* t = kb ? reinterpret_cast<BYTE*>(GetProcAddress(kb, "RegQueryValueExW")) : nullptr;
    // Hot-patchable prologue: mov edi,edi / push ebp / mov ebp,esp
    if (!t || !(t[0] == 0x8B && t[1] == 0xFF && t[2] == 0x55 && t[3] == 0x8B && t[4] == 0xEC)) {
        HLOG("online: kernelbase!RegQueryValueExW has an unexpected start, not hooked");
        return false;
    }
    BYTE* tramp = static_cast<BYTE*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, t, 5);
    tramp[5] = 0xE9;
    *reinterpret_cast<INT32*>(tramp + 6) = static_cast<INT32>((t + 5) - (tramp + 10));
    FlushInstructionCache(GetCurrentProcess(), tramp, 16);
    g_regQuery = reinterpret_cast<FnRegQuery>(tramp);
    DWORD old = 0;
    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    t[0] = 0xE9;
    *reinterpret_cast<INT32*>(t + 1) = static_cast<INT32>(reinterpret_cast<BYTE*>(&regQueryDetour) - (t + 5));
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    return true;
}

bool hookOpenRequest(uintptr_t exeBase, uintptr_t iatSlot) {
    void** slot = reinterpret_cast<void**>(exeBase - kPreferredBase + iatSlot);
    HMODULE wi = GetModuleHandleW(L"wininet.dll");
    void* real = wi ? reinterpret_cast<void*>(GetProcAddress(wi, "HttpOpenRequestA")) : nullptr;
    if (!real || *slot != real) {
        HLOG("online: import slot of HttpOpenRequestA does not match, not hooked");
        return false;
    }
    g_openRequest = reinterpret_cast<FnOpenRequest>(real);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = reinterpret_cast<void*>(&openRequestDetour);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

} // namespace

void onlineFixInstall(uintptr_t exeBase, uintptr_t iatSlot) {
    const bool a = hookRegQuery();
    const bool b = a && hookOpenRequest(exeBase, iatSlot);
    if (a && b) HLOG("online: fix active (TLS 1.0 for WinINet in this process, no revocation check on HTTPS)");
    else HLOG("online: fix NOT active");
}

} // namespace horizon
