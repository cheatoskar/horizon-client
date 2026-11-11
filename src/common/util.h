// Small Windows helpers shared by the host, the plugins' tests and the unit tests.
#pragma once

#include <string>

namespace horizon {

std::string utf8(const std::wstring& w);
std::string utf8(const wchar_t* w);
std::wstring wide(const std::string& s);

bool readFile(const std::wstring& path, std::string& out);
// Writes path.tmp and replaces path with it, so a crash never leaves half a file.
bool writeFileAtomic(const std::wstring& path, const std::string& data);
bool fileExists(const std::wstring& path);
bool dirExists(const std::wstring& path);
// Creates every missing folder of path (a folder path, with or without trailing backslash).
bool ensureDir(const std::wstring& path);
// Folder of a file path, with trailing backslash ("" if none).
std::wstring folderOf(const std::wstring& path);
// Full path of a module's file ("" on error); nullptr = the exe.
std::wstring modulePath(void* module);

// "2026-11-11 21:04:11" / "2026-11-11_21-04-11" (local time).
std::string nowText();
std::string nowFileStamp();

// Lowercase ASCII copy.
std::string lower(std::string s);
// Trims spaces and tabs at both ends.
std::string trim(const std::string& s);
// True for ids made of [a-z0-9-_.], 1..64 chars, not starting with '.' (plugin ids, folder names).
bool validId(const std::string& id);

// SHA-256 of a file as 64 lowercase hex chars. False on any error.
bool sha256File(const std::wstring& path, std::string& hexOut);

} // namespace horizon
