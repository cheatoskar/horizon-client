// Play events for TM Horizon's statistics page (header-only, no host state, unit tested).
// TM Horizon names a folder in profile.json ("stats_dir"); the host appends one JSON line per event to
// <stats_dir>\<yyyy-mm>.jsonl and never reads, rewrites or deletes those files. Spec: tm-hub-mono
// docs/design/host-stats-channel.md, here docs/profile-json.md.
//   {"v":1,"t":1794326446230,"ev":"finish","uid":"...","time_ms":45230,"respawns":0,"local":true,"pb":true,"mode":"solo"}
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include "common/json.h"
#include "common/util.h"
#include "horizon_plugin.h"

namespace horizon {
namespace stats {

inline const char* modeName(uint32_t mode) {
    switch (mode) {
    case HORIZON_MODE_SOLO: return "solo";
    case HORIZON_MODE_NET: return "net";
    case HORIZON_MODE_EDITOR_TEST: return "editor";
    case HORIZON_MODE_REPLAY: return "replay";
    default: return "unknown";
    }
}

// "2026-11.jsonl" for a Unix time in milliseconds (local calendar month).
inline std::string fileName(int64_t tMs) {
    const time_t secs = static_cast<time_t>(tMs / 1000);
    tm lt = {};
    localtime_s(&lt, &secs);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d.jsonl", lt.tm_year + 1900, lt.tm_mon + 1);
    return buf;
}

inline int64_t nowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const int64_t t = (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) / 10000;
}

// Tracks the run and turns host events into lines. "" = nothing to write for this event.
class Recorder {
public:
    // uid: the map's uid (from the event's map info or found by the host); name/author/env for "map".
    std::string line(const HorizonEvent& e, const std::string& uid, const std::string& name, const std::string& author, const std::string& env,
                     int64_t tMs) {
        // replay viewing and the map editor's test drives are not play
        if (e.play_mode == HORIZON_MODE_REPLAY || e.play_mode == HORIZON_MODE_EDITOR_TEST) return std::string();
        std::string o;
        switch (e.type) {
        case HORIZON_EVENT_MAP_LOADED:
            respawns_ = 0;
            o = head(tMs, "map", uid);
            field(o, "name", name);
            if (!author.empty()) field(o, "author", author);
            if (!env.empty()) field(o, "env", env);
            break;
        case HORIZON_EVENT_RACE_START:
            respawns_ = 0;
            o = head(tMs, "start", uid);
            break;
        case HORIZON_EVENT_RESPAWN:
            respawns_ = e.respawns >= 0 ? e.respawns : respawns_ + 1;
            o = head(tMs, "respawn", uid);
            o += ",\"respawns\":" + std::to_string(respawns_);
            break;
        case HORIZON_EVENT_FINISH:
            if (e.time_ms <= 0) return std::string();
            o = head(tMs, "finish", uid);
            o += ",\"time_ms\":" + std::to_string(e.time_ms);
            o += ",\"respawns\":" + std::to_string(e.respawns >= 0 ? e.respawns : respawns_);
            o += std::string(",\"local\":") + (e.is_local_player ? "true" : "false");
            o += std::string(",\"pb\":") + (e.is_pb ? "true" : "false");
            field(o, "mode", modeName(e.play_mode));
            break;
        default: return std::string();
        }
        return o + "}\n";
    }

private:
    int respawns_ = 0;
    static std::string head(int64_t tMs, const char* ev, const std::string& uid) {
        std::string o = "{\"v\":1,\"t\":" + std::to_string(tMs) + ",\"ev\":\"" + ev + "\"";
        if (!uid.empty()) field(o, "uid", uid);
        return o;
    }
    static void field(std::string& o, const char* key, const std::string& v) {
        o += ",\"";
        o += key;
        o += "\":";
        jsonQuote(o, v);
    }
};

// Appends text to <dir>\<file> (creates the folder). False on any error; never throws.
inline bool append(const std::wstring& dir, const std::string& file, const std::string& text) {
    if (dir.empty() || text.empty() || !ensureDir(dir)) return false;
    std::wstring path = dir;
    if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += wide(file);
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD done = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &done, nullptr);
    CloseHandle(h);
    return ok && done == text.size();
}

} // namespace stats
} // namespace horizon
