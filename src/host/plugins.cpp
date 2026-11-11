// Plugin loading (profile.json -> horizon\plugins\<id>\), the per-plugin HorizonHostApi tables, and
// every call into a plugin (each one SEH-guarded: a plugin that crashes is switched off, the game goes on).
#include "host/plugins.h"
#include "host/panels.h"

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>

#include "common/profile.h"
#include "common/util.h"
#include "reflect/reflect.h"

namespace horizon {
namespace {

std::vector<std::unique_ptr<Plugin>> g_plugins;
Profile g_profile;

struct Queued {
    Plugin* plugin;
    HorizonCallback cb;
    void* user;
};
std::mutex g_queueMutex;
std::vector<Queued> g_queue;

Plugin* fromHandle(HorizonPluginHandle h) { return reinterpret_cast<Plugin*>(h); }

// ---------------------------------------------------------------- guarded calls (no C++ objects inside)
bool sehInfo(HorizonPlugin_InfoFn fn, const HorizonPluginInfo** out) {
    __try {
        *out = fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehInit(HorizonPlugin_InitFn fn, const HorizonHostApi* api, int32_t* out) {
    __try {
        *out = fn(api);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehVoid(void(HORIZON_CALL* fn)(void)) {
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehEvent(HorizonPlugin_OnEventFn fn, const HorizonEvent* e) {
    __try {
        fn(e);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehInt(HorizonPlugin_SetEnabledFn fn, int32_t v) {
    __try {
        fn(v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehCallback(HorizonCallback fn, void* user) {
    __try {
        fn(user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehDev2(void(HORIZON_CALL* fn)(void*, void*), void* a, void* b) {
    __try {
        fn(a, b);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehDev3(void(HORIZON_CALL* fn)(void*, void*, void*), void* a, void* b, void* c) {
    __try {
        fn(a, b, c);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool sehDevHr(void(HORIZON_CALL* fn)(void*, void*, int32_t), void* a, void* b, int32_t c) {
    __try {
        fn(a, b, c);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void crashed(Plugin* p, const char* where) {
    p->running = false;
    p->status = ui::PluginStatus::Failed;
    p->statusText = std::string("crashed in ") + where + ": switched off until the next start";
    logLine(HORIZON_LOG_ERROR, p->id.c_str(), "crashed in %s: switched off until the next start", where);
}

// ---------------------------------------------------------------- the API table
void HORIZON_CALL apiLog(HorizonPluginHandle h, int32_t level, const char* fmt, ...) {
    Plugin* p = fromHandle(h);
    if (!fmt) return;
    va_list ap;
    va_start(ap, fmt);
    logLineV(level, p ? p->id.c_str() : "?", fmt, ap);
    va_end(ap);
}
int32_t HORIZON_CALL apiGetString(HorizonPluginHandle h, const char* key, const char* fallback, char* out, uint32_t size) {
    Plugin* p = fromHandle(h);
    if (!p || !key || !out || !size) return 0;
    const std::string v = p->settings.getString(key, fallback ? fallback : "");
    const size_t n = v.size() < size - 1 ? v.size() : size - 1;
    memcpy(out, v.data(), n);
    out[n] = 0;
    return static_cast<int32_t>(n);
}
void HORIZON_CALL apiSetString(HorizonPluginHandle h, const char* key, const char* value) {
    Plugin* p = fromHandle(h);
    if (p && key) p->settings.setString(key, value ? value : "");
}
int32_t HORIZON_CALL apiGetInt(HorizonPluginHandle h, const char* key, int32_t fallback) {
    Plugin* p = fromHandle(h);
    return p && key ? p->settings.getInt(key, fallback) : fallback;
}
void HORIZON_CALL apiSetInt(HorizonPluginHandle h, const char* key, int32_t value) {
    Plugin* p = fromHandle(h);
    if (p && key) p->settings.setNumber(key, value);
}
float HORIZON_CALL apiGetFloat(HorizonPluginHandle h, const char* key, float fallback) {
    Plugin* p = fromHandle(h);
    return p && key ? p->settings.getFloat(key, fallback) : fallback;
}
void HORIZON_CALL apiSetFloat(HorizonPluginHandle h, const char* key, float value) {
    Plugin* p = fromHandle(h);
    if (p && key) p->settings.setNumber(key, value);
}
const char* HORIZON_CALL apiDataFolder(HorizonPluginHandle h) {
    Plugin* p = fromHandle(h);
    return p ? p->dataFolder.c_str() : "";
}
const wchar_t* HORIZON_CALL apiDataFolderW(HorizonPluginHandle h) {
    Plugin* p = fromHandle(h);
    return p ? p->dataFolderW.c_str() : L"";
}
void* HORIZON_CALL apiImguiContext() { return imguiContext(); }
int32_t HORIZON_CALL apiImguiVersion() { return HORIZON_IMGUI_VERSION_NUM; }
void HORIZON_CALL apiImguiAllocators(void** a, void** f, void** u) { imguiAllocators(a, f, u); }
const HorizonStyle* HORIZON_CALL apiStyle() { return hostStyle(); }
const HorizonGameInfo* HORIZON_CALL apiGameInfo() { return &state().info; }
void* HORIZON_CALL apiEnginePointer(uint32_t which) { return game().engine(which); }

struct HotkeyCtx {
    Plugin* plugin;
    HorizonCallback cb;
    void* user;
};
void hotkeyTrampoline(void* raw) {
    HotkeyCtx* c = static_cast<HotkeyCtx*>(raw);
    Plugin* p = c->plugin;
    if (!p->running || !p->enabled) return;
    if (pluginGated(p)) {
        logLine(HORIZON_LOG_INFO, p->id.c_str(), "hotkey ignored: solo only, off while online");
        return;
    }
    if (!sehCallback(c->cb, c->user)) crashed(p, "a hotkey");
}
int32_t HORIZON_CALL apiRegisterHotkey(HorizonPluginHandle h, const char* action, const char* name, const char* key,
                                       HorizonCallback cb, void* user) {
    Plugin* p = fromHandle(h);
    if (!p || !action || !*action || !cb) return 0;
    HotkeyCtx* ctx = new HotkeyCtx{p, cb, user}; // lives as long as the process
    return hotkeyAdd(p->id, p->name, action, name && *name ? name : action, key ? key : "", &hotkeyTrampoline, ctx);
}
const char* HORIZON_CALL apiHotkeyText(HorizonPluginHandle h, int32_t handle) {
    Plugin* p = fromHandle(h);
    if (!p) return "";
    p->hotkeyTextBuf = hotkeyText(handle);
    return p->hotkeyTextBuf.c_str();
}
void HORIZON_CALL apiSubscribe(HorizonPluginHandle h, uint32_t mask) {
    Plugin* p = fromHandle(h);
    if (p) p->eventMask = mask;
}

struct ReplayJob {
    Plugin* plugin;
    std::string path;
};
void postReplaySavedNow(Plugin* p, const std::string& path) {
    HorizonEvent e = {};
    e.size = sizeof(e);
    e.type = HORIZON_EVENT_REPLAY_SAVED;
    e.path = path.c_str();
    e.source = p ? p->id.c_str() : "game";
    dispatchEvent(e);
}
void HORIZON_CALL replayJob(void* raw) {
    ReplayJob* j = static_cast<ReplayJob*>(raw);
    postReplaySavedNow(j->plugin, j->path);
    delete j;
}
void HORIZON_CALL apiPostReplaySaved(HorizonPluginHandle h, const char* path) {
    Plugin* p = fromHandle(h);
    if (!p || !path) return;
    if (GetCurrentThreadId() == state().mainThread) {
        postReplaySavedNow(p, path);
    } else {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queue.push_back(Queued{nullptr, &replayJob, new ReplayJob{p, path}});
    }
}
HorizonResult HORIZON_CALL apiPlayMapFile(HorizonPluginHandle h, const char* path, uint32_t flags) {
    Plugin* p = fromHandle(h);
    if (!p || !path || !*path) return HORIZON_ERR_INVALID_ARG;
    if (isOnlineNow() || runGate()) return HORIZON_ERR_BLOCKED_ONLINE;
    if (GetCurrentThreadId() != state().mainThread) return HORIZON_ERR_WRONG_STATE;
    const HorizonResult r = game().playMapFile(path, flags);
    logLine(HORIZON_LOG_INFO, p->id.c_str(), "PlayMapFile(%s) -> %d", path, r);
    return r;
}
HorizonResult HORIZON_CALL apiReturnToMenu(HorizonPluginHandle h) {
    Plugin* p = fromHandle(h);
    if (!p) return HORIZON_ERR_INVALID_ARG;
    if (isOnlineNow() || runGate()) return HORIZON_ERR_BLOCKED_ONLINE;
    if (GetCurrentThreadId() != state().mainThread) return HORIZON_ERR_WRONG_STATE;
    const HorizonResult r = game().returnToMenu();
    logLine(HORIZON_LOG_INFO, p->id.c_str(), "ReturnToMenu() -> %d", r);
    return r;
}
int32_t HORIZON_CALL apiGameState() { return game().gameState(); }
int32_t HORIZON_CALL apiIsOnline() { return isOnlineNow() ? 1 : 0; }
int32_t HORIZON_CALL apiActionsAllowed(HorizonPluginHandle h) {
    Plugin* p = fromHandle(h);
    return p && p->running && p->enabled && !pluginGated(p) ? 1 : 0;
}
void HORIZON_CALL apiRunOnGameThread(HorizonPluginHandle h, HorizonCallback cb, void* user) {
    Plugin* p = fromHandle(h);
    if (!p || !cb) return;
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_queue.push_back(Queued{p, cb, user});
}
void* HORIZON_CALL apiDevice() { return gameDevice(); }
void HORIZON_CALL apiSetDeviceCallbacks(HorizonPluginHandle h, const HorizonDeviceCallbacks* cb) {
    Plugin* p = fromHandle(h);
    if (!p) return;
    p->dev = HorizonDeviceCallbacks{};
    p->hasDev = false;
    if (!cb) return;
    const size_t n = cb->size < sizeof(HorizonDeviceCallbacks) ? cb->size : sizeof(HorizonDeviceCallbacks);
    memcpy(&p->dev, cb, n);
    p->hasDev = true;
    logLine(HORIZON_LOG_INFO, p->id.c_str(), "device callbacks set");
    if (gameDevice() && p->dev.device_created) {
        if (!sehDev3(p->dev.device_created, p->dev.user, gameDevice(), nullptr)) crashed(p, "device_created");
    }
}
int32_t HORIZON_CALL apiMenuOpen() { return menuOpen() ? 1 : 0; }
void HORIZON_CALL apiRestartNote(HorizonPluginHandle h, const char* why) {
    Plugin* p = fromHandle(h);
    if (!p) return;
    p->restartNote = true;
    p->restartWhy = why ? why : "";
    logLine(HORIZON_LOG_INFO, p->id.c_str(), "applies after a restart: %s", p->restartWhy.c_str());
}
int32_t HORIZON_CALL apiReflectOffset(const char* cls, const char* member) {
    if (!cls || !member || !reflect::ready()) return -1;
    return reflect::offsetOf(cls, member);
}
void* HORIZON_CALL apiReflectMethod(const char* cls, const char* member) {
    if (!cls || !member || !reflect::ready()) return nullptr;
    return reflect::methodOf(cls, member);
}

uint64_t HORIZON_CALL apiCreateTexture(HorizonPluginHandle h, uint32_t w, uint32_t hgt, const void* rgba) {
    Plugin* p = fromHandle(h);
    if (!p || !rgba || !w || !hgt || w > 4096 || hgt > 4096) return 0;
    const uint64_t id = createTextureRgba(w, hgt, rgba);
    if (!id) logLine(HORIZON_LOG_WARN, p->id.c_str(), "create_texture_rgba(%ux%u) failed (no device yet?)", w, hgt);
    return id;
}
void HORIZON_CALL apiReleaseTexture(HorizonPluginHandle, uint64_t id) { releaseTexture(id); }
HorizonResult HORIZON_CALL apiSaveReplay(HorizonPluginHandle h, const char* name) {
    Plugin* p = fromHandle(h);
    if (!p) return HORIZON_ERR_INVALID_ARG;
    if (GetCurrentThreadId() != state().mainThread) {
        logLine(HORIZON_LOG_WARN, p->id.c_str(), "save_replay: called outside the game thread");
        return HORIZON_ERR_WRONG_STATE;
    }
    const std::wstring stem = replayStemFromName(wide(name ? name : ""));
    std::string path, why;
    const HorizonResult r = game().saveReplay(stem, &path, &why);
    if (r == HORIZON_OK) {
        logLine(HORIZON_LOG_INFO, p->id.c_str(), "save_replay: saved %s", path.c_str());
        postReplaySavedNow(p, path);
    } else {
        logLine(HORIZON_LOG_INFO, p->id.c_str(), "save_replay: %d (%s)", r, why.c_str());
    }
    return r;
}

// ---- ABI minor 2: panels, host flags
HorizonResult HORIZON_CALL apiRegisterPanel(HorizonPluginHandle h, const HorizonPanelDef* def) {
    Plugin* p = fromHandle(h);
    if (!p) return HORIZON_ERR_INVALID_ARG;
    const HorizonResult r = panelRegister(p, def);
    if (r != HORIZON_OK) logLine(HORIZON_LOG_WARN, p->id.c_str(), "register_panel: invalid (key [a-z0-9-], title and draw needed)");
    return r;
}
void HORIZON_CALL apiUnregisterPanel(HorizonPluginHandle h, const char* key) {
    if (Plugin* p = fromHandle(h)) panelUnregister(p, key);
}
int32_t HORIZON_CALL apiPanelVisible(HorizonPluginHandle h, const char* key) {
    Plugin* p = fromHandle(h);
    return p ? panelVisible(p, key) : 0;
}
void HORIZON_CALL apiSetHostFlag(HorizonPluginHandle h, uint32_t flag, int32_t on) {
    Plugin* p = fromHandle(h);
    if (!p) return;
    const uint32_t before = p->hostFlags;
    p->hostFlags = on ? (p->hostFlags | flag) : (p->hostFlags & ~flag);
    if (before != p->hostFlags) logLine(HORIZON_LOG_INFO, p->id.c_str(), "host flags 0x%x", p->hostFlags);
}
uint32_t HORIZON_CALL apiHostFlags() { return hostFlagsAll(); }

void fillApi(Plugin* p) {
    HorizonHostApi& a = p->api;
    a = HorizonHostApi{};
    a.abi = HORIZON_ABI;
    a.size = sizeof(HorizonHostApi);
    a.abi_minor = HORIZON_ABI_MINOR;
    a.plugin = reinterpret_cast<HorizonPluginHandle>(p);
    a.log = &apiLog;
    a.get_string = &apiGetString;
    a.set_string = &apiSetString;
    a.get_int = &apiGetInt;
    a.set_int = &apiSetInt;
    a.get_float = &apiGetFloat;
    a.set_float = &apiSetFloat;
    a.data_folder = &apiDataFolder;
    a.data_folder_w = &apiDataFolderW;
    a.imgui_context = &apiImguiContext;
    a.imgui_version = &apiImguiVersion;
    a.imgui_allocators = &apiImguiAllocators;
    a.style = &apiStyle;
    a.game_info = &apiGameInfo;
    a.engine_pointer = &apiEnginePointer;
    a.register_hotkey = &apiRegisterHotkey;
    a.hotkey_text = &apiHotkeyText;
    a.subscribe = &apiSubscribe;
    a.post_replay_saved = &apiPostReplaySaved;
    a.play_map_file = &apiPlayMapFile;
    a.return_to_menu = &apiReturnToMenu;
    a.get_game_state = &apiGameState;
    a.is_online = &apiIsOnline;
    a.actions_allowed = &apiActionsAllowed;
    a.run_on_game_thread = &apiRunOnGameThread;
    a.device = &apiDevice;
    a.set_device_callbacks = &apiSetDeviceCallbacks;
    a.menu_open = &apiMenuOpen;
    a.request_restart_note = &apiRestartNote;
    a.reflect_offset = &apiReflectOffset;
    a.reflect_method = &apiReflectMethod;
    a.create_texture_rgba = &apiCreateTexture;
    a.release_texture = &apiReleaseTexture;
    a.save_replay = &apiSaveReplay;
    a.register_panel = &apiRegisterPanel;
    a.unregister_panel = &apiUnregisterPanel;
    a.panel_visible = &apiPanelVisible;
    a.set_host_flag = &apiSetHostFlag;
    a.host_flags = &apiHostFlags;
}

// ---------------------------------------------------------------- loading
void loadOne(Plugin* p) {
    const std::wstring dll = p->folder + wide(p->manifest.dll);
    if (!fileExists(dll)) {
        p->status = ui::PluginStatus::Failed;
        p->statusText = p->manifest.dll + " missing";
        logLine(HORIZON_LOG_ERROR, p->id.c_str(), "not loaded: %s", p->statusText.c_str());
        return;
    }
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        p->status = ui::PluginStatus::Failed;
        p->statusText = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
        logLine(HORIZON_LOG_ERROR, p->id.c_str(), "not loaded: %s", p->statusText.c_str());
        return;
    }
    p->module = m;
    auto infoFn = reinterpret_cast<HorizonPlugin_InfoFn>(GetProcAddress(m, HORIZON_EXPORT_INFO));
    auto initFn = reinterpret_cast<HorizonPlugin_InitFn>(GetProcAddress(m, HORIZON_EXPORT_INIT));
    p->shutdown = reinterpret_cast<HorizonPlugin_ShutdownFn>(GetProcAddress(m, HORIZON_EXPORT_SHUTDOWN));
    p->drawMenu = reinterpret_cast<HorizonPlugin_DrawMenuFn>(GetProcAddress(m, HORIZON_EXPORT_DRAW_MENU));
    p->drawOverlay = reinterpret_cast<HorizonPlugin_DrawOverlayFn>(GetProcAddress(m, HORIZON_EXPORT_DRAW_OVERLAY));
    p->onEvent = reinterpret_cast<HorizonPlugin_OnEventFn>(GetProcAddress(m, HORIZON_EXPORT_ON_EVENT));
    p->setEnabled = reinterpret_cast<HorizonPlugin_SetEnabledFn>(GetProcAddress(m, HORIZON_EXPORT_SET_ENABLED));
    const HorizonPluginInfo* info = nullptr;
    if (!infoFn || !initFn || !p->shutdown) {
        p->status = ui::PluginStatus::Failed;
        p->statusText = "not a Horizon plugin (exports missing)";
    } else if (!sehInfo(infoFn, &info) || !info) {
        p->status = ui::PluginStatus::Failed;
        p->statusText = "HorizonPlugin_Info failed";
    } else if (info->abi != HORIZON_ABI) {
        p->status = ui::PluginStatus::Failed;
        p->statusText = "made for plugin ABI " + std::to_string(info->abi) + ", this host has " + std::to_string(HORIZON_ABI);
    } else if (!info->id || p->id != info->id) {
        p->status = ui::PluginStatus::Failed;
        p->statusText = std::string("its id \"") + (info->id ? info->id : "") + "\" differs from its folder";
    } else if (state().game.game && !(info->games & state().game.game)) {
        p->status = ui::PluginStatus::WrongGame;
        p->statusText = "not made for this game";
    }
    if (p->status == ui::PluginStatus::Failed || p->status == ui::PluginStatus::WrongGame) {
        logLine(p->status == ui::PluginStatus::Failed ? HORIZON_LOG_ERROR : HORIZON_LOG_INFO, p->id.c_str(), "not started: %s",
                p->statusText.c_str());
        return;
    }
    p->info = info;
    if (info->name && *info->name) p->name = info->name;
    if (info->version) p->version = info->version;
    if (info->author && *info->author) p->author = info->author;
    if (info->description && *info->description) p->description = info->description;
    p->soloOnly = !(info->flags & HORIZON_FLAG_ONLINE_OK);
    p->live = (info->flags & HORIZON_FLAG_LIVE_TOGGLE) != 0;
    if (const ProfilePlugin* entry = g_profile.find(p->id)) {
        if (!entry->version.empty() && entry->version != p->version) {
            logLine(HORIZON_LOG_WARN, p->id.c_str(), "profile.json wants version %s, the installed one is %s: loading it anyway",
                    entry->version.c_str(), p->version.c_str());
        }
    }
    p->settings.load(state().dataRoot + L"settings\\" + wide(p->id) + L".json");
    p->dataFolderW = state().dataRoot + L"data\\" + wide(p->id) + L"\\";
    ensureDir(p->dataFolderW);
    p->dataFolder = utf8(p->dataFolderW);
    fillApi(p);
    int32_t result = -1;
    const DWORD t0 = GetTickCount();
    if (!sehInit(initFn, &p->api, &result)) {
        p->status = ui::PluginStatus::Failed;
        p->statusText = "crashed in Init";
        logLine(HORIZON_LOG_ERROR, p->id.c_str(), "crashed in HorizonPlugin_Init: not started");
        return;
    }
    if (result != 0) {
        p->status = ui::PluginStatus::Failed;
        p->statusText = "Init returned " + std::to_string(result);
        logLine(HORIZON_LOG_ERROR, p->id.c_str(), "HorizonPlugin_Init returned %d: not started", result);
        return;
    }
    p->running = true;
    p->status = ui::PluginStatus::On;
    p->statusText.clear();
    logLine(HORIZON_LOG_INFO, p->id.c_str(), "started: %s %s (%s, %s%s), Init took %lu ms", p->name.c_str(), p->version.c_str(),
            p->soloOnly ? "solo only" : "online ok", p->live ? "live on/off" : "on/off after restart",
            p->drawMenu ? ", has a page" : "", GetTickCount() - t0);
}

} // namespace

bool pluginGated(const Plugin* p) { return p->soloOnly && (!game().onlineKnown() || isOnlineNow() || runGate()); }

std::vector<std::unique_ptr<Plugin>>& plugins() { return g_plugins; }
const Profile& profile() { return g_profile; }

Plugin* findPlugin(const std::string& id) {
    for (auto& p : g_plugins)
        if (p->id == id) return p.get();
    return nullptr;
}

size_t pluginCount() { return g_plugins.size(); }

void pluginsReadProfile() {
    HostState& s = state();
    const std::wstring profilePath = s.baseFolder + L"profile.json";
    std::string text;
    if (readFile(profilePath, text)) {
        g_profile = parseProfile(text);
        if (!g_profile.valid) {
            HERR("profile: %s - no plugins are loaded", g_profile.error.c_str());
            s.notes.push_back("profile.json: " + g_profile.error);
        } else {
            HLOG("profile: \"%s\" (format %d, %zu plugins listed%s%s)", g_profile.name.c_str(), g_profile.format, g_profile.plugins.size(),
                 g_profile.writtenBy.empty() ? "" : ", written by ", g_profile.writtenBy.c_str());
            for (const auto& w : g_profile.warnings) HWARN("profile: %s", w.c_str());
            s.profileName = g_profile.name;
            if (g_profile.safeMode && !s.safeMode) {
                s.safeMode = true;
                s.safeWhy = "profile.json (Start without mods)";
            }
        }
    } else {
        g_profile = Profile{};
        HLOG("profile: no profile.json next to host.dll: every installed plugin is loaded");
    }
}

void pluginsLoadAll() {
    HostState& s = state();

    // installed plugins
    const std::wstring root = s.baseFolder + L"plugins\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            const std::string id = utf8(fd.cFileName);
            if (!validId(id)) {
                HWARN("plugins: folder \"%s\" is not a valid plugin id, skipped", id.c_str());
                continue;
            }
            auto p = std::make_unique<Plugin>();
            p->id = id;
            p->name = id;
            p->folder = root + fd.cFileName + L"\\";
            std::string mt;
            if (readFile(p->folder + L"plugin.json", mt)) {
                p->manifest = parsePluginManifest(mt, id);
                if (!p->manifest.valid) {
                    p->status = ui::PluginStatus::Failed;
                    p->statusText = p->manifest.error;
                }
            } else {
                p->manifest.valid = true;
                p->manifest.id = id;
                p->manifest.name = id;
                p->manifest.dll = id + ".dll";
            }
            if (!p->manifest.name.empty()) p->name = p->manifest.name;
            p->version = p->manifest.version;
            p->author = p->manifest.author;
            p->description = p->manifest.description;
            p->soloOnly = p->manifest.soloOnly;
            g_plugins.push_back(std::move(p));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (const auto& e : g_profile.plugins)
        if (e.enabled && !findPlugin(e.id)) HWARN("profile: \"%s\" is enabled but not installed in %s", e.id.c_str(), utf8(root).c_str());

    for (auto& up : g_plugins) {
        Plugin* p = up.get();
        const bool inProfile = !g_profile.present || (g_profile.valid && g_profile.find(p->id) && g_profile.find(p->id)->enabled);
        // default when the menu never switched it: on, unless plugin.json says enabled_by_default false (only
        // without profile.json) or the old ESWC Autosave loader is chained (two autosaves = double saves)
        bool localDefault = g_profile.present || p->manifest.enabledByDefault;
        std::string offWhy = "switched off in the menu";
        if (p->id == "eswc-autosave" && s.chainIsAutosave) {
            localDefault = false;
            offWhy = "the old ESWC Autosave loader (dinput8.chain.dll) is active";
        } else if (!g_profile.present && !p->manifest.enabledByDefault) {
            offWhy = "off by default: switch it on to try it";
        }
        const bool local = hostSettingInt("plugin." + p->id + ".enabled", localDefault ? 1 : 0) != 0;
        p->enabled = inProfile && local;
        if (p->status == ui::PluginStatus::Failed) {
            logLine(HORIZON_LOG_ERROR, p->id.c_str(), "not loaded: %s", p->statusText.c_str());
            continue;
        }
        if (p->manifest.games && s.game.game && !(p->manifest.games & s.game.game)) {
            p->status = ui::PluginStatus::WrongGame;
            p->statusText = "not made for this game";
            continue;
        }
        if (s.safeMode) {
            p->status = ui::PluginStatus::Off;
            p->statusText = "safe mode";
            continue;
        }
        if (!inProfile) {
            p->status = ui::PluginStatus::Off;
            p->statusText = "off in TM Horizon";
            logLine(HORIZON_LOG_INFO, p->id.c_str(), "not loaded: not enabled in profile.json");
            continue;
        }
        if (!local) {
            p->status = ui::PluginStatus::Off;
            p->statusText = offWhy;
            logLine(HORIZON_LOG_INFO, p->id.c_str(), "not loaded: %s", offWhy.c_str());
            continue;
        }
        loadOne(p);
    }
    if (s.safeMode) HLOG("safe mode (%s): %zu plugins installed, none loaded", s.safeWhy.c_str(), g_plugins.size());
}

void pluginSetEnabled(const std::string& id, bool on) {
    Plugin* p = findPlugin(id);
    if (!p) return;
    hostSetSettingInt("plugin." + id + ".enabled", on ? 1 : 0);
    if (p->status == ui::PluginStatus::Failed || p->status == ui::PluginStatus::WrongGame) return;
    if (p->running) {
        if (p->live) {
            p->enabled = on;
            p->status = on ? ui::PluginStatus::On : ui::PluginStatus::Off;
            p->statusText = on ? "" : "switched off in the menu";
            if (p->setEnabled && !sehInt(p->setEnabled, on ? 1 : 0)) crashed(p, "SetEnabled");
            logLine(HORIZON_LOG_INFO, p->id.c_str(), "switched %s in the menu", on ? "on" : "off");
        } else {
            p->status = on == p->enabled ? ui::PluginStatus::On : ui::PluginStatus::NeedsRestart;
            p->statusText = on == p->enabled ? "" : "applies after a restart";
            p->wanted = on;
            logLine(HORIZON_LOG_INFO, p->id.c_str(), "switched %s in the menu: applies after a restart", on ? "on" : "off");
        }
        return;
    }
    // not loaded this start
    p->wanted = on;
    const bool inProfile = !g_profile.present || (g_profile.valid && g_profile.find(p->id) && g_profile.find(p->id)->enabled);
    if (on && !inProfile) {
        p->status = ui::PluginStatus::Off;
        p->statusText = "turn it on in TM Horizon";
    } else if (state().safeMode) {
        p->status = ui::PluginStatus::Off;
        p->statusText = "safe mode";
    } else {
        p->status = on ? ui::PluginStatus::NeedsRestart : ui::PluginStatus::Off;
        p->statusText = on ? "applies after a restart" : "switched off in the menu";
    }
    logLine(HORIZON_LOG_INFO, p->id.c_str(), "switched %s in the menu (not loaded now: %s)", on ? "on" : "off", p->statusText.c_str());
}

bool pluginSwitchPosition(const Plugin* p) {
    if (p->running && p->live) return p->enabled;
    if (p->wanted >= 0) return p->wanted != 0;
    return p->enabled;
}

void pluginCrashed(Plugin* p, const char* where) { crashed(p, where); }

void pluginDrawMenu(const std::string& id) {
    Plugin* p = findPlugin(id);
    if (!p) return;
    if (!p->running || !p->drawMenu) return;
    if (!sehVoid(p->drawMenu)) crashed(p, "DrawMenu");
}

void pluginsDrawOverlays() {
    for (auto& up : g_plugins) {
        Plugin* p = up.get();
        if (!p->running || !p->enabled || !p->drawOverlay || pluginGated(p)) continue;
        if (!sehVoid(p->drawOverlay)) crashed(p, "DrawOverlay");
    }
}

void pluginsDeliver(const HorizonEvent& e) {
    for (auto& up : g_plugins) {
        Plugin* p = up.get();
        if (!p->running || !p->enabled || !p->onEvent) continue;
        if (!(p->eventMask & HORIZON_EVENT_BIT(e.type))) continue;
        if (e.type != HORIZON_EVENT_ONLINE_CHANGED && pluginGated(p)) continue;
        if (!sehEvent(p->onEvent, &e)) crashed(p, "OnEvent");
    }
}

void pluginsRunQueued() {
    std::vector<Queued> q;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        q.swap(g_queue);
    }
    for (const auto& item : q) {
        Plugin* p = item.plugin;
        if (p && (!p->running || pluginGated(p))) {
            if (p->running) logLine(HORIZON_LOG_INFO, p->id.c_str(), "queued call dropped: solo only, off while online");
            continue;
        }
        if (!sehCallback(item.cb, item.user) && p) crashed(p, "a queued call");
    }
}

void pluginsSaveSettings(bool force) {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (!force && now - last < 1000) return;
    last = now;
    for (auto& up : g_plugins)
        if (up->settings.dirty() && !up->settings.saveIfDirty())
            logLine(HORIZON_LOG_WARN, up->id.c_str(), "settings could not be written");
}

void pluginsShutdownAll() {
    pluginsRunQueued();
    for (auto& up : g_plugins) {
        Plugin* p = up.get();
        if (!p->running) continue;
        p->running = false;
        if (!sehVoid(p->shutdown)) logLine(HORIZON_LOG_ERROR, p->id.c_str(), "crashed in Shutdown");
        else logLine(HORIZON_LOG_INFO, p->id.c_str(), "shut down");
        panelsRemovePlugin(p);
    }
    pluginsSaveSettings(true);
    // The DLLs stay loaded until the process ends: queued calls, threads and hooks of theirs may still point into them.
}

// ---------------------------------------------------------------- device callbacks
void pluginsDeviceCreated(void* device, void* params) {
    for (auto& up : g_plugins) {
        Plugin* p = up.get();
        if (p->running && p->hasDev && p->dev.device_created && !sehDev3(p->dev.device_created, p->dev.user, device, params))
            crashed(p, "device_created");
    }
}
void pluginsAdjustParams(void* params) {
    for (auto& up : g_plugins) {
        Plugin* p = up.get();
        if (p->running && p->hasDev && p->dev.adjust_present_params && !sehDev2(p->dev.adjust_present_params, p->dev.user, params))
            crashed(p, "adjust_present_params");
    }
}
void pluginsBeforeReset(void* device) {
    for (auto& up : g_plugins) {
        Plugin* p = up.get();
        if (p->running && p->hasDev && p->dev.before_reset && !sehDev2(p->dev.before_reset, p->dev.user, device)) crashed(p, "before_reset");
    }
}
void pluginsAfterReset(void* device, long hr) {
    for (auto& up : g_plugins) {
        Plugin* p = up.get();
        if (p->running && p->hasDev && p->dev.after_reset && !sehDevHr(p->dev.after_reset, p->dev.user, device, static_cast<int32_t>(hr)))
            crashed(p, "after_reset");
    }
}
void pluginsPrePresent(void* device) {
    for (auto& up : g_plugins) {
        Plugin* p = up.get();
        if (p->running && p->enabled && p->hasDev && p->dev.pre_present && !sehDev2(p->dev.pre_present, p->dev.user, device))
            crashed(p, "pre_present");
    }
}

} // namespace horizon
