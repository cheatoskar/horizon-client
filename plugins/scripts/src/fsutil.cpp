#include "fsutil.h"

#include <windows.h>

#include <algorithm>

namespace hs {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

bool readFile(const std::wstring& path, std::string* out, size_t maxBytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) != 0 && static_cast<uint64_t>(size.QuadPart) <= maxBytes;
    if (ok) {
        out->assign(static_cast<size_t>(size.QuadPart), '\0');
        DWORD got = 0;
        ok = size.QuadPart == 0 || (ReadFile(h, &(*out)[0], static_cast<DWORD>(size.QuadPart), &got, nullptr) && got == size.QuadPart);
    }
    CloseHandle(h);
    return ok;
}

bool writeFile(const std::wstring& path, const std::string& data, bool append) {
    HANDLE h = CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = data.empty() || (WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size());
    CloseHandle(h);
    return ok;
}

bool fileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool makeDirs(const std::wstring& path) {
    if (path.empty()) return false;
    if (dirExists(path)) return true;
    std::wstring p = path;
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t cut = p.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 2) makeDirs(p.substr(0, cut));
    CreateDirectoryW(p.c_str(), nullptr);
    return dirExists(p);
}

namespace {

bool hidden(const WIN32_FIND_DATAW& fd) { return fd.cFileName[0] == L'.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN); }

bool endsWithNoCase(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

void collect(const std::wstring& root, const std::wstring& rel, int depth, std::vector<FileStamp>* out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (hidden(fd)) continue;
        const std::wstring name = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 3) collect(root, rel + name + L"\\", depth + 1, out);
            continue;
        }
        const bool manifest = depth == 0 && (_wcsicmp(name.c_str(), L"info.toml") == 0 || _wcsicmp(name.c_str(), L"info.json") == 0);
        if (!manifest && !endsWithNoCase(name, L".as")) continue;
        FileStamp f;
        f.rel = narrow(rel + name);
        std::replace(f.rel.begin(), f.rel.end(), '\\', '/');
        f.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        f.mtime = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        out->push_back(f);
        if (out->size() > 512) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void collectAll(const std::wstring& root, const std::wstring& rel, int depth, std::vector<std::string>* out, uint64_t* total) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 4) collectAll(root, rel + fd.cFileName + L"\\", depth + 1, out, total);
            continue;
        }
        std::string r = narrow(rel + fd.cFileName);
        std::replace(r.begin(), r.end(), '\\', '/');
        out->push_back(r);
        if (total) *total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd) && out->size() < 4096);
    FindClose(h);
}

} // namespace

std::vector<FileStamp> scriptFiles(const std::wstring& folder) {
    std::vector<FileStamp> v;
    std::wstring root = folder;
    if (!root.empty() && root.back() != L'\\') root += L'\\';
    collect(root, L"", 0, &v);
    std::sort(v.begin(), v.end(), [](const FileStamp& a, const FileStamp& b) { return a.rel < b.rel; });
    return v;
}

uint64_t signature(const std::vector<FileStamp>& files) {
    uint64_t h = 1469598103934665603ull; // FNV-1a
    auto mix = [&h](const void* p, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; i++) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    for (const FileStamp& f : files) {
        mix(f.rel.data(), f.rel.size());
        mix(&f.size, sizeof(f.size));
        mix(&f.mtime, sizeof(f.mtime));
    }
    mix(&h, 1); // empty list != 0
    return h;
}

std::vector<std::string> subfolders(const std::wstring& folder) {
    std::vector<std::string> v;
    std::wstring root = folder;
    if (!root.empty() && root.back() != L'\\') root += L'\\';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || hidden(fd)) continue;
        v.push_back(narrow(fd.cFileName));
    } while (FindNextFileW(h, &fd) && v.size() < 256);
    FindClose(h);
    std::sort(v.begin(), v.end());
    return v;
}

namespace {
bool nameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '.' || c == '-';
}
} // namespace

bool validScriptId(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] == '.') return false;
    for (char c : name)
        if (!nameChar(c)) return false;
    return true;
}

bool safeDataName(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    int segments = 0;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        const std::string seg = name.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (seg.front() == ' ' || seg.back() == ' ' || seg.back() == '.' || seg.front() == '.') return false;
        for (char c : seg)
            if (!nameChar(c)) return false;
        if (++segments > 4) return false;
        start = end + 1;
        if (end == name.size()) break;
    }
    // reserved device names (CON, NUL, COM1 ...) as a whole segment or before the first dot
    for (size_t s = 0; s < name.size();) {
        size_t e = name.find('/', s);
        if (e == std::string::npos) e = name.size();
        std::string base = name.substr(s, e - s);
        base = base.substr(0, base.find('.'));
        std::transform(base.begin(), base.end(), base.begin(), [](char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); });
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
            (base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) && base[3] >= '0' && base[3] <= '9'))
            return false;
        s = e + 1;
    }
    return true;
}

std::vector<std::string> listFiles(const std::wstring& folder, uint64_t* totalBytes) {
    std::vector<std::string> v;
    if (totalBytes) *totalBytes = 0;
    std::wstring root = folder;
    if (!root.empty() && root.back() != L'\\') root += L'\\';
    collectAll(root, L"", 0, &v, totalBytes);
    std::sort(v.begin(), v.end());
    return v;
}

} // namespace hs
