// One log file per start: <dataRoot>logs\<yyyy-mm-dd_hh-mm-ss>.log, the oldest beyond 'keep' deleted.
// Also a ring of the last lines for the menu's Log page.
#include "host/host.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

#include "common/util.h"

namespace horizon {
namespace {
std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::wstring g_path;
std::deque<LogEntry> g_ring;
unsigned g_counter = 0;
constexpr size_t kRing = 600;
const char* kLevel[] = {"debug", "info", "WARN", "ERROR"};

void pruneOld(const std::wstring& folder, int keep) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> names;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end()); // names are timestamps
    for (size_t i = 0; i + static_cast<size_t>(keep) < names.size(); i++) DeleteFileW((folder + names[i]).c_str());
}
} // namespace

void logOpen(const std::wstring& folder, int keep) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) return;
    ensureDir(folder);
    pruneOld(folder, keep - 1);
    g_path = folder + wide(nowFileStamp()) + L".log";
    g_file = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

std::wstring logFilePath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_path;
}

void logLineV(int level, const char* owner, const char* fmt, va_list ap) {
    char msg[2048];
    int m = _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    if (m < 0) m = static_cast<int>(strlen(msg));
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2300];
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    const int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%02u:%02u:%02u.%03u  %-5s [%s] %s", t.wHour, t.wMinute, t.wSecond,
                              t.wMilliseconds, kLevel[level], owner ? owner : "?", msg);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE && n > 0) {
        DWORD written = 0;
        WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
        WriteFile(g_file, "\r\n", 2, &written, nullptr);
    }
    g_ring.push_back(LogEntry{level, line});
    while (g_ring.size() > kRing) g_ring.pop_front();
    g_counter++;
}

void logLine(int level, const char* owner, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logLineV(level, owner, fmt, ap);
    va_end(ap);
}

unsigned logSnapshot(unsigned seen, std::vector<LogEntry>& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (seen == g_counter) return seen;
    out.assign(g_ring.begin(), g_ring.end());
    return g_counter;
}

} // namespace horizon
