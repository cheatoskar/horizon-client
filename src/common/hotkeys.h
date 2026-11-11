// Hotkey combos ("Ctrl+Tab", "F9", "Shift+F11") and the registry with conflict detection.
// Pure logic (no key polling): unit-tested. The host polls the keyboard and calls Registry::press.
#pragma once

#include <string>
#include <vector>

namespace horizon {

struct KeyCombo {
    unsigned vk = 0; // Windows virtual-key code, 0 = none
    bool ctrl = false, shift = false, alt = false;
    bool operator==(const KeyCombo& o) const { return vk == o.vk && ctrl == o.ctrl && shift == o.shift && alt == o.alt; }
    bool operator!=(const KeyCombo& o) const { return !(*this == o); }
    bool empty() const { return vk == 0; }
};

// "Ctrl+Shift+F9", case-insensitive, '+' separated; "" / "None" = empty combo (returns true).
// False for unknown key names or a modifier alone.
bool parseKey(const std::string& text, KeyCombo& out);
std::string keyText(const KeyCombo& k); // "Ctrl+Shift+F9", "" for none
std::string vkName(unsigned vk);        // "F9", "Insert", "A", ""
bool isModifierVk(unsigned vk);

// A key another (non-Horizon) mod in the game uses, for the conflict display.
struct ExternalKey {
    KeyCombo key;
    std::string who; // "TM Vibrant Shaders"
};

struct HotkeyEntry {
    int handle = 0;
    std::string owner;      // "host" or plugin id
    std::string ownerName;
    std::string action;     // stable id, "menu", "save"
    std::string name;       // shown
    KeyCombo defaultKey;
    KeyCombo key;           // current binding
    std::string conflict;   // "" or text
    bool active = true;     // false: loses a conflict with an earlier entry (does not fire)
    void (*callback)(void*) = nullptr;
    void* user = nullptr;
};

class HotkeyRegistry {
public:
    // override: a user binding ("" none stored -> default; "None" -> unbound). Returns the handle.
    int add(const std::string& owner, const std::string& ownerName, const std::string& action, const std::string& name,
            const KeyCombo& defaultKey, const std::string* overrideText, void (*callback)(void*), void* user);
    // False if text doesn't parse. Recomputes conflicts.
    bool rebind(int handle, const std::string& text);
    void setExternal(const std::vector<ExternalKey>& keys);
    // Entries whose key equals combo and that are active (callers fire them).
    std::vector<const HotkeyEntry*> matching(const KeyCombo& combo) const;
    const HotkeyEntry* find(int handle) const;
    HotkeyEntry* find(int handle);
    const std::vector<HotkeyEntry>& entries() const { return entries_; }
    void removeOwner(const std::string& owner);
    // Recomputes conflict texts and active flags: the first entry with a key wins, later ones with the
    // same key are inactive; keys of other mods are marked but stay active.
    void recompute();

private:
    std::vector<HotkeyEntry> entries_;
    std::vector<ExternalKey> external_;
    int next_ = 1;
};

} // namespace horizon
