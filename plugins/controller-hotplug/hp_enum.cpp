#include "hp_enum.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

namespace hp {

std::string toUtf8(const std::wstring& w); // hp_game.cpp

namespace {
bool due(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }
bool listed(const std::vector<Attached>& list, const Guid& g) {
    for (const auto& a : list)
        if (sameGuid(a.instance, g)) return true;
    return false;
}
std::string utf8(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) { // names are ASCII in practice; anything else becomes '?' (log only)
        s.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?');
    }
    return s;
}
} // namespace

void Schedule::trigger(uint32_t nowMs) {
    start_ = nowMs;
    next_ = 0;
}

bool Schedule::take(uint32_t nowMs) {
    bool any = false;
    while (next_ < kSteps && due(nowMs, start_ + kAfterMs[next_])) {
        next_++;
        any = true;
    }
    return any;
}

uint32_t Schedule::waitMs(uint32_t nowMs) const {
    if (next_ >= kSteps) return 0xFFFFFFFFu;
    const uint32_t at = start_ + kAfterMs[next_];
    return due(nowMs, at) ? 0 : at - nowMs;
}

const char* typeName(uint32_t devType) {
    switch (devType & 0xff) {
    case 0x14: return "joystick";     // DI8DEVTYPE_JOYSTICK
    case 0x15: return "gamepad";      // DI8DEVTYPE_GAMEPAD
    case 0x16: return "wheel";        // DI8DEVTYPE_DRIVING
    case 0x17: return "flight stick"; // DI8DEVTYPE_FLIGHT
    case 0x18: return "1st person";   // DI8DEVTYPE_1STPERSON
    case 0x19: return "device";       // DI8DEVTYPE_DEVICECTRL
    case 0x1a: return "screen pointer";
    case 0x1b: return "remote";
    case 0x1c: return "supplemental"; // pedals, shifters
    default: return "controller";
    }
}

bool productIds(const Guid& product, uint16_t& vid, uint16_t& pid) {
    static const uint8_t kSig[8] = {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'};
    if (memcmp(product.b + 8, kSig, 8) != 0) return false;
    vid = static_cast<uint16_t>(product.b[0] | (product.b[1] << 8));
    pid = static_cast<uint16_t>(product.b[2] | (product.b[3] << 8));
    return true;
}

std::string shortGuid(const Guid& g) {
    char buf[16];
    sprintf_s(buf, "%02X%02X%02X%02X", g.b[3], g.b[2], g.b[1], g.b[0]);
    return buf;
}

std::string describe(const Attached& a) {
    std::string s = a.name.empty() ? std::string("(no name)") : toUtf8(a.name);
    s += " (";
    s += typeName(a.devType);
    uint16_t vid = 0, pid = 0;
    char buf[48];
    if (productIds(a.product, vid, pid)) {
        sprintf_s(buf, ", VID %04X PID %04X", vid, pid);
        s += buf;
    }
    if (a.xinput) s += ", XInput";
    s += ", id ";
    s += shortGuid(a.instance);
    s += ")";
    return s;
}

Diff diff(const std::vector<Attached>& before, const std::vector<Attached>& after) {
    Diff d;
    for (const auto& a : after)
        if (!listed(before, a.instance)) d.added.push_back(a);
    for (const auto& b : before)
        if (!listed(after, b.instance)) d.gone.push_back(b);
    return d;
}

Hint diagnose(const Environment& e) {
    int xinputListed = 0;
    for (const auto& a : e.lists)
        if (a.xinput) xinputListed++;
    int xinputPads = 0;
    for (int i = 0; i < 4; i++)
        if (e.xinputMask & (1u << i)) xinputPads++;
    if (e.xinputKnown && xinputPads > xinputListed) return Hint::XInputNotListed;
    if (e.xboxUsb > 0 && xinputListed == 0 && xinputPads == 0) return Hint::XboxOff;
    return Hint::None;
}

const char* hintText(Hint h) {
    switch (h) {
    case Hint::XboxOff:
        return "an Xbox controller is plugged in but has no game controller device (switched off or asleep): the game "
               "cannot see it until it is switched on (Xbox button); it is added then";
    case Hint::XInputNotListed:
        return "XInput reports a pad that DirectInput does not list: the game (DirectInput only) cannot see it";
    default: return "";
    }
}

bool isXInputPath(const wchar_t* path) {
    if (!path) return false;
    for (const wchar_t* p = path; *p; p++)
        if ((p[0] == L'I' || p[0] == L'i') && (p[1] == L'G' || p[1] == L'g') && p[2] == L'_' && p > path &&
            (p[-1] == L'&' || p[-1] == L'#'))
            return true;
    return false;
}

std::string idsFromPath(const wchar_t* path) {
    if (!path) return std::string();
    std::string out;
    auto grab = [&](const wchar_t* key) {
        const size_t n = wcslen(key);
        for (const wchar_t* p = path; *p; p++) {
            if (_wcsnicmp(p, key, n) == 0) {
                std::string v;
                for (size_t k = 0; k < 4 && p[n + k]; k++) {
                    wchar_t c = p[n + k];
                    if (c >= L'a' && c <= L'f') c = static_cast<wchar_t>(c - 32);
                    v.push_back(static_cast<char>(c));
                }
                if (!out.empty()) out += " ";
                out += utf8(std::wstring(key, n - 1)) + " " + v; // "VID 045E"
                return;
            }
        }
    };
    grab(L"VID_");
    grab(L"PID_");
    return out;
}

} // namespace hp
