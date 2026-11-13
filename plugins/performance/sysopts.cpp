#include "sysopts.h"

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <vector>

namespace perf {
namespace sys {
namespace {

DWORD g_origPriority = 0;
DWORD_PTR g_origAffinity = 0;
bool g_captured = false;
bool g_fineTimer = false;
char g_affNote[96] = "";
double g_qpcToMs = 0;

using NtSetTimerResolutionFn = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
using NtQueryTimerResolutionFn = LONG(NTAPI*)(PULONG, PULONG, PULONG);

void initClock() {
    if (g_qpcToMs != 0) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcToMs = 1000.0 / static_cast<double>(f.QuadPart);
}

// Performance cores of a hybrid CPU: the logical processors with the highest EfficiencyClass.
DWORD_PTR performanceCores(int* perfCount, int* total) {
    *perfCount = *total = 0;
    using Fn = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation"));
    if (!fn) return 0;
    ULONG len = 0;
    fn(nullptr, 0, &len, GetCurrentProcess(), 0);
    if (!len || len > 1 << 20) return 0;
    static unsigned char buf[1 << 16];
    if (len > sizeof(buf) || !fn(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf), len, &len, GetCurrentProcess(), 0)) return 0;
    BYTE maxClass = 0;
    for (ULONG off = 0; off < len;) {
        auto* e = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf + off);
        if (e->Type == CpuSetInformation && e->CpuSet.EfficiencyClass > maxClass) maxClass = e->CpuSet.EfficiencyClass;
        off += e->Size;
    }
    DWORD_PTR mask = 0;
    for (ULONG off = 0; off < len;) {
        auto* e = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf + off);
        if (e->Type == CpuSetInformation && e->CpuSet.Group == 0 && e->CpuSet.LogicalProcessorIndex < sizeof(DWORD_PTR) * 8) {
            (*total)++;
            if (e->CpuSet.EfficiencyClass == maxClass) {
                mask |= static_cast<DWORD_PTR>(1) << e->CpuSet.LogicalProcessorIndex;
                (*perfCount)++;
            }
        }
        off += e->Size;
    }
    if (maxClass == 0) return 0; // not a hybrid CPU: every core is the same class
    return mask;
}

} // namespace

void captureOriginals() {
    if (g_captured) return;
    g_captured = true;
    initClock();
    g_origPriority = GetPriorityClass(GetCurrentProcess());
    DWORD_PTR sysMask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &g_origAffinity, &sysMask);
}

bool setPriority(int level) {
    const DWORD want = level == 1 ? ABOVE_NORMAL_PRIORITY_CLASS : level == 2 ? HIGH_PRIORITY_CLASS : g_origPriority;
    return want != 0 && SetPriorityClass(GetCurrentProcess(), want) != FALSE;
}

bool setAffinity(int mode) {
    DWORD_PTR mask = g_origAffinity;
    if (mode == 1) {
        mask = g_origAffinity & ~static_cast<DWORD_PTR>(1);
        if (!mask) mask = g_origAffinity;
        snprintf(g_affNote, sizeof(g_affNote), "all cores but CPU 0");
    } else if (mode == 2) {
        int perf = 0, total = 0;
        const DWORD_PTR p = performanceCores(&perf, &total) & g_origAffinity;
        if (p) {
            mask = p;
            snprintf(g_affNote, sizeof(g_affNote), "%d performance threads of %d", perf, total);
        } else {
            snprintf(g_affNote, sizeof(g_affNote), "no hybrid CPU found: all cores");
        }
    } else {
        g_affNote[0] = 0;
    }
    return mask && SetProcessAffinityMask(GetCurrentProcess(), mask) != FALSE;
}

const char* affinityNote() { return g_affNote; }

// ---- smoothing switches
namespace {
char g_threadNote[96] = "";
char g_mmcssNote[96] = "";
bool g_timer1ms = false;
HANDLE g_mmcss = nullptr;
DWORD g_mmcssThread = 0;

// CPU set ids of the performance cores (group 0: a 32-bit game sees only that one). Empty when every core has the
// same efficiency class.
std::vector<ULONG> perfCpuSets(int* total) {
    std::vector<ULONG> ids;
    *total = 0;
    using Fn = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation"));
    if (!fn) return ids;
    ULONG len = 0;
    fn(nullptr, 0, &len, GetCurrentProcess(), 0);
    if (!len || len > 1 << 20) return ids;
    std::vector<unsigned char> buf(len);
    if (!fn(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()), len, &len, GetCurrentProcess(), 0)) return ids;
    BYTE minClass = 255, maxClass = 0;
    for (ULONG off = 0; off + sizeof(DWORD) * 2 <= len;) {
        auto* e = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data() + off);
        if (!e->Size) break;
        if (e->Type == CpuSetInformation && e->CpuSet.Group == 0) {
            (*total)++;
            if (e->CpuSet.EfficiencyClass > maxClass) maxClass = e->CpuSet.EfficiencyClass;
            if (e->CpuSet.EfficiencyClass < minClass) minClass = e->CpuSet.EfficiencyClass;
        }
        off += e->Size;
    }
    if (*total == 0 || minClass == maxClass) return ids; // not a hybrid CPU
    for (ULONG off = 0; off + sizeof(DWORD) * 2 <= len;) {
        auto* e = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data() + off);
        if (!e->Size) break;
        if (e->Type == CpuSetInformation && e->CpuSet.Group == 0 && e->CpuSet.EfficiencyClass == maxClass) ids.push_back(e->CpuSet.Id);
        off += e->Size;
    }
    return ids;
}
} // namespace

bool setThreadPerfCores(unsigned long threadId, bool on) {
    if (!threadId) return false;
    using Fn = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadSelectedCpuSets"));
    if (!fn) {
        snprintf(g_threadNote, sizeof(g_threadNote), "needs Windows 10");
        return false;
    }
    HANDLE t = OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId);
    if (!t) {
        snprintf(g_threadNote, sizeof(g_threadNote), "thread not opened (%lu)", GetLastError());
        return false;
    }
    bool ok = true;
    if (!on) {
        ok = fn(t, nullptr, 0) != FALSE; // no selection: the thread may run anywhere again
        g_threadNote[0] = 0;
    } else {
        int total = 0;
        const std::vector<ULONG> ids = perfCpuSets(&total);
        if (ids.empty()) {
            snprintf(g_threadNote, sizeof(g_threadNote), "no hybrid CPU: unchanged");
        } else if (fn(t, ids.data(), static_cast<ULONG>(ids.size()))) {
            snprintf(g_threadNote, sizeof(g_threadNote), "%u of %d CPU sets", static_cast<unsigned>(ids.size()), total);
        } else {
            ok = false;
            snprintf(g_threadNote, sizeof(g_threadNote), "refused by Windows (%lu)", GetLastError());
        }
    }
    CloseHandle(t);
    return ok;
}

const char* threadCoresNote() { return g_threadNote; }

bool setTimer1ms(bool on) {
    if (on == g_timer1ms) return true;
    HMODULE winmm = GetModuleHandleW(L"winmm.dll");
    if (!winmm) winmm = LoadLibraryW(L"winmm.dll"); // all five games import it, so this only adds a reference
    if (!winmm) return false;
    using Fn = UINT(WINAPI*)(UINT);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(winmm, on ? "timeBeginPeriod" : "timeEndPeriod"));
    if (!fn || fn(1) != 0) return false; // TIMERR_NOERROR = 0
    g_timer1ms = on;
    return true;
}

bool setMmcssGames(bool on) {
    HMODULE avrt = GetModuleHandleW(L"avrt.dll");
    if (!avrt) avrt = LoadLibraryW(L"avrt.dll");
    if (!avrt) {
        snprintf(g_mmcssNote, sizeof(g_mmcssNote), "avrt.dll missing");
        return false;
    }
    if (!on) {
        if (!g_mmcss) {
            g_mmcssNote[0] = 0;
            return true;
        }
        if (GetCurrentThreadId() != g_mmcssThread) return false; // only the thread that joined can leave
        using Fn = BOOL(WINAPI*)(HANDLE);
        auto fn = reinterpret_cast<Fn>(GetProcAddress(avrt, "AvRevertMmThreadCharacteristics"));
        const bool ok = fn && fn(g_mmcss);
        g_mmcss = nullptr;
        g_mmcssThread = 0;
        g_mmcssNote[0] = 0;
        return ok;
    }
    if (g_mmcss) return GetCurrentThreadId() == g_mmcssThread;
    using Fn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"));
    DWORD task = 0;
    HANDLE h = fn ? fn(L"Games", &task) : nullptr;
    if (!h) {
        snprintf(g_mmcssNote, sizeof(g_mmcssNote), "refused by Windows (%lu)", GetLastError());
        return false;
    }
    g_mmcss = h;
    g_mmcssThread = GetCurrentThreadId();
    snprintf(g_mmcssNote, sizeof(g_mmcssNote), "task Games");
    return true;
}

const char* mmcssNote() { return g_mmcssNote; }

bool setPowerThrottlingOff(bool off) {
    using Fn = BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation"));
    if (!fn) return false;
    PROCESS_POWER_THROTTLING_STATE s = {};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // 0x1 execution speed (EcoQoS), 0x4 ignore timer resolution (Windows 11: timer requests of hidden windows)
    s.ControlMask = off ? (0x1u | 0x4u) : 0;
    s.StateMask = 0;
    if (fn(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s))) return true;
    s.ControlMask = off ? 0x1u : 0; // older Windows 10 knows only the first flag
    return fn(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)) != FALSE;
}

bool setFineTimer(bool on) {
    auto fn = reinterpret_cast<NtSetTimerResolutionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetTimerResolution"));
    if (!fn || on == g_fineTimer) return fn != nullptr;
    ULONG cur = 0;
    const LONG st = fn(5000, on ? TRUE : FALSE, &cur); // 100 ns units: 0.5 ms
    if (st >= 0) g_fineTimer = on;
    return st >= 0;
}

double currentTimerMs() {
    auto fn = reinterpret_cast<NtQueryTimerResolutionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution"));
    ULONG mn = 0, mx = 0, cur = 0;
    if (!fn || fn(&mn, &mx, &cur) < 0) return 0;
    return cur / 10000.0;
}

Memory memory() {
    Memory m;
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(exe) + dos->e_lfanew);
    m.largeAddressAware = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        m.privateMB = pmc.PrivateUsage / (1024.0 * 1024.0);
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m.addressSpaceTotalMB = ms.ullTotalVirtual / (1024.0 * 1024.0);
        m.addressSpaceUsedMB = (ms.ullTotalVirtual - ms.ullAvailVirtual) / (1024.0 * 1024.0);
    }
    return m;
}

double secondsSinceProcessStart() {
    FILETIME c, e, k, u, now;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG a = (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    const ULONGLONG b = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return b > a ? (b - a) / 1e7 : 0;
}

double nowMs() {
    initClock();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * g_qpcToMs;
}

double waitUntil(double deadlineMs) {
    const double start = nowMs();
    double left = deadlineMs - start;
    if (left <= 0) return 0;
    static HANDLE timer = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+): fine waits without changing the timer resolution
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    const double spin = 0.7; // ms kept for spinning: timer wake-ups can be late by about that much
    if (timer && left > spin + 0.2) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((left - spin) * 10000.0); // relative, 100 ns units
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 1000);
    }
    while (nowMs() < deadlineMs) YieldProcessor();
    return nowMs() - start;
}

} // namespace sys
} // namespace perf
