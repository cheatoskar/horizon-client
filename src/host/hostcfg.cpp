// The host's own settings (settings\host.json) and the hotkey registry: polling, conflicts, rebinding.
#include "host/host.h"

#include <psapi.h>

#include <mutex>

#include "common/hotkeys.h"
#include "common/settings.h"
#include "common/util.h"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "version.lib")

namespace horizon {
namespace {
SettingsFile g_host;
HotkeyRegistry g_keys;
std::recursive_mutex g_keysMutex;
bool g_prev[256] = {};
int g_captureHandle = 0;
bool g_captureArmed = false; // all keys were up once since the capture started
bool g_captureHeld[256] = {};

bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
bool g_fromPad[256] = {}; // this navigation key is down because of the number pad

bool navKey(unsigned vk) {
    return vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_LEFT ||
           vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_CLEAR;
}

std::string fileVersion(const std::wstring& path) {
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return std::string();
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi)
        return std::string();
    char buf[48];
    snprintf(buf, sizeof(buf), "%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
    return buf;
}

std::string moduleDescription(HMODULE m) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(m, path, MAX_PATH)) return std::string();
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return std::string();
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) return std::string();
    struct Lang {
        WORD lang, cp;
    }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) || len < sizeof(Lang))
        return std::string();
    wchar_t key[64];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].cp);
    wchar_t* text = nullptr;
    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&text), &len) || !text) return std::string();
    return utf8(text);
}
} // namespace

void hostConfigLoad() {
    const std::wstring path = state().dataRoot + L"settings\\host.json";
    if (!g_host.load(path)) HWARN("settings: %s is not a JSON object, using defaults", utf8(path).c_str());
}

std::string hostSetting(const std::string& key, const std::string& fallback) { return g_host.getString(key, fallback); }
int hostSettingInt(const std::string& key, int fallback) { return g_host.getInt(key, fallback); }
void hostSetSetting(const std::string& key, const std::string& value) { g_host.setString(key, value); }
void hostSetSettingInt(const std::string& key, int value) { g_host.setNumber(key, value); }

void hostConfigSave(bool force) {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (!force && now - last < 1000) return;
    last = now;
    if (g_host.dirty() && !g_host.saveIfDirty()) HWARN("settings: could not write host.json");
}

int hotkeyAdd(const std::string& owner, const std::string& ownerName, const std::string& action, const std::string& name,
              const char* defaultKey, void (*callback)(void*), void* user) {
    KeyCombo def;
    if (defaultKey && *defaultKey && !parseKey(defaultKey, def)) {
        HWARN("hotkey: %s/%s default key \"%s\" not understood: no key", owner.c_str(), action.c_str(), defaultKey);
        def = KeyCombo{};
    }
    const std::string settingKey = "hotkey." + owner + "." + action;
    const bool hasOverride = g_host.has(settingKey);
    const std::string overrideText = g_host.getString(settingKey, "");
    std::lock_guard<std::recursive_mutex> lock(g_keysMutex);
    const int handle = g_keys.add(owner, ownerName, action, name, def, hasOverride ? &overrideText : nullptr, callback, user);
    const HotkeyEntry* e = g_keys.find(handle);
    HLOG("hotkey: %s \"%s\" on %s%s%s", ownerName.c_str(), name.c_str(), e && !e->key.empty() ? keyText(e->key).c_str() : "no key",
         e && !e->conflict.empty() ? " - " : "", e ? e->conflict.c_str() : "");
    return handle;
}

std::string hotkeyText(int handle) {
    std::lock_guard<std::recursive_mutex> lock(g_keysMutex);
    const HotkeyEntry* e = g_keys.find(handle);
    return e ? keyText(e->key) : std::string();
}

bool hotkeyRebind(int handle, const std::string& text) {
    std::lock_guard<std::recursive_mutex> lock(g_keysMutex);
    HotkeyEntry* e = g_keys.find(handle);
    if (!e) return false;
    const std::string owner = e->owner, action = e->action;
    if (!g_keys.rebind(handle, text)) return false;
    const HotkeyEntry* now = g_keys.find(handle);
    const std::string stored = now->key.empty() ? "None" : keyText(now->key);
    g_host.setString("hotkey." + owner + "." + action, stored);
    HLOG("hotkey: %s/%s now %s%s%s", owner.c_str(), action.c_str(), stored.c_str(), now->conflict.empty() ? "" : " - ",
         now->conflict.c_str());
    return true;
}

void hotkeysRemoveOwner(const std::string& owner) {
    std::lock_guard<std::recursive_mutex> lock(g_keysMutex);
    g_keys.removeOwner(owner);
}

std::vector<HotkeyRow> hotkeyRows() {
    std::lock_guard<std::recursive_mutex> lock(g_keysMutex);
    std::vector<HotkeyRow> rows;
    for (const auto& e : g_keys.entries()) rows.push_back(HotkeyRow{e.handle, e.owner, e.ownerName, e.name, keyText(e.key), e.conflict});
    return rows;
}

void hotkeysPoll(bool foreground, bool typing, bool menuIsOpen) {
    (void)menuIsOpen;
    struct Fire {
        void (*cb)(void*);
        void* user;
    };
    std::vector<Fire> fire;
    {
        std::lock_guard<std::recursive_mutex> lock(g_keysMutex);
        bool seen[256] = {};
        for (const auto& e : g_keys.entries()) {
            if (e.key.empty() || e.key.vk >= 256) continue;
            const unsigned vk = e.key.vk;
            if (seen[vk]) continue;
            seen[vk] = true;
            const bool now = down(static_cast<int>(vk));
            const bool edge = now && !g_prev[vk];
            g_prev[vk] = now;
            if (!edge || !foreground || g_captureHandle) continue;
            if (navKey(vk) && g_fromPad[vk]) continue;
            if (typing && !(vk >= VK_F1 && vk <= VK_F24)) continue;
            KeyCombo combo;
            combo.vk = vk;
            combo.ctrl = down(VK_CONTROL);
            combo.shift = down(VK_SHIFT);
            combo.alt = down(VK_MENU);
            for (const HotkeyEntry* m : g_keys.matching(combo))
                if (m->callback) fire.push_back(Fire{m->callback, m->user});
        }
    }
    for (const auto& f : fire) f.cb(f.user); // outside the lock: callbacks may register or rebind
}

void hotkeysKeyMessage(UINT msg, WPARAM vk, LPARAM lparam) {
    if (vk >= 256 || !navKey(static_cast<unsigned>(vk))) return;
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) g_fromPad[vk] = (lparam & (1 << 24)) == 0; // not extended = number pad
    else if (msg == WM_KEYUP || msg == WM_SYSKEYUP) g_fromPad[vk] = false;
}

void keyCaptureBegin(int handle) {
    g_captureHandle = handle;
    g_captureArmed = false;
    for (int vk = 0; vk < 256; vk++) g_captureHeld[vk] = down(vk);
}

bool keyCaptureActive() { return g_captureHandle != 0; }

bool keyCapturePoll(int handle, std::string& out) {
    if (g_captureHandle != handle) return false;
    for (int vk = 8; vk < 256; vk++) {
        if (isModifierVk(static_cast<unsigned>(vk)) || vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 ||
            vk == VK_XBUTTON2)
            continue;
        const bool now = down(vk);
        const bool edge = now && !g_captureHeld[vk];
        g_captureHeld[vk] = now;
        if (!edge) continue;
        g_captureHandle = 0;
        if (vk == VK_ESCAPE) {
            out.clear();
            dinputHoldUntilReleased(VK_ESCAPE);
            return true;
        }
        KeyCombo combo;
        combo.vk = static_cast<unsigned>(vk);
        combo.ctrl = down(VK_CONTROL);
        combo.shift = down(VK_SHIFT);
        combo.alt = down(VK_MENU);
        out = keyText(combo);
        // the key itself must not reach the hotkeys this frame
        if (vk < 256) g_prev[vk] = true;
        return true;
    }
    return false;
}

// Keys of other mods loaded in this process (not Horizon plugins), for the conflict display.
void hotkeysDetectExternal() {
    std::vector<ExternalKey> ext;
    auto add = [&](const char* key, const char* who) {
        KeyCombo k;
        if (parseKey(key, k)) ext.push_back(ExternalKey{k, who});
    };
    std::vector<OtherMod>& others = state().otherMods;
    others.clear();
    auto other = [&](const std::string& name, const std::wstring& path, const char* kind, const char* keys, const char* note) {
        const std::string p = utf8(path);
        others.push_back(OtherMod{name, fileVersion(path), p.substr(p.find_last_of('\\') + 1), kind, keys ? keys : "", note ? note : ""});
    };
    if (state().chainPresent) {
        const std::wstring chain = state().game.exeFolder + L"dinput8.chain.dll";
        if (state().chainIsAutosave) other("ESWC Autosave (old loader)", chain, "dinput8 chain", "F9 (save now)", "Saves replays itself: the Autosave plugin stays off.");
        else other("dinput8.chain.dll", chain, "dinput8 chain", nullptr, "An earlier dinput8.dll, loaded before the system one.");
    }
    const std::string gameDir = lower(utf8(state().game.exeFolder));
    HMODULE mods[512];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        const DWORD n = needed / sizeof(HMODULE) < 512 ? needed / sizeof(HMODULE) : 512;
        for (DWORD i = 0; i < n; i++) {
            wchar_t path[MAX_PATH];
            if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
            const std::wstring wpath = path;
            const std::string full = lower(utf8(path));
            const std::string file = full.substr(full.find_last_of('\\') + 1);
            const std::string desc = lower(moduleDescription(mods[i]));
            const bool inGame = full.rfind(gameDir, 0) == 0 && full.find('\\', gameDir.size()) == std::string::npos;
            static const char kProducts[] = "\\tmloader\\database\\tmforever\\products\\";
            const size_t prod = full.find(kProducts);
            if (file == "tmvibrantshaders.dll" || desc.find("vibrant shaders") != std::string::npos) {
                state().tmvsLoaded = true;
                for (const char* k : {"F5", "F6", "F7", "F8", "F12", "Shift+F6", "F3", "Ctrl+F3", "Ctrl+Shift+F3"}) add(k, "TM Vibrant Shaders");
                other("TM Vibrant Shaders", wpath, file == "d3d9.dll" ? "d3d9 proxy" : "TMLoader mod", "F5-F8, F12 (menu F8), clips F3",
                      "Keeps its own Direct3D hook and menu.");
                HLOG("other mods: TM Vibrant Shaders is loaded (%s): it keeps its own Direct3D hook", utf8(path).c_str());
            } else if (file.find("clips") != std::string::npos) {
                for (const char* k : {"F3", "Ctrl+F3", "Ctrl+Shift+F3"}) add(k, "TM Clips");
                other("TM Clips", wpath, "loaded DLL", "F3, Ctrl+F3", nullptr);
            } else if (file == "eswc_autosave.dll") {
                add("F9", "ESWC Autosave (old loader)");
                other("ESWC Autosave (old)", wpath, "loaded DLL", "F9", "Saves replays itself.");
            } else if (file == "legacy_randomizer.dll") {
                for (const char* k : {"F11", "Shift+F11", "Ctrl+F11"}) add(k, "Legacy Randomizer (spike)");
                other("Legacy Randomizer (test build)", wpath, "loaded DLL", "F11, Shift+F11, Ctrl+F11", nullptr);
            } else if (file == "tmcomppatch.dll") {
                state().competitionPatch = true;
                other("Competition Patch", wpath, "TMLoader mod", nullptr, "Checks runs on its server: Horizon never touches input, timing or replays.");
                HLOG("other mods: Competition Patch is loaded: no input blocking, solo-only plugins pause while a run goes");
            } else if (file == "tminterface.dll") {
                other("TMInterface", wpath, "TMLoader mod", "its console key", nullptr);
            } else if (file == "centerspeed.dll") {
                other("CenterSpeed", wpath, "TMLoader mod", nullptr, "Hooks Direct3D itself (kiero/MinHook).");
            } else if (file == "twinkiemodloader.dll") {
                other("Twinkie", wpath, "TMLoader mod", nullptr, "Has its own overlay.");
            } else if (file == "tmunlimiter.dll") {
                other("TMUnlimiter", wpath, "TMLoader mod", nullptr, nullptr);
            } else if (prod != std::string::npos) {
                // any other TMLoader product: ...\products\<Name>\<version>\x.dll
                const size_t a = prod + sizeof(kProducts) - 1;
                const size_t b = full.find('\\', a);
                const std::string u = utf8(path);
                const std::string name = b != std::string::npos ? u.substr(a, b - a) : file;
                if (name != "coremod" && name != "CoreMod" && name != "TmForever" && name != "Horizon Host" && file != "lua54.dll" && file != "modloader.dll")
                    other(name, wpath, "TMLoader mod", nullptr, nullptr);
            } else if (inGame && file == "d3d9.dll") {
                other("Direct3D wrapper (d3d9.dll)", wpath, "d3d9 proxy", nullptr, "Lies in the game folder; Horizon hooks the device it hands out.");
            }
        }
    }
    std::lock_guard<std::recursive_mutex> lock(g_keysMutex);
    g_keys.setExternal(ext);
}

} // namespace horizon
