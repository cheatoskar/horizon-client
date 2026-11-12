// The host's own settings (settings\host.json) and the hotkey registry: polling, conflicts, rebinding.
#include "host/host.h"

#include <psapi.h>
#include <shlobj.h>

#include <algorithm>
#include <mutex>

#include "common/hotkeys.h"
#include "common/settings.h"
#include "common/setup.h"
#include "common/util.h"
#include "host/panels.h"
#include "host/plugins.h"

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

// ---- the shared setup ---------------------------------------------------------------------------------------
// settings\host.json is this game's own file. With the shared setup on, every key that is not per game
// (common/setup.h) is read from and written to <setup root>shared\host.json instead, and modules made for
// every game keep their settings there too (shared\<id>.json). The switch itself (setup.shared) and the
// game's own copies of the shared keys stay in settings\host.json, so switching off brings them back.
namespace {
SettingsFile g_shared;
std::wstring g_setupRoot;     // ...\Horizon Client\ with trailing backslash; "" = no shared setup (test program)
bool g_sharedActive = false;  // this start reads the shared setup
bool g_newInstall = false;    // no settings\host.json at this start
bool g_setupChanged = false;  // copied, reset or imported this start: restart to apply

bool gameSpecificPlugin(const std::string& id, void*) {
    const Plugin* p = findPlugin(id);
    return p && setup::pluginGameSpecific(p->manifest.games);
}
bool keyIsLocal(const std::string& key, void*) { return setup::isLocalHostKey(key, &gameSpecificPlugin, nullptr); }
bool keyIsShared(const std::string& key, void* u) { return !keyIsLocal(key, u); }
bool keyIsExportable(const std::string& key, void*) { return setup::isExportableHostKey(key); }
bool keyIsLocalExportable(const std::string& key, void* u) { return keyIsLocal(key, u) && setup::isExportableHostKey(key); }
bool keyIsSharedExportable(const std::string& key, void* u) { return keyIsShared(key, u) && setup::isExportableHostKey(key); }

SettingsFile& storeFor(const std::string& key) { return g_sharedActive && !keyIsLocal(key, nullptr) ? g_shared : g_host; }

std::wstring sharedDir() { return g_setupRoot.empty() ? std::wstring() : g_setupRoot + L"shared\\"; }
std::wstring localSettingsDir() { return state().dataRoot + L"settings\\"; }

std::wstring setupRootFolder() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"HORIZON_SETUP_DIR", buf, MAX_PATH); // tests: a temp folder
    if (n > 0 && n < MAX_PATH) {
        std::wstring r = buf;
        if (r.back() != L'\\') r += L'\\';
        return r;
    }
    if (state().loader == LoaderKind::Test) return std::wstring(); // tests never touch the player's AppData
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf)) || !buf[0]) return std::wstring();
    return std::wstring(buf) + L"\\Horizon Client\\";
}

// The plugins whose settings follow the shared setup (made for every game).
bool pluginShared(const Plugin& p) { return !setup::pluginGameSpecific(p.manifest.games); }
} // namespace

void hostConfigLoad() {
    const std::wstring path = localSettingsDir() + L"host.json";
    const bool existed = fileExists(path);
    g_newInstall = !existed;
    if (!g_host.load(path)) HWARN("settings: %s is not a JSON object, using defaults", utf8(path).c_str());
    g_setupRoot = setupRootFolder();
    if (g_setupRoot.empty()) {
        HLOG("setup: this game's own (no shared setup in the test program)");
        return;
    }
    if (!g_host.has("setup.shared")) {
        // a new install starts on the shared setup; an existing one keeps its own until the player switches
        g_host.setNumber("setup.shared", setup::sharedDefault(existed));
    }
    g_sharedActive = g_host.getInt("setup.shared", 0) != 0;
    if (g_sharedActive) {
        g_shared.setMerging(true); // several games may write it
        if (!g_shared.load(sharedDir() + L"host.json")) HWARN("setup: shared host.json is not a JSON object, using defaults");
        HLOG("setup: shared (%s), %zu keys", utf8(sharedDir()).c_str(), g_shared.keys().size());
    } else {
        HLOG("setup: this game's own%s", existed ? "" : " (new install)");
    }
}

std::string hostSetting(const std::string& key, const std::string& fallback) { return storeFor(key).getString(key, fallback); }
int hostSettingInt(const std::string& key, int fallback) { return storeFor(key).getInt(key, fallback); }
void hostSetSetting(const std::string& key, const std::string& value) { storeFor(key).setString(key, value); }
void hostSetSettingInt(const std::string& key, int value) { storeFor(key).setNumber(key, value); }

void hostConfigSave(bool force) {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (!force && now - last < 1000) return;
    last = now;
    if (g_host.dirty() && !g_host.saveIfDirty()) HWARN("settings: could not write host.json");
    if (g_sharedActive && g_shared.dirty() && !g_shared.saveIfDirty()) HWARN("setup: could not write the shared host.json");
}

bool hostConfigNewInstall() { return g_newInstall; }

std::wstring pluginSettingsPath(const Plugin& p, bool* shared) {
    const bool s = g_sharedActive && pluginShared(p);
    if (shared) *shared = s;
    return (s ? sharedDir() : localSettingsDir()) + wide(p.id) + L".json";
}

SetupStatus setupStatus() {
    SetupStatus st;
    st.available = !g_setupRoot.empty();
    st.active = g_sharedActive;
    st.wanted = g_host.getInt("setup.shared", 0) != 0;
    st.restartNeeded = st.available && (st.wanted != st.active || g_setupChanged);
    st.folder = g_setupRoot;
    if (st.available) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g_setupRoot + L"setups\\*.json").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) st.exports.push_back(utf8(fd.cFileName));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(st.exports.rbegin(), st.exports.rend()); // newest first (the names hold the time)
    }
    return st;
}

namespace {
// Copies this game's own setup (settings\host.json and settings\<id>.json) over the shared one. With the shared
// setup active the copy also goes into what this start holds, so nothing older is written back later.
void copyGameToShared() {
    SettingsFile fileHost;
    SettingsFile* dst = &g_shared;
    if (!g_sharedActive) {
        fileHost.load(sharedDir() + L"host.json");
        dst = &fileHost;
    }
    setup::copyKeys(g_host, *dst, &keyIsShared, nullptr, true);
    if (!dst->saveIfDirty()) HWARN("setup: could not write the shared host.json");
    for (auto& up : plugins()) {
        Plugin& p = *up;
        if (!pluginShared(p)) continue;
        const std::wstring localPath = localSettingsDir() + wide(p.id) + L".json";
        const std::wstring sharedPath = sharedDir() + wide(p.id) + L".json";
        SettingsFile fileSrc, fileDst;
        const SettingsFile* src = &fileSrc;
        if (!g_sharedActive && p.settings.path() == localPath) src = &p.settings; // loaded: what this start holds
        else fileSrc.load(localPath);
        SettingsFile* to = &fileDst;
        if (g_sharedActive && p.settings.path() == sharedPath) to = &p.settings;
        else fileDst.load(sharedPath);
        setup::copyKeys(*src, *to, nullptr, nullptr, true);
        if (!to->saveIfDirty()) logLine(HORIZON_LOG_WARN, p.id.c_str(), "setup: shared settings could not be written");
    }
}
} // namespace

bool setupSetShared(bool on) {
    if (g_setupRoot.empty()) return false;
    if (on && !g_sharedActive && !fileExists(sharedDir() + L"host.json")) {
        // the first game switched on: its own setup becomes the shared one
        panelsSaveAll();
        copyGameToShared();
        HLOG("setup: shared setup made from this game");
    }
    g_host.setNumber("setup.shared", on ? 1 : 0);
    hostConfigSave(true);
    HLOG("setup: shared %s (applies after a restart)", on ? "on" : "off");
    return true;
}

bool setupCopyFromGame() {
    if (g_setupRoot.empty()) return false;
    if (!g_sharedActive) panelsSaveAll(); // anchors for the widgets of this game
    hostConfigSave(true);
    pluginsSaveSettings(true);
    copyGameToShared();
    if (g_sharedActive) g_setupChanged = true;
    HLOG("setup: this game's setup copied to the shared setup");
    return true;
}

bool setupReset() {
    if (g_setupRoot.empty()) return false;
    const std::wstring dir = sharedDir();
    if (g_sharedActive) {
        g_shared.clear();
        if (!g_shared.saveIfDirty()) HWARN("setup: could not write the shared host.json");
        g_setupChanged = true;
    } else {
        DeleteFileW((dir + L"host.json").c_str());
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || _wcsicmp(fd.cFileName, L"host.json") == 0) continue;
            const std::wstring path = dir + fd.cFileName;
            bool held = false;
            for (auto& up : plugins()) {
                if (up->settings.path() == path) {
                    up->settings.clear(); // this start holds it: write it empty, nothing old comes back
                    up->settings.saveIfDirty();
                    held = true;
                }
            }
            if (!held) DeleteFileW(path.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    HLOG("setup: shared setup reset");
    return true;
}

bool setupExport(std::string& fileName, std::string& err) {
    if (!g_setupRoot.empty()) panelsSaveAll();
    // the setup in use: this game's own keys, and with the shared setup on the shared ones from there
    SettingsFile view;
    setup::copyKeys(g_host, view, g_sharedActive ? &keyIsLocalExportable : &keyIsExportable, nullptr, false);
    if (g_sharedActive) setup::copyKeys(g_shared, view, &keyIsSharedExportable, nullptr, false);
    std::vector<setup::PluginSettings> ps;
    for (auto& up : plugins()) {
        Plugin& p = *up;
        const std::wstring path = pluginSettingsPath(p, nullptr);
        if (p.settings.path() == path) {
            ps.push_back(setup::PluginSettings{p.id, p.settings.toText()});
        } else {
            SettingsFile f;
            f.load(path);
            ps.push_back(setup::PluginSettings{p.id, f.toText()});
        }
    }
    const std::wstring folder = (g_setupRoot.empty() ? state().dataRoot : g_setupRoot) + L"setups\\";
    fileName = "setup-" + nowFileStamp() + ".json";
    ensureDir(folder);
    if (!writeFileAtomic(folder + wide(fileName), setup::exportText(view.toText(), ps, state().game.name, kHostVersion))) {
        err = "could not write " + utf8(folder) + fileName;
        HWARN("setup: %s", err.c_str());
        return false;
    }
    HLOG("setup: exported to %s%s (%zu modules)", utf8(folder).c_str(), fileName.c_str(), ps.size());
    return true;
}

bool setupImport(const std::string& fileName, std::string& err) {
    if (g_setupRoot.empty() || fileName.find_first_of("\\/:") != std::string::npos) {
        err = "not available";
        return false;
    }
    std::string text;
    if (!readFile(g_setupRoot + L"setups\\" + wide(fileName), text)) {
        err = "file not found";
        return false;
    }
    setup::Imported im;
    if (!setup::importText(text, im, err)) {
        HWARN("setup: %s not imported: %s", fileName.c_str(), err.c_str());
        return false;
    }
    SettingsFile in;
    in.loadText(im.hostJson);
    for (const std::string& key : in.keys()) storeFor(key).copyKey(in, key);
    size_t modules = 0;
    for (const setup::PluginSettings& ps : im.plugins) {
        Plugin* p = findPlugin(ps.id);
        if (!p) continue; // not installed in this game
        SettingsFile from, file;
        from.loadText(ps.json);
        const std::wstring path = pluginSettingsPath(*p, nullptr);
        SettingsFile* to = p->settings.path() == path ? &p->settings : &file;
        if (to == &file) file.load(path);
        setup::copyKeys(from, *to, nullptr, nullptr, true);
        to->saveIfDirty();
        modules++;
    }
    hostConfigSave(true);
    g_setupChanged = true;
    HLOG("setup: imported %s (%zu host keys, %zu modules, made on %s)", fileName.c_str(), in.keys().size(), modules,
         im.game.empty() ? "?" : im.game.c_str());
    return true;
}

int hotkeyAdd(const std::string& owner, const std::string& ownerName, const std::string& action, const std::string& name,
              const char* defaultKey, void (*callback)(void*), void* user) {
    KeyCombo def;
    if (defaultKey && *defaultKey && !parseKey(defaultKey, def)) {
        HWARN("hotkey: %s/%s default key \"%s\" not understood: no key", owner.c_str(), action.c_str(), defaultKey);
        def = KeyCombo{};
    }
    const std::string settingKey = "hotkey." + owner + "." + action;
    const bool hasOverride = storeFor(settingKey).has(settingKey);
    const std::string overrideText = storeFor(settingKey).getString(settingKey, "");
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
    hostSetSetting("hotkey." + owner + "." + action, stored);
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
        if (state().chainIsAutosave) other("ESWC Autosave (old loader)", chain, "dinput8 chain", "F9 (save now)", "Saves replays itself: the Autosave module is paused.");
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
