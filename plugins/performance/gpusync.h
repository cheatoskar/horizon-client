// Cpu/Gpu synchro (CSystemConfigDisplay::GpuSync): names, the ring the engine builds from it, and the edit of the
// 2006 games' text Default.SystemConfig.Gbx. Pure logic here (unit tested in tests\perf_test.cpp); the live part
// (the engine's viewport, SyncGpuInit) is in gpusync.cpp. Facts: trackmania-internals rendering/gpu-sync.md.
#pragma once

#include <cstdint>
#include <string>

namespace perf {
namespace gpusync {

// GpuSync values as the game stores them (ApplySystemConfig's switch): 0 None ... 7 Immediate.
constexpr int kNone = 0, kHalfFrame = 6, kOneFrame = 5, kImmediate = 7;

// The launcher's name of a value ("Immediate", "1 Frame & Half"; "?" outside 0..7).
inline const char* name(int value) {
    static const char* const kNames[8] = {"None", "3 Frames", "2 Frames & Half", "2 Frames", "1 Frame & Half", "1 Frame", "Half a frame", "Immediate"};
    return value >= 0 && value <= 7 ? kNames[value] : "?";
}

// The SyncGpuInit arguments of a value: count (frames waited behind; >= 256 = no ring) and half.
inline void initArgs(int value, uint32_t* count, uint32_t* half) {
    static const uint32_t kCount[8] = {0xffffffffu, 3, 2, 2, 1, 1, 0, 0};
    static const uint32_t kHalf[8] = {0, 0, 1, 0, 1, 0, 1, 0};
    const int v = value >= 0 && value <= 7 ? value : kImmediate;
    *count = kCount[v];
    *half = kHalf[v];
}

// The value from what the viewport holds: ring entries (count + 1, 0 = no ring) and the half flag. -1: no known value.
inline int fromRing(uint32_t entries, bool half) {
    if (entries == 0) return half ? -1 : kNone;
    if (entries > 4) return -1;
    static const int kByFrames[2][4] = {{kImmediate, kOneFrame, 3, 1}, {kHalfFrame, 4, 2, -1}};
    return kByFrames[half ? 1 : 0][entries - 1];
}

enum class Edit { Ok, Unchanged, NotText, NoChunk, BadLayout, BadValue };

inline const char* editText(Edit e) {
    switch (e) {
    case Edit::Ok: return "saved";
    case Edit::Unchanged: return "already set";
    case Edit::NotText: return "not a text GBX";
    case Edit::NoChunk: return "no display chunk";
    case Edit::BadLayout: return "unknown layout";
    case Edit::BadValue: return "value out of range";
    }
    return "?";
}

// Default.SystemConfig.Gbx of ESWC, Sunrise and Original: "GBX" + version + "TUUE", then one value per CRLF line.
// The display chunk: "b013003", "1397442896" (skip marker), its byte count, six values; the second is GpuSync, a
// single digit (so the byte count stays right). Only that one character changes; everything else is kept byte for
// byte. Refused (file untouched) when anything does not look exactly like that. *old: the value found (-1 unknown).
inline Edit setInConfig(const std::string& in, int value, std::string* out, int* old) {
    if (old) *old = -1;
    if (value < 0 || value > 7) return Edit::BadValue;
    if (in.size() < 9 || in.compare(0, 3, "GBX") != 0 || in.compare(5, 4, "TUUE") != 0) return Edit::NotText;
    const std::string tag = "\r\nb013003\r\n";
    const size_t at = in.find(tag);
    if (at == std::string::npos) return Edit::NoChunk;
    if (in.find(tag, at + 1) != std::string::npos) return Edit::BadLayout; // two display chunks: not touched
    // lines after the chunk id
    size_t pos = at + tag.size();
    auto line = [&](std::string* s) {
        const size_t e = in.find("\r\n", pos);
        if (e == std::string::npos) return false;
        *s = in.substr(pos, e - pos);
        pos = e + 2;
        return true;
    };
    std::string skip, bytes;
    if (!line(&skip) || skip != "1397442896" || !line(&bytes) || bytes.empty() || bytes.size() > 4) return Edit::BadLayout;
    for (char c : bytes)
        if (c < '0' || c > '9') return Edit::BadLayout;
    const size_t want = static_cast<size_t>(std::stoul(bytes));
    const size_t valuesAt = pos;
    std::string v[6];
    size_t gpuAt = 0;
    for (int i = 0; i < 6; i++) {
        if (i == 1) gpuAt = pos;
        if (!line(&v[i])) return Edit::BadLayout;
    }
    if (pos - valuesAt != want) return Edit::BadLayout; // six values must be the whole chunk
    auto isBool = [](const std::string& s) { return s == "True" || s == "False"; };
    if (!isBool(v[0]) || !isBool(v[2]) || v[1].size() != 1 || v[1][0] < '0' || v[1][0] > '7') return Edit::BadLayout;
    if (old) *old = v[1][0] - '0';
    if (v[1][0] - '0' == value) {
        if (out) *out = in;
        return Edit::Unchanged;
    }
    if (out) {
        *out = in;
        (*out)[gpuAt] = static_cast<char>('0' + value);
    }
    return Edit::Ok;
}

// The GpuSync value in such a file (-1: not found or another layout).
inline int readFromConfig(const std::string& in) {
    int old = -1;
    setInConfig(in, kImmediate, nullptr, &old);
    return old;
}

} // namespace gpusync
} // namespace perf
