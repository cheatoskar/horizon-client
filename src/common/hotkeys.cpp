#include "common/hotkeys.h"

#include "common/util.h"

#include <windows.h>

#include <cstdio>

namespace horizon {
namespace {

struct Named {
    unsigned vk;
    const char* name;
};
// Virtual-key codes (winuser.h) with the names shown and parsed.
const Named kNames[] = {
    {0x08, "Backspace"}, {0x09, "Tab"},     {0x0D, "Enter"},    {0x13, "Pause"},     {0x14, "CapsLock"},  {0x1B, "Esc"},
    {0x20, "Space"},     {0x21, "PageUp"},  {0x22, "PageDown"}, {0x23, "End"},       {0x24, "Home"},      {0x25, "Left"},
    {0x26, "Up"},        {0x27, "Right"},   {0x28, "Down"},     {0x2C, "PrintScreen"}, {0x2D, "Insert"},  {0x2E, "Delete"},
    {0x60, "Num0"},      {0x61, "Num1"},    {0x62, "Num2"},     {0x63, "Num3"},      {0x64, "Num4"},      {0x65, "Num5"},
    {0x66, "Num6"},      {0x67, "Num7"},    {0x68, "Num8"},     {0x69, "Num9"},      {0x6A, "NumMultiply"}, {0x6B, "NumPlus"},
    {0x6D, "NumMinus"},  {0x6E, "NumDecimal"}, {0x6F, "NumDivide"}, {0x90, "NumLock"}, {0x91, "ScrollLock"},
    {0xBA, "Semicolon"}, {0xBB, "Equals"},  {0xBC, "Comma"},    {0xBD, "Minus"},     {0xBE, "Period"},    {0xBF, "Slash"},
    {0xC0, "Backquote"}, {0xDB, "LeftBracket"}, {0xDC, "Backslash"}, {0xDD, "RightBracket"}, {0xDE, "Quote"},
};
// Accepted spellings besides the names above.
const Named kAliases[] = {{0x2D, "Ins"}, {0x2E, "Del"}, {0x1B, "Escape"}, {0x0D, "Return"}, {0x21, "PgUp"}, {0x22, "PgDn"},
                          {0xC0, "Tilde"}, {0xC0, "Grave"}};

} // namespace

bool isModifierVk(unsigned vk) {
    return vk == 0x10 || vk == 0x11 || vk == 0x12 || (vk >= 0xA0 && vk <= 0xA5) || vk == 0x5B || vk == 0x5C;
}

std::string vkName(unsigned vk) {
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x70 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, static_cast<char>(vk));
    if ((vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDF) || vk == 0xE2) {
        // punctuation keys: the character the current layout types ('#' on a German keyboard)
        const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7fff;
        if (ch > 0x20 && ch < 0x7f) return std::string(1, static_cast<char>(ch));
    }
    for (const auto& n : kNames)
        if (n.vk == vk) return n.name;
    if (vk == 0) return std::string();
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

// The key that types this character on the current keyboard layout ('#', '+', ...), 0 if none.
static unsigned vkFromChar(char c) {
    const SHORT r = VkKeyScanW(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    if (r == -1) return 0;
    return static_cast<unsigned>(r & 0xff);
}

static unsigned vkFromName(const std::string& raw) {
    const std::string n = lower(trim(raw));
    if (n.empty()) return 0;
    if (n.size() == 1 && !((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= '0' && n[0] <= '9'))) return vkFromChar(n[0]);
    if (n.size() >= 2 && n[0] == 'f' && n.find_first_not_of("0123456789", 1) == std::string::npos) {
        const int f = atoi(n.c_str() + 1);
        if (f >= 1 && f <= 24) return 0x70u + static_cast<unsigned>(f - 1);
    }
    if (n.size() == 1) {
        const char c = n[0];
        if (c >= 'a' && c <= 'z') return static_cast<unsigned>(c - 'a' + 'A');
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c);
    }
    if (n.size() > 2 && n[0] == '0' && n[1] == 'x') {
        const unsigned long v = strtoul(n.c_str(), nullptr, 16);
        if (v > 0 && v < 0xff) return static_cast<unsigned>(v);
    }
    for (const auto& k : kNames)
        if (lower(k.name) == n) return k.vk;
    for (const auto& k : kAliases)
        if (lower(k.name) == n) return k.vk;
    return 0;
}

bool parseKey(const std::string& text, KeyCombo& out) {
    out = KeyCombo{};
    std::string t = trim(text);
    if (t.empty() || lower(t) == "none") return true;
    KeyCombo k;
    // '+' as the key itself: "+" or "Ctrl++"
    if (t == "+" || (t.size() >= 2 && t.compare(t.size() - 2, 2, "++") == 0)) {
        const unsigned vk = vkFromChar('+');
        if (!vk) return false;
        const std::string mods = t == "+" ? std::string() : t.substr(0, t.size() - 2);
        if (!mods.empty() && !parseKey(mods + "+A", k)) return false;
        k.vk = vk;
        out = k;
        return true;
    }
    size_t start = 0;
    bool haveKey = false;
    while (start <= t.size()) {
        size_t plus = t.find('+', start);
        // "Ctrl++" or "NumPlus": a '+' right after a separator is the key itself; we only accept NumPlus.
        std::string part = t.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        const std::string p = lower(trim(part));
        if (p == "ctrl" || p == "control") k.ctrl = true;
        else if (p == "shift") k.shift = true;
        else if (p == "alt") k.alt = true;
        else {
            if (haveKey) return false; // two keys
            const unsigned vk = vkFromName(p);
            if (!vk || isModifierVk(vk)) return false;
            k.vk = vk;
            haveKey = true;
        }
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    if (!haveKey) return false;
    out = k;
    return true;
}

std::string keyText(const KeyCombo& k) {
    if (k.empty()) return std::string();
    std::string s;
    if (k.ctrl) s += "Ctrl+";
    if (k.shift) s += "Shift+";
    if (k.alt) s += "Alt+";
    return s + vkName(k.vk);
}

int HotkeyRegistry::add(const std::string& owner, const std::string& ownerName, const std::string& action,
                        const std::string& name, const KeyCombo& defaultKey, const std::string* overrideText,
                        void (*callback)(void*), void* user) {
    HotkeyEntry e;
    e.handle = next_++;
    e.owner = owner;
    e.ownerName = ownerName;
    e.action = action;
    e.name = name;
    e.defaultKey = defaultKey;
    e.key = defaultKey;
    if (overrideText) {
        KeyCombo k;
        if (parseKey(*overrideText, k)) e.key = k;
    }
    e.callback = callback;
    e.user = user;
    entries_.push_back(e);
    recompute();
    return e.handle;
}

bool HotkeyRegistry::rebind(int handle, const std::string& text) {
    HotkeyEntry* e = find(handle);
    KeyCombo k;
    if (!e || !parseKey(text, k)) return false;
    e->key = k;
    recompute();
    return true;
}

void HotkeyRegistry::setExternal(const std::vector<ExternalKey>& keys) {
    external_ = keys;
    recompute();
}

std::vector<const HotkeyEntry*> HotkeyRegistry::matching(const KeyCombo& combo) const {
    std::vector<const HotkeyEntry*> out;
    if (combo.empty()) return out;
    for (const auto& e : entries_)
        if (e.active && e.key == combo) out.push_back(&e);
    return out;
}

const HotkeyEntry* HotkeyRegistry::find(int handle) const {
    for (const auto& e : entries_)
        if (e.handle == handle) return &e;
    return nullptr;
}

HotkeyEntry* HotkeyRegistry::find(int handle) {
    for (auto& e : entries_)
        if (e.handle == handle) return &e;
    return nullptr;
}

void HotkeyRegistry::removeOwner(const std::string& owner) {
    for (size_t i = 0; i < entries_.size();) {
        if (entries_[i].owner == owner) entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        else i++;
    }
    recompute();
}

void HotkeyRegistry::recompute() {
    for (size_t i = 0; i < entries_.size(); i++) {
        HotkeyEntry& e = entries_[i];
        e.conflict.clear();
        e.active = true;
        if (e.key.empty()) continue;
        for (size_t j = 0; j < i; j++) {
            const HotkeyEntry& o = entries_[j];
            if (!o.key.empty() && o.key == e.key) {
                e.active = false;
                e.conflict = "taken by " + o.ownerName + " (" + o.name + "): off until you pick another key";
                break;
            }
        }
        if (e.active) {
            for (size_t j = i + 1; j < entries_.size(); j++) {
                const HotkeyEntry& o = entries_[j];
                if (!o.key.empty() && o.key == e.key) {
                    e.conflict = "also wanted by " + o.ownerName + " (" + o.name + ")";
                    break;
                }
            }
        }
        for (const auto& x : external_) {
            if (x.key == e.key) {
                if (!e.conflict.empty()) e.conflict += "; ";
                e.conflict += "also " + x.who;
                break;
            }
        }
    }
}

} // namespace horizon
