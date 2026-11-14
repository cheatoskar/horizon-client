// Online fix for ESWC, TM Original and TM Sunrise (host setting OnlineFix, default on). Moved here unchanged from my
// eswc-autosave (src/online.cpp, MIT): the game's login must keep working when this host replaces that loader.
//
// TM Original (Online Patch v2 exe): the patch's readme asks for the same two things system-wide (Internet
// Options: "Use TLS 1.0" on, "Check for server certificate revocation" off). The same in-process change is used
// there, at the exe's own HttpOpenRequestA import slot (0x00a162d4); nothing in the game's login code is touched.
// Not tested against the server yet.
//
// TM Sunrise (Win10 community repack, TmSunrise.exe, image base 0x400000). Checked by me on 2026-11-14:
//  - Nadeo's legacy master box 91.121.54.245 (the same box as game.trackmanianations.com, the ESWC master) still
//    serves Sunrise: over HTTPS (TLS 1.0 only, expired certificate) with "Host: game.trackmaniasunrise.com" it
//    answers /online_game/getservers.php from its Sunrise scripts, with "Host: www.trackmaniasunrise.com" from the
//    Sunrise web scripts; "/" gives an Apache 403 naming the vhost. So the site is picked by the Host header.
//  - DNS: game.trackmaniasunrise.com does not resolve; www.trackmaniasunrise.com resolves to a parking server that
//    resets the connection, and the game shows HTTP error 12031. With both names -> 91.121.54.245 in the hosts file
//    the error becomes 12157, the ESWC TLS 1.0 problem.
//  - The exe's WinINet import slots (pefile): InternetCloseHandle 0x00a1a2d4, InternetConnectA 0x00a1a2d8,
//    HttpOpenRequestA 0x00a1a2dc. The code calls InternetConnectA and HttpOpenRequestA once each through
//    "call [slot]" (0x00484adb, 0x00485911) and loads InternetCloseHandle from its slot at the call, so swapping a
//    slot reaches every call.
//  - HttpOpenRequestA flags (0x004858d6): 0x84000100 | KEEP_CONNECTION, for port 443 also 0x0080f000 (SECURE,
//    IGNORE_CERT_CN_INVALID, IGNORE_CERT_DATE_INVALID, IGNORE_REDIRECT_TO_HTTP/HTTPS), as ESWC. The game ignores the
//    certificate's name, so connecting by IP address changes nothing in the certificate check.
//  - The only master name in the exe is www.trackmaniasunrise.com.
// Inferred, not checked: game.trackmaniasunrise.com comes from the master's own answers (as on ESWC); the box needs
// no SNI (WinINet sends none for an IP address; the vhost is picked by the Host header).
// Not tested in the game yet.
//
// Fix, only inside this process:
//  1. When WinINet reads the DWORD "SecureProtocols", TLS 1.0 (0x80) is added. Nothing else changes,
//     the registry itself is untouched.
//  2. The game's HttpOpenRequestA (import slot given by the caller) additionally sets
//     SECURITY_FLAG_IGNORE_REVOCATION on HTTPS requests; the game already ignores name and date of
//     the certificate, and the revocation check of that expired certificate fails (12057), which
//     the game's retry does not handle.
//  3. Sunrise only (the caller gives the InternetConnectA slot): InternetConnectA to game.trackmaniasunrise.com or
//     www.trackmaniasunrise.com (any case) connects to 91.121.54.245 instead (kSunriseMasterIp in onlinefix_names.h,
//     hard-coded), and every request opened on that connection gets "Host: <the original name>" back. A hosts-file
//     entry for these names keeps working too (the same box). InternetCloseHandle (slot also given) forgets the
//     connection; the small table is bounded and also overwritten when WinINet hands out a handle value again.
// ESWC and Original: no URL, host, port, request or answer is changed. Sunrise: only the server address of the two
// master names; path, port and request stay as they are, the Host header stays the name the game asked for.
#include <windows.h>
#include <wininet.h>
#include <cstdint>
#include <stdio.h>
#include <string.h>

#include "host/host.h"
#include "onlinefix_names.h"

namespace horizon {
namespace {

constexpr uintptr_t kPreferredBase = 0x00400000;
constexpr DWORD kTls10Client = 0x80; // SP_PROT_TLS1_0_CLIENT as used by "SecureProtocols"

using FnRegQuery = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using FnOpenRequest = HINTERNET(WINAPI*)(HINTERNET, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR*, DWORD, DWORD_PTR);
using FnConnect = HINTERNET(WINAPI*)(HINTERNET, LPCSTR, INTERNET_PORT, LPCSTR, LPCSTR, DWORD, DWORD, DWORD_PTR);
using FnClose = BOOL(WINAPI*)(HINTERNET);

FnRegQuery g_regQuery = nullptr;
FnOpenRequest g_openRequest = nullptr;
FnConnect g_connect = nullptr;
FnClose g_close = nullptr;
volatile LONG g_loggedProtocols = 0;
volatile LONG g_loggedRequests = 0;
volatile LONG g_loggedRedirect = 0;
volatile LONG g_loggedHostFail = 0;

// Sunrise: connections opened to kSunriseMasterIp in place of a master name (connect handle -> that name).
struct Redirect {
    HINTERNET connect;
    const char* name; // static, from kSunriseMasterNames
    INTERNET_PORT port;
};
constexpr int kMaxRedirects = 16;
Redirect g_redirects[kMaxRedirects] = {};
int g_redirectNext = 0;
SRWLOCK g_redirectLock = SRWLOCK_INIT;

// Records connect -> name (name nullptr: forgets connect). A full table overwrites its oldest entry.
void redirectSet(HINTERNET connect, const char* name, INTERNET_PORT port) {
    if (!connect) return;
    AcquireSRWLockExclusive(&g_redirectLock);
    int freeSlot = -1;
    for (int i = 0; i < kMaxRedirects; i++) {
        if (g_redirects[i].connect == connect) g_redirects[i] = Redirect{};
        if (!g_redirects[i].connect && freeSlot < 0) freeSlot = i;
    }
    if (name) {
        if (freeSlot < 0) {
            freeSlot = g_redirectNext;
            g_redirectNext = (g_redirectNext + 1) % kMaxRedirects;
        }
        g_redirects[freeSlot] = Redirect{connect, name, port};
    }
    ReleaseSRWLockExclusive(&g_redirectLock);
}

bool redirectFind(HINTERNET connect, const char** name, INTERNET_PORT* port) {
    bool found = false;
    AcquireSRWLockShared(&g_redirectLock);
    for (int i = 0; i < kMaxRedirects && !found; i++) {
        if (g_redirects[i].connect == connect) {
            *name = g_redirects[i].name;
            *port = g_redirects[i].port;
            found = true;
        }
    }
    ReleaseSRWLockShared(&g_redirectLock);
    return found;
}

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
    const char* hostName = nullptr;
    INTERNET_PORT hostPort = 0;
    if (r && g_connect && redirectFind(connect, &hostName, &hostPort)) { // g_connect: only on Sunrise
        // The connection goes to the IP address: the box picks its site by this header.
        char header[96];
        if (hostPort == 0 || hostPort == INTERNET_DEFAULT_HTTPS_PORT || hostPort == INTERNET_DEFAULT_HTTP_PORT)
            snprintf(header, sizeof(header), "Host: %s\r\n", hostName);
        else
            snprintf(header, sizeof(header), "Host: %s:%u\r\n", hostName, static_cast<unsigned>(hostPort));
        const DWORD how = HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE;
        if (!HttpAddRequestHeadersA(r, header, static_cast<DWORD>(-1), how) && InterlockedExchange(&g_loggedHostFail, 1) == 0)
            HLOG("online: Host header %s NOT set (error %lu)", hostName, GetLastError());
    }
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

HINTERNET WINAPI connectDetour(HINTERNET session, LPCSTR server, INTERNET_PORT port, LPCSTR user, LPCSTR password,
                               DWORD service, DWORD flags, DWORD_PTR context) {
    const char* master = sunriseMasterName(server);
    HINTERNET c = g_connect(session, master ? kSunriseMasterIp : server, port, user, password, service, flags, context);
    // also for other names: a handle value WinINet hands out again must not keep an old entry
    if (c) redirectSet(c, master, port);
    if (master && InterlockedExchange(&g_loggedRedirect, 1) == 0)
        HLOG("online: Sunrise master name -> %s (Host header kept)", kSunriseMasterIp);
    return c;
}

BOOL WINAPI closeDetour(HINTERNET handle) {
    redirectSet(handle, nullptr, 0); // before the close: afterwards the value may be handed out again
    return g_close(handle);
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

// Swaps the exe's import slot of wininet!function (link address iatSlot) for detour, if it still holds WinINet's.
bool hookImport(uintptr_t exeBase, uintptr_t iatSlot, const char* function, void* detour, void** original) {
    void** slot = reinterpret_cast<void**>(exeBase - kPreferredBase + iatSlot);
    HMODULE wi = GetModuleHandleW(L"wininet.dll");
    void* real = wi ? reinterpret_cast<void*>(GetProcAddress(wi, function)) : nullptr;
    if (!real || *slot != real) {
        HLOG("online: import slot of %s does not match, not hooked", function);
        return false;
    }
    *original = real;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = detour;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

} // namespace

void onlineFixInstall(uintptr_t exeBase, uintptr_t iatSlot, uintptr_t iatConnect, uintptr_t iatClose) {
    const bool a = hookRegQuery();
    const bool b = a && hookImport(exeBase, iatSlot, "HttpOpenRequestA", reinterpret_cast<void*>(&openRequestDetour),
                                   reinterpret_cast<void**>(&g_openRequest));
    if (a && b) HLOG("online: fix active (TLS 1.0 for WinINet in this process, no revocation check on HTTPS)");
    else HLOG("online: fix NOT active");
    if (!iatConnect) return;
    // Sunrise: the master names (the Host header needs the HttpOpenRequestA hook above)
    // hookImport sets the original before it swaps the slot, so a detour never sees it unset
    if (!(a && b) ||
        !hookImport(exeBase, iatConnect, "InternetConnectA", reinterpret_cast<void*>(&connectDetour),
                    reinterpret_cast<void**>(&g_connect))) {
        HLOG("online: Sunrise master name redirect NOT active");
        return;
    }
    // without it a closed connection stays in the table until its value comes back or the table is full
    if (iatClose)
        hookImport(exeBase, iatClose, "InternetCloseHandle", reinterpret_cast<void*>(&closeDetour),
                   reinterpret_cast<void**>(&g_close));
    HLOG("online: Sunrise master names (game./www.trackmaniasunrise.com) go to %s, Host header kept", kSunriseMasterIp);
}

} // namespace horizon
