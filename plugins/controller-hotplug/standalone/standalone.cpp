// controller_hotplug.dll - standalone build of the Horizon module controller-hotplug (same code: hp_*.cpp).
//
// Started by the Controller Hotplug Loader (dinput8.dll) right after the game's first DirectInput8Create, on the
// game's main thread. Checks the exe (PE timestamp + every code byte it uses), claims controller hot-plug for this
// process (the Horizon module then stays passive) and starts the worker thread. Steps run on the game's main
// thread: the worker posts WM_NULL with a marker to a window of that thread; a WH_GETMESSAGE hook of that thread
// sees it when the game takes it from its queue. Log: controller_hotplug.log next to this DLL.
#include <windows.h>

#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../hp_service.h"

namespace {
constexpr unsigned kLoaderApi = 1;
constexpr char kVersion[] = "1.0.0";
constexpr WPARAM kMarker = 0x48504C47; // 'HPLG'

CRITICAL_SECTION g_logLock;
FILE* g_log = nullptr;
DWORD g_thread = 0;
HHOOK g_hook = nullptr;
HWND g_wnd = nullptr;
HANDLE g_claim = nullptr;
volatile LONG g_queued = 0;
hp::GameBackend* g_backend = nullptr;
hp::Service* g_service = nullptr;

void logLine(int level, const char* text) {
    EnterCriticalSection(&g_logLock);
    if (g_log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_log, "%02d:%02d:%02d.%03d %s %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                level == 0 ? "INFO " : level == 1 ? "WARN " : "ERROR", text);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_logLock);
}

void logf(int level, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    logLine(level, buf);
}

void logCb(void*, int level, const char* text) { logLine(level, text); }
void toastCb(void*, const char* title) { logLine(0, title); } // no overlay in the standalone build: the log only

BOOL CALLBACK pickWindow(HWND h, LPARAM lp) {
    HWND* out = reinterpret_cast<HWND*>(lp);
    if (!*out) *out = h;
    if (IsWindowVisible(h)) {
        *out = h;
        return FALSE;
    }
    return TRUE;
}

// worker thread: ask the game thread for a step
void kick(void*) {
    if (InterlockedExchange(&g_queued, 1) != 0) return;
    if (!g_wnd || !IsWindow(g_wnd)) {
        HWND w = nullptr;
        EnumThreadWindows(g_thread, &pickWindow, reinterpret_cast<LPARAM>(&w));
        g_wnd = w;
    }
    BOOL ok = g_wnd ? PostMessageW(g_wnd, WM_NULL, kMarker, 0) : FALSE;
    if (!ok) ok = PostThreadMessageW(g_thread, WM_NULL, kMarker, 0);
    if (!ok) InterlockedExchange(&g_queued, 0);
}

LRESULT CALLBACK getMessageHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == PM_REMOVE) {
        MSG* m = reinterpret_cast<MSG*>(lp);
        if (m && m->message == WM_NULL && m->wParam == kMarker) {
            InterlockedExchange(&g_queued, 0);
            if (g_service) g_service->tick();
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

void openLog() {
    wchar_t path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&openLog), &self);
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash || (slash - path) + 24 >= MAX_PATH) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"controller_hotplug.log");
    g_log = _wfsopen(path, L"w", _SH_DENYWR);
}
} // namespace

extern "C" __declspec(dllexport) int __cdecl ControllerHotplugInit(unsigned loaderApi) {
    static volatile LONG started = 0;
    if (InterlockedExchange(&started, 1) != 0) return 0;
    InitializeCriticalSection(&g_logLock);
    openLog();
    logf(0, "---- controller_hotplug %s start (standalone) ----", kVersion);
    if (loaderApi != kLoaderApi) {
        logf(2, "loader api %u, expected %u: dinput8.dll and controller_hotplug.dll are from different versions; off", loaderApi, kLoaderApi);
        return 1;
    }
    HMODULE exe = GetModuleHandleW(nullptr);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
    const hp::GameTable* table = hp::tableForTimestamp(nt->FileHeader.TimeDateStamp);
    if (!table) {
        logf(1, "unknown exe (PE timestamp %08lX): only TmNationsESWC.exe 1.80 and TmSunrise.exe (Win10 repack) are supported; off",
             static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
        return 2;
    }
    if (!hp::claimOwner(&g_claim)) {
        logf(1, "controller hot-plug already runs in this game (Horizon module): off");
        return 3;
    }
    g_backend = new hp::GameBackend(table, reinterpret_cast<uintptr_t>(exe), &logCb, nullptr);
    if (!g_backend->verify()) return 4; // logged
    g_thread = GetCurrentThreadId();
    g_hook = SetWindowsHookExW(WH_GETMESSAGE, &getMessageHook, nullptr, g_thread);
    if (!g_hook) {
        logf(2, "message hook failed (%lu): off", GetLastError());
        return 5;
    }
    g_service = new hp::Service();
    hp::ServiceCallbacks cb = {nullptr, &logCb, &toastCb, &kick};
    if (!g_service->start(g_backend, cb, false)) return 6;
    logf(0, "ready: %s, watching for controllers plugged in while the game runs", table->name);
    return 0;
}
