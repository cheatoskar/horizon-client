#pragma once
// Name matching of the Sunrise login redirect (onlinefix.cpp). Header only and without dependencies, so the unit
// tests (here and in Legacy Essentials, which vendors this file next to onlinefix.cpp) test the same code.
#include <stddef.h>

namespace horizon {

// Nadeo's legacy master box for Sunrise (the same box as game.trackmanianations.com, the ESWC master). Hard-coded:
// the Sunrise names no longer resolve to it. A hosts-file entry for these names keeps working too.
constexpr const char kSunriseMasterIp[] = "91.121.54.245";

// The Sunrise master names the redirect handles. The box picks the site (vhost) by the Host header.
constexpr const char* kSunriseMasterNames[] = {"game.trackmaniasunrise.com", "www.trackmaniasunrise.com"};

// name is one of kSunriseMasterNames (ASCII, case-insensitive, whole name): that entry (lower case, static), else
// nullptr. A trailing dot, a port or anything else around the name does not match.
inline const char* sunriseMasterName(const char* name) {
    if (!name) return nullptr;
    for (const char* want : kSunriseMasterNames) {
        size_t i = 0;
        for (; want[i] && name[i]; i++) {
            char c = name[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != want[i]) break;
        }
        if (!want[i] && !name[i]) return want;
    }
    return nullptr;
}

} // namespace horizon
