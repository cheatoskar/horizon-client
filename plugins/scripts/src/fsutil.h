// Small file helpers for Horizon Scripts: UTF-8/UTF-16, reading and writing whole files, the file
// snapshot that drives hot reload, and the name check for a script's own data folder (IO namespace).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace hs {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);

// Whole file into *out. False when missing, unreadable or larger than maxBytes.
bool readFile(const std::wstring& path, std::string* out, size_t maxBytes);
// Writes (replaces) a whole file. append: adds to the end instead.
bool writeFile(const std::wstring& path, const std::string& data, bool append = false);
bool fileExists(const std::wstring& path);
bool dirExists(const std::wstring& path);
// Creates the folder and its parents. True when it exists afterwards.
bool makeDirs(const std::wstring& path);

// A script folder's files that matter for loading: info.toml / info.json and every *.as file
// (sub-folders up to 3 levels, hidden files and folders skipped), sorted by relative path.
struct FileStamp {
    std::string rel;     // UTF-8, '/' separated: "main.as", "ui/window.as"
    uint64_t size = 0;
    uint64_t mtime = 0;  // FILETIME as a number
};
std::vector<FileStamp> scriptFiles(const std::wstring& folder);
// One number for a file list (names, sizes, times): changes when any file is added, removed or saved.
uint64_t signature(const std::vector<FileStamp>& files);
// Sub-folder names (UTF-8) of a folder, sorted, hidden ones skipped.
std::vector<std::string> subfolders(const std::wstring& folder);

// A script folder name that becomes the script id: 1-64 chars of [A-Za-z0-9 _.-], no leading dot.
bool validScriptId(const std::string& name);
// A file name inside a script's data folder (IO namespace): segments of [A-Za-z0-9 _.-] separated by '/'
// (at most 4), no empty segment, no "." or ".." segment, no leading/trailing space or dot in a segment,
// 1-128 chars. Rejects drive letters, backslashes, ':' and anything absolute.
bool safeDataName(const std::string& name);

// Files directly in a folder and below it (relative, '/' separated), and their total size.
std::vector<std::string> listFiles(const std::wstring& folder, uint64_t* totalBytes);

} // namespace hs
