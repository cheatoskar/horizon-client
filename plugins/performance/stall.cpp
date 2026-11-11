// See stall.h.
#include "stall.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "sysopts.h"

namespace perf {
namespace stall {
namespace {

CRITICAL_SECTION g_cs;
bool g_csInit = false;
std::wstring g_logPath, g_gameFolder;
uint32_t g_exeTimestamp = 0;
volatile LONG g_enabled = 0;
volatile LONG g_mainTid = 0;
volatile LONG64 g_lastFrameUs = 0;
HWND g_window = nullptr;
double g_thresholdMs = 1500;
char g_state[48] = "starting";
volatile LONG g_race = 0;
volatile LONG g_busy = 0;
bool g_assumeFront = false;
HANDLE g_thread = nullptr;
volatile LONG g_stop = 0;
Last g_last;

struct Module {
    uintptr_t base = 0, size = 0;
    char name[64] = "";
    bool isExe = false;
};
std::vector<Module> g_modules;
std::vector<std::pair<uint32_t, std::string>> g_map; // TMNF TmForever.map: link VA -> name
bool g_mapTried = false;

void writeLine(const char* line) {
    if (!g_csInit) return;
    EnterCriticalSection(&g_cs);
    if (FILE* f = _wfopen(g_logPath.c_str(), L"ab")) {
        fputs(line, f);
        fputs("\r\n", f);
        fclose(f);
    }
    LeaveCriticalSection(&g_cs);
}

void refreshModules() {
    g_modules.clear();
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    const DWORD n = needed / sizeof(HMODULE) < 512 ? needed / sizeof(HMODULE) : 512;
    HMODULE exe = GetModuleHandleW(nullptr);
    for (DWORD i = 0; i < n; i++) {
        MODULEINFO mi = {};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        Module m;
        m.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        m.size = mi.SizeOfImage;
        m.isExe = mods[i] == exe;
        char path[MAX_PATH] = "";
        GetModuleFileNameA(mods[i], path, MAX_PATH);
        const char* slash = strrchr(path, '\\');
        strncpy_s(m.name, slash ? slash + 1 : path, _TRUNCATE);
        g_modules.push_back(m);
    }
}

const Module* moduleOf(uintptr_t a) {
    for (const auto& m : g_modules)
        if (a >= m.base && a < m.base + m.size) return &m;
    return nullptr;
}

bool readBytes(uintptr_t a, void* out, size_t n) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Is `ret` right after a CALL instruction? (the usual shapes MSVC emits)
bool afterCall(uintptr_t ret) {
    unsigned char b[8];
    if (ret < 8 || !readBytes(ret - 8, b, 8)) return false;
    const unsigned char* e = b + 8; // e[-1] is the byte just before ret
    if (e[-5] == 0xE8) return true;                                // call rel32
    if (e[-6] == 0xFF && (e[-5] == 0x15 || (e[-5] & 0xF8) == 0x90)) return true; // call [abs] / [reg+disp32]
    if (e[-2] == 0xFF && ((e[-1] & 0xF8) == 0xD0 || (e[-1] & 0xF8) == 0x10)) return true; // call reg / [reg]
    if (e[-3] == 0xFF && (e[-2] & 0xF8) == 0x50) return true;      // call [reg+disp8]
    if (e[-4] == 0xFF && e[-3] == 0x54) return true;               // call [reg+reg*s+disp8]
    if (e[-7] == 0xFF && e[-6] == 0x94) return true;               // call [reg+reg*s+disp32]
    return false;
}

const char* nearestExport(const Module& m, uintptr_t a, uintptr_t* off) {
    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m.base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(m.base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress) return nullptr;
        auto* ex = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(m.base + dir.VirtualAddress);
        auto* names = reinterpret_cast<DWORD*>(m.base + ex->AddressOfNames);
        auto* ords = reinterpret_cast<WORD*>(m.base + ex->AddressOfNameOrdinals);
        auto* funcs = reinterpret_cast<DWORD*>(m.base + ex->AddressOfFunctions);
        const uintptr_t rva = a - m.base;
        DWORD best = 0;
        const char* bestName = nullptr;
        for (DWORD i = 0; i < ex->NumberOfNames; i++) {
            const DWORD f = funcs[ords[i]];
            if (f <= rva && f > best) {
                best = f;
                bestName = reinterpret_cast<const char*>(m.base + names[i]);
            }
        }
        if (!bestName || rva - best > 0x20000) return nullptr;
        *off = rva - best;
        return bestName;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void loadMap() {
    if (g_mapTried) return;
    g_mapTried = true;
    if (g_exeTimestamp != 0x4d431494u) return; // TmForever.map belongs to the TMNF exe only
    FILE* f = _wfopen((g_gameFolder + L"TmForever.map").c_str(), L"rb");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        unsigned seg = 0, off = 0, va = 0;
        char name[600];
        if (sscanf_s(line, " %x:%x %599s %x", &seg, &off, name, static_cast<unsigned>(sizeof(name)), &va) == 4 && seg == 1 && va >= 0x400000)
            g_map.emplace_back(va, name);
    }
    fclose(f);
    std::sort(g_map.begin(), g_map.end());
}

void describeImpl(uintptr_t a, char* out, size_t n) {
    const Module* m = moduleOf(a);
    if (!m) {
        snprintf(out, n, "0x%08x", static_cast<unsigned>(a));
        return;
    }
    const unsigned off = static_cast<unsigned>(a - m->base);
    if (m->isExe) {
        loadMap();
        const uint32_t link = static_cast<uint32_t>(a - m->base + 0x400000);
        if (!g_map.empty()) {
            auto it = std::upper_bound(g_map.begin(), g_map.end(), std::make_pair(link, std::string("\x7f")));
            if (it != g_map.begin()) {
                --it;
                if (link - it->first < 0x10000) {
                    snprintf(out, n, "%s+0x%x (VA %08x) %s+0x%x", m->name, off, link, it->second.c_str(), link - it->first);
                    return;
                }
            }
        }
        snprintf(out, n, "%s+0x%x (VA %08x)", m->name, off, link);
        return;
    }
    uintptr_t eoff = 0;
    if (const char* e = nearestExport(*m, a, &eoff)) snprintf(out, n, "%s!%s+0x%x", m->name, e, static_cast<unsigned>(eoff));
    else snprintf(out, n, "%s+0x%x", m->name, off);
}

// ---- sampling: nothing between SuspendThread and ResumeThread allocates or takes a lock
struct Raw {
    uintptr_t eip = 0, esp = 0;
    size_t bytes = 0;
    uint32_t stack[4096]; // 16 KB
};
Raw g_raw;
uintptr_t g_stackBase = 0;
HANDLE g_target = nullptr;

bool copyStack(uintptr_t esp, size_t n) {
    __try {
        memcpy(g_raw.stack, reinterpret_cast<const void*>(esp), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool sampleRaw() {
    if (SuspendThread(g_target) == static_cast<DWORD>(-1)) return false;
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    bool ok = GetThreadContext(g_target, &ctx) != FALSE;
    if (ok) {
        g_raw.eip = ctx.Eip;
        g_raw.esp = ctx.Esp;
        size_t n = sizeof(g_raw.stack);
        if (g_stackBase > g_raw.esp && g_stackBase - g_raw.esp < n) n = g_stackBase - g_raw.esp;
        n &= ~static_cast<size_t>(3);
        g_raw.bytes = copyStack(g_raw.esp, n) ? n : 0;
    }
    ResumeThread(g_target);
    return ok;
}

bool openTarget(DWORD tid) {
    if (g_target) return true;
    g_target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!g_target) return false;
    using Fn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    auto q = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    struct {
        LONG exitStatus;
        PVOID teb;
        PVOID pid, tid;
        ULONG_PTR affinity;
        LONG priority, basePriority;
    } tbi = {};
    if (q && q(g_target, 0, &tbi, sizeof(tbi), nullptr) >= 0 && tbi.teb)
        readBytes(reinterpret_cast<uintptr_t>(tbi.teb) + 4, &g_stackBase, sizeof(g_stackBase)); // NT_TIB.StackBase
    return true;
}

// Return address chain of the last raw sample: EIP first, then up to 15 call sites.
std::vector<uintptr_t> frames() {
    std::vector<uintptr_t> f;
    f.push_back(g_raw.eip);
    for (size_t i = 0; i < g_raw.bytes / 4 && f.size() < 16; i++) {
        const uintptr_t v = g_raw.stack[i];
        const Module* m = moduleOf(v);
        if (!m || v == f.back()) continue;
        if (afterCall(v)) f.push_back(v);
    }
    return f;
}

double secs() { return sys::secondsSinceProcessStart(); }

// The game window is minimised or another program is in front: no frames then is normal (ESWC and Sunrise stop
// drawing in the background), not a hang. Any thread may ask.
bool windowAway() {
    HWND w = g_window;
    if (!w) return false;
    if (IsIconic(w)) return true;
    if (g_assumeFront) return false;
    HWND f = GetForegroundWindow();
    return !f || (f != w && GetAncestor(f, GA_ROOTOWNER) != GetAncestor(w, GA_ROOTOWNER));
}

DWORD WINAPI watcher(LPVOID) {
    bool inStall = false;
    double stallStart = 0, nextSample = 0;
    int samples = 0;
    IO_COUNTERS io0 = {};
    std::map<std::string, int> tops;
    while (!g_stop) {
        Sleep(100);
        if (!g_enabled || !g_mainTid) continue;
        const double now = sys::nowMs();
        const double last = static_cast<double>(g_lastFrameUs) / 1000.0;
        const double gap = now - last;
        const bool hidden = windowAway() || g_busy;
        if (!inStall) {
            if (gap < g_thresholdMs || g_race || hidden || last <= 0) continue;
            inStall = true;
            stallStart = last;
            samples = 0;
            nextSample = now;
            tops.clear();
            GetProcessIoCounters(GetCurrentProcess(), &io0);
            refreshModules();
            char state[48];
            EnterCriticalSection(&g_cs);
            strcpy_s(state, g_state);
            LeaveCriticalSection(&g_cs);
            note("stall: no frame for %.1f s (game state: %s); sampling the game thread", gap / 1000.0, state);
            if (!openTarget(static_cast<DWORD>(g_mainTid))) note("stall: cannot open the game thread (error %lu)", GetLastError());
        }
        if (gap < g_thresholdMs || last > stallStart) { // frames again
            inStall = false;
            IO_COUNTERS io1 = {};
            GetProcessIoCounters(GetCurrentProcess(), &io1);
            const double dur = (last - stallStart) / 1000.0;
            std::string top;
            int best = 0;
            for (const auto& kv : tops)
                if (kv.second > best) {
                    best = kv.second;
                    top = kv.first;
                }
            note("stall end: %.1f s without a frame; read %.1f MB in %llu ops, wrote %.1f MB in %llu ops, other %llu ops; "
                 "most samples (%d of %d) in: %s",
                 dur, (io1.ReadTransferCount - io0.ReadTransferCount) / 1048576.0, io1.ReadOperationCount - io0.ReadOperationCount,
                 (io1.WriteTransferCount - io0.WriteTransferCount) / 1048576.0, io1.WriteOperationCount - io0.WriteOperationCount,
                 io1.OtherOperationCount - io0.OtherOperationCount, best, samples, top.empty() ? "-" : top.c_str());
            EnterCriticalSection(&g_cs);
            g_last.seconds = dur;
            strncpy_s(g_last.top, top.c_str(), _TRUNCATE);
            g_last.count++;
            LeaveCriticalSection(&g_cs);
            continue;
        }
        if (hidden) continue; // went to the background (or into a Reset) during the stall: wait, do not sample
        if (now < nextSample || !g_target) continue;
        nextSample = now + (samples < 40 ? 500 : 5000);
        if (!sampleRaw()) continue;
        samples++;
        const std::vector<uintptr_t> f = frames();
        char line[4096];
        int n = snprintf(line, sizeof(line), "  sample %d at +%.1f s:", samples, (now - stallStart) / 1000.0);
        std::string firstExe, firstAny;
        for (size_t i = 0; i < f.size() && n < static_cast<int>(sizeof(line)) - 300; i++) {
            char d[300];
            describeImpl(f[i], d, sizeof(d));
            n += snprintf(line + n, sizeof(line) - n, "\r\n    #%u %s", static_cast<unsigned>(i), d);
            const Module* m = moduleOf(f[i]);
            if (i == 0) firstAny = d;
            if (m && m->isExe && firstExe.empty()) firstExe = d;
        }
        // group by the innermost frame in the game's exe (the engine function that waits), else the innermost one
        tops[firstExe.empty() ? firstAny : firstExe]++;
        writeLine(line);
    }
    return 0;
}

} // namespace

void start(const wchar_t* folder, const wchar_t* gameFolder, uint32_t exeTimestamp) {
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
    g_logPath = std::wstring(folder ? folder : L"") + L"hangs.log";
    g_gameFolder = gameFolder ? gameFolder : L"";
    g_exeTimestamp = exeTimestamp;
    // keep the log small: start over above 2 MB
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > (2u << 20)))
        DeleteFileW(g_logPath.c_str());
    g_stop = 0;
    if (!g_thread) g_thread = CreateThread(nullptr, 0, &watcher, nullptr, 0, nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    note("---- start %04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

void stop() {
    g_stop = 1;
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_target) {
        CloseHandle(g_target);
        g_target = nullptr;
    }
}

void setEnabled(bool on) { InterlockedExchange(&g_enabled, on ? 1 : 0); }
void setBusy(bool busy) { InterlockedExchange(&g_busy, busy ? 1 : 0); }
void setAssumeFront(bool on) { g_assumeFront = on; }
void setThresholdMs(double ms) { g_thresholdMs = ms < 300 ? 300 : ms; }

void frame(void* window) {
    if (!g_mainTid) InterlockedExchange(&g_mainTid, static_cast<LONG>(GetCurrentThreadId()));
    if (window) g_window = static_cast<HWND>(window);
    InterlockedExchange64(&g_lastFrameUs, static_cast<LONG64>(sys::nowMs() * 1000.0));
}

void setState(const char* name, bool raceRunning) {
    if (!g_csInit) return;
    EnterCriticalSection(&g_cs);
    strncpy_s(g_state, name ? name : "?", _TRUNCATE);
    LeaveCriticalSection(&g_cs);
    InterlockedExchange(&g_race, raceRunning ? 1 : 0);
}

void note(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char line[1100];
    snprintf(line, sizeof(line), "[%8.2f s] %s", secs(), msg);
    writeLine(line);
}

Last last() {
    Last l;
    if (!g_csInit) return l;
    EnterCriticalSection(&g_cs);
    l = g_last;
    LeaveCriticalSection(&g_cs);
    return l;
}

void describe(uintptr_t address, char* out, size_t outSize) {
    if (g_modules.empty()) refreshModules();
    describeImpl(address, out, outSize);
}

} // namespace stall
} // namespace perf
