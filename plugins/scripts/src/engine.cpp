#include "engine.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "angelscript.h"
#include "fsutil.h"
#include "scriptarray/scriptarray.h"
#include "scriptbuilder/scriptbuilder.h"

namespace hs {

// ---------------------------------------------------------------- current script ------------------

namespace {
Manager* g_manager = nullptr;
Manager* g_instance = nullptr; // the plugin's manager (queued game-thread calls run outside script calls)
Script* g_script = nullptr;

// AngelScript's own allocations (engine, modules, script arrays) count against a cap, so a script
// cannot eat the 32-bit game's address space ("Out of memory" exception instead).
const size_t kMemCap = 256u * 1024u * 1024u;
size_t g_memUsed = 0;
void* capAlloc(size_t n) {
    if (n > kMemCap || g_memUsed + n > kMemCap) return nullptr;
    void* p = malloc(n + 8);
    if (!p) return nullptr;
    *static_cast<size_t*>(p) = n;
    g_memUsed += n;
    return static_cast<char*>(p) + 8;
}
void capFree(void* p) {
    if (!p) return;
    void* base = static_cast<char*>(p) - 8;
    g_memUsed -= *static_cast<size_t*>(base);
    free(base);
}

const char* kDecls[CbCount][3] = {
    {"void Main()", nullptr, nullptr},
    {"void Render()", nullptr, nullptr},
    {"void RenderMenu()", nullptr, nullptr},
    {"void OnMapLoaded(Game::Map@)", "void OnMapLoaded(const Game::Map@)", nullptr},
    {"void OnMapLoaded()", nullptr, nullptr},
    {"void OnRaceStart()", nullptr, nullptr},
    {"void OnCheckpoint(int, int)", nullptr, nullptr},
    {"void OnRespawn(int)", nullptr, nullptr},
    {"void OnFinish(int, bool)", nullptr, nullptr},
    {"void OnRaceEnd()", nullptr, nullptr},
    {"void OnMenu()", nullptr, nullptr},
    {"void OnGameState(Game::State)", nullptr, nullptr},
    {"void OnPeek()", nullptr, nullptr},
    {"void OnUnload()", nullptr, nullptr},
    {"void RenderWidget(const string &in)", nullptr, nullptr},
};

struct CbName {
    const char* name;
    const char* expected; // shown when a function with this name has another signature
    uint32_t perm;
};
const CbName kCbNames[] = {
    {"Main", "void Main()", 0},
    {"Render", "void Render()", PermUi},
    {"RenderMenu", "void RenderMenu()", PermUi},
    {"OnMapLoaded", "void OnMapLoaded(Game::Map@ map) or void OnMapLoaded()", PermReadGame},
    {"OnRaceStart", "void OnRaceStart()", PermReadGame},
    {"OnCheckpoint", "void OnCheckpoint(int time, int index)", PermReadGame},
    {"OnRespawn", "void OnRespawn(int count)", PermReadGame},
    {"OnFinish", "void OnFinish(int time, bool isPb)", PermReadGame},
    {"OnRaceEnd", "void OnRaceEnd()", PermReadGame},
    {"OnMenu", "void OnMenu()", PermReadGame},
    {"OnGameState", "void OnGameState(Game::State state)", PermReadGame},
    {"OnPeek", "void OnPeek()", 0},
    {"OnUnload", "void OnUnload()", 0},
    {"RenderWidget", "void RenderWidget(const string &in id)", PermUi},
};

struct IncludeCtx {
    Manager* m;
    Script* s;
    bool used;
};
int rejectInclude(const char* include, const char* from, CScriptBuilder*, void* user) {
    IncludeCtx* c = static_cast<IncludeCtx*>(user);
    c->used = true;
    c->m->addMessage(*c->s, Message::Error, from ? from : "",
                     std::string("#include \"") + include + "\" is not needed: every .as file in the script's folder is compiled together.");
    return -1;
}

std::string where(const char* section, int line, int col) {
    std::string w = section ? section : "";
    if (line > 0) {
        char b[32];
        snprintf(b, sizeof(b), col > 0 ? ":%d:%d" : ":%d", line, col);
        w += b;
    }
    return w;
}

// Keeps a setting inside its min/max and its options (saved values and defaults alike).
void clampSetting(ScriptSetting& st) {
    if (st.meta.hasMin && st.meta.hasMax) {
        if (st.typeId == asTYPEID_INT32)
            *static_cast<int32_t*>(st.addr) = std::clamp(*static_cast<int32_t*>(st.addr), static_cast<int32_t>(st.meta.min), static_cast<int32_t>(st.meta.max));
        if (st.typeId == asTYPEID_UINT32)
            *static_cast<uint32_t*>(st.addr) = std::clamp(*static_cast<uint32_t*>(st.addr), static_cast<uint32_t>(st.meta.min), static_cast<uint32_t>(st.meta.max));
        if (st.typeId == asTYPEID_FLOAT)
            *static_cast<float*>(st.addr) = std::clamp(*static_cast<float*>(st.addr), static_cast<float>(st.meta.min), static_cast<float>(st.meta.max));
        if (st.typeId == asTYPEID_DOUBLE) *static_cast<double*>(st.addr) = std::clamp(*static_cast<double*>(st.addr), st.meta.min, st.meta.max);
    }
    if (!st.meta.options.empty() && st.typeId == asTYPEID_INT32)
        *static_cast<int32_t*>(st.addr) = std::clamp(*static_cast<int32_t*>(st.addr), 0, static_cast<int32_t>(st.meta.options.size()) - 1);
}

int executeGuarded(asIScriptContext* ctx, bool* crashed) {
    __try {
        return ctx->Execute();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *crashed = true;
        return asERROR;
    }
}

} // namespace

Script* currentScript() { return g_script; }
Manager* currentManager() { return g_manager ? g_manager : g_instance; }
void setCurrent(Manager* m, Script* s) {
    g_manager = m;
    g_script = s;
}

void scriptError(const std::string& text) {
    if (asIScriptContext* ctx = asGetActiveContext()) ctx->SetException(text.c_str());
}

bool requirePerm(uint32_t perm, const char* what) {
    Script* s = currentScript();
    if (s && (s->manifest.perms & perm)) return true;
    scriptError(std::string(what) + " needs the permission \"" + permName(perm) + "\" in info.toml");
    return false;
}

const char* stateLabel(ScriptState s) {
    switch (s) {
    case ScriptState::Off: return "Off";
    case ScriptState::Running: return "Running";
    case ScriptState::Paused: return "Paused";
    case ScriptState::WrongGame: return "Other game";
    case ScriptState::BlockedCp: return "Blocked";
    case ScriptState::Failed: return "Error";
    case ScriptState::Stopped: return "Stopped";
    case ScriptState::Suspended: return "Suspended";
    }
    return "?";
}

// ---------------------------------------------------------------- settings as text ----------------

std::string settingToText(int typeId, const void* addr, int stringTypeId) {
    char b[64];
    switch (typeId) {
    case asTYPEID_BOOL: return *static_cast<const bool*>(addr) ? "true" : "false";
    case asTYPEID_INT32: snprintf(b, sizeof(b), "%d", *static_cast<const int32_t*>(addr)); return b;
    case asTYPEID_UINT32: snprintf(b, sizeof(b), "%u", *static_cast<const uint32_t*>(addr)); return b;
    case asTYPEID_FLOAT: snprintf(b, sizeof(b), "%.9g", static_cast<double>(*static_cast<const float*>(addr))); return b;
    case asTYPEID_DOUBLE: snprintf(b, sizeof(b), "%.17g", *static_cast<const double*>(addr)); return b;
    default:
        if (typeId == stringTypeId) return *static_cast<const std::string*>(addr);
        return std::string();
    }
}

bool settingFromText(int typeId, void* addr, const std::string& t, int stringTypeId) {
    char* end = nullptr;
    switch (typeId) {
    case asTYPEID_BOOL:
        if (t != "true" && t != "false") return false;
        *static_cast<bool*>(addr) = t == "true";
        return true;
    case asTYPEID_INT32: {
        const long v = strtol(t.c_str(), &end, 10);
        if (t.empty() || *end) return false;
        *static_cast<int32_t*>(addr) = static_cast<int32_t>(v);
        return true;
    }
    case asTYPEID_UINT32: {
        const unsigned long v = strtoul(t.c_str(), &end, 10);
        if (t.empty() || *end) return false;
        *static_cast<uint32_t*>(addr) = static_cast<uint32_t>(v);
        return true;
    }
    case asTYPEID_FLOAT:
    case asTYPEID_DOUBLE: {
        const double v = strtod(t.c_str(), &end);
        if (t.empty() || *end) return false;
        if (typeId == asTYPEID_FLOAT) *static_cast<float*>(addr) = static_cast<float>(v);
        else *static_cast<double*>(addr) = v;
        return true;
    }
    default:
        if (typeId != stringTypeId) return false;
        *static_cast<std::string*>(addr) = t;
        return true;
    }
}

// ---------------------------------------------------------------- manager -------------------------

bool Manager::init(const HorizonHostApi* host, const std::wstring& scriptsDir, const std::wstring& dataDir, uint32_t gameBit) {
    host_ = host;
    game = gameBit;
    scriptsDir_ = scriptsDir;
    dataDir_ = dataDir;
    if (!scriptsDir_.empty() && scriptsDir_.back() != L'\\') scriptsDir_ += L'\\';
    if (!dataDir_.empty() && dataDir_.back() != L'\\') dataDir_ += L'\\';
    gate_.game = gameBit;
    g_instance = this;
    startMs = nowMs = GetTickCount64();
    g_memUsed = 0;
    asSetGlobalMemoryFunctions(capAlloc, capFree);
    engine_ = asCreateScriptEngine();
    if (!engine_) return false;
    engine_->SetMessageCallback(asMETHOD(Manager, onMessage), this, asCALL_THISCALL);
    engine_->SetEngineProperty(asEP_INIT_GLOBAL_VARS_AFTER_BUILD, false); // run initialisers under the budget
    engine_->SetEngineProperty(asEP_MAX_STACK_SIZE, 1024 * 1024);
    engine_->SetEngineProperty(asEP_MAX_CALL_STACK_SIZE, 1000);
    engine_->SetEngineProperty(asEP_MAX_NESTED_CALLS, 50);
    engine_->SetEngineProperty(asEP_FOREACH_SUPPORT, true);
    engine_->SetEngineProperty(asEP_COMPILER_WARNINGS, 1);
    bool ok = registerCoreApi(engine_) && registerGameApi(engine_) && registerUiApi(engine_) && registerIoApi(engine_) &&
              registerReflectionApi(engine_);
    stringTypeId_ = engine_->GetTypeIdByDecl("string");
    if (!ok) {
        if (host_) host_->log(host_->plugin, HORIZON_LOG_ERROR, "script API registration failed");
        shutdown();
        return false;
    }
    makeDirs(scriptsDir_);
    nextPollMs_ = 0;
    return true;
}

void Manager::shutdown() {
    for (auto& s : scripts_) unloadScript(*s, true);
    scripts_.clear();
    reflectionShutdown();
    if (engine_) {
        engine_->ShutDownAndRelease();
        engine_ = nullptr;
    }
    asResetGlobalMemoryFunctions();
    if (g_instance == this) g_instance = nullptr;
}

Script* Manager::find(const std::string& id) {
    for (auto& s : scripts_)
        if (s->id == id) return s.get();
    return nullptr;
}

std::string Manager::settingKey(const Script& s, const std::string& var) const { return "s." + s.id + "." + var; }

void Manager::addMessage(Script& s, Message::Kind k, const std::string& w, const std::string& text) {
    Message m;
    m.kind = k;
    m.where = w;
    m.text = text;
    s.messages.push_back(m);
    if (k == Message::Error) s.errorCount++;
    if (k == Message::Warning) s.warningCount++;
    if (s.messages.size() > 200) s.messages.erase(s.messages.begin(), s.messages.begin() + 50);
    if (host_ && k != Message::Log) {
        const int level = k == Message::Error ? HORIZON_LOG_ERROR : k == Message::Warning ? HORIZON_LOG_WARN : HORIZON_LOG_INFO;
        host_->log(host_->plugin, level, "[%s] %s%s%s", s.id.c_str(), w.c_str(), w.empty() ? "" : ": ", text.c_str());
    }
}

void Manager::scriptLog(Script& s, int level, const std::string& text) {
    Message m;
    m.kind = level >= HORIZON_LOG_ERROR ? Message::Error : level == HORIZON_LOG_WARN ? Message::Warning : Message::Log;
    m.text = text;
    s.messages.push_back(m);
    if (s.messages.size() > 200) s.messages.erase(s.messages.begin(), s.messages.begin() + 50);
    // at most 20 lines per second per script into the host log
    if (nowMs - s.logWindowStart >= 1000) {
        if (s.logDropped && host_)
            host_->log(host_->plugin, HORIZON_LOG_WARN, "[%s] %d log lines not written (more than 20 per second)", s.id.c_str(), s.logDropped);
        s.logWindowStart = nowMs;
        s.logInWindow = 0;
        s.logDropped = 0;
    }
    if (++s.logInWindow > 20) {
        s.logDropped++;
        return;
    }
    if (host_) host_->log(host_->plugin, level, "[%s] %s", s.id.c_str(), text.c_str());
}

void Manager::onMessage(const asSMessageInfo* msg) {
    Message::Kind k = msg->type == asMSGTYPE_ERROR ? Message::Error : msg->type == asMSGTYPE_WARNING ? Message::Warning : Message::Info;
    if (compiling_) {
        addMessage(*compiling_, k, where(msg->section, msg->row, msg->col), msg->message);
    } else if (host_) {
        host_->log(host_->plugin, k == Message::Error ? HORIZON_LOG_ERROR : HORIZON_LOG_INFO, "angelscript: %s (%d,%d): %s", msg->section, msg->row,
                   msg->col, msg->message);
    }
}

void Manager::lineCallback(asIScriptContext* ctx) {
    if (budget_.expired()) ctx->Abort();
}

bool Manager::call(Script& s, asIScriptFunction* f, const char* what, double limitMs, bool ui, void (*args)(asIScriptContext*, const void*),
                   const void* argData) {
    if (!f || !engine_) return true;
    asIScriptContext* ctx = engine_->RequestContext();
    if (!ctx) return false;
    if (ctx->Prepare(f) < 0) {
        engine_->ReturnContext(ctx);
        return false;
    }
    if (args) args(ctx, argData);
    ctx->SetLineCallback(asMETHOD(Manager, lineCallback), this, asCALL_THISCALL);
    Manager* prevM = g_manager;
    Script* prevS = g_script;
    setCurrent(this, &s);
    if (ui) uiBeginCall();
    budget_.start(limitMs);
    bool crashed = false;
    const int r = executeGuarded(ctx, &crashed);
    const double ms = budget_.elapsedMs();
    if (ui) uiEndCall();
    setCurrent(prevM, prevS);
    s.lastMs = ms;
    if (ms > s.maxMs) s.maxMs = ms;
    if (crashed) {
        // the context may be in any state after a hardware exception: abandon it
        s.state = ScriptState::Stopped;
        s.stateText = std::string(what) + " crashed (access violation or similar).";
        addMessage(s, Message::Error, "", std::string(what) + " crashed. The script is stopped; Reload starts it again.");
        return false;
    }
    bool ok = true;
    if (r == asEXECUTION_ABORTED) {
        int col = 0;
        const char* section = nullptr;
        const int line = ctx->GetLineNumber(0, &col, &section);
        char b[160];
        snprintf(b, sizeof(b), "%s ran longer than %.0f ms and was stopped (an endless loop?).", what, limitMs);
        s.state = ScriptState::Suspended;
        s.stateText = b;
        addMessage(s, Message::Error, where(section, line, col), std::string(b) + " Save a file or press Reload to start it again.");
        ok = false;
    } else if (r == asEXECUTION_EXCEPTION) {
        int col = 0;
        const char* section = nullptr;
        const int line = ctx->GetExceptionLineNumber(&col, &section);
        const char* ex = ctx->GetExceptionString();
        s.state = ScriptState::Stopped;
        s.stateText = std::string(what) + ": " + (ex ? ex : "exception");
        addMessage(s, Message::Error, where(section, line, col), s.stateText);
        ok = false;
    } else if (r != asEXECUTION_FINISHED) {
        s.state = ScriptState::Stopped;
        s.stateText = std::string(what) + " did not finish.";
        addMessage(s, Message::Error, "", s.stateText);
        ok = false;
    }
    engine_->ReturnContext(ctx);
    return ok;
}

// ---------------------------------------------------------------- loading -------------------------

bool Manager::compile(Script& s) {
    CScriptBuilder b;
    IncludeCtx inc = {this, &s, false};
    b.SetIncludeCallback(rejectInclude, &inc);
    // #if ESWC / SUNRISE / TMNF / TMUF / FOREVER / LEGACY for the few lines that differ per game
    if (game == HORIZON_GAME_ESWC) b.DefineWord("ESWC");
    if (game == HORIZON_GAME_SUNRISE) b.DefineWord("SUNRISE");
    if (game == HORIZON_GAME_ORIGINAL) b.DefineWord("ORIGINAL");
    if (game == HORIZON_GAME_TMNF) b.DefineWord("TMNF");
    if (game == HORIZON_GAME_TMUF) b.DefineWord("TMUF");
    if (game & HORIZON_GAME_FOREVER) b.DefineWord("FOREVER");
    if (game & HORIZON_GAME_LEGACY) b.DefineWord("LEGACY");
    const std::string moduleName = "script:" + s.id;
    if (b.StartNewModule(engine_, moduleName.c_str()) < 0) {
        addMessage(s, Message::Error, "", "could not create the script module");
        return false;
    }
    s.files.clear();
    for (const FileStamp& f : scriptFiles(s.folder)) {
        if (f.rel.size() < 3 || _stricmp(f.rel.c_str() + f.rel.size() - 3, ".as") != 0) continue;
        std::string code;
        std::wstring path = s.folder + widen(f.rel);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        if (!readFile(path, &code, 1024 * 1024)) {
            addMessage(s, Message::Error, f.rel, "could not be read (or is larger than 1 MB)");
            continue;
        }
        if (code.size() >= 3 && static_cast<unsigned char>(code[0]) == 0xEF && static_cast<unsigned char>(code[1]) == 0xBB &&
            static_cast<unsigned char>(code[2]) == 0xBF)
            code.erase(0, 3);
        b.AddSectionFromMemory(f.rel.c_str(), code.c_str(), static_cast<unsigned>(code.size()), 0);
        s.files.push_back(f.rel);
    }
    if (s.files.empty()) {
        addMessage(s, Message::Error, "", "no .as file in the script's folder");
        engine_->DiscardModule(moduleName.c_str());
        return false;
    }
    compiling_ = &s;
    const int r = b.BuildModule();
    compiling_ = nullptr;
    asIScriptModule* mod = engine_->GetModule(moduleName.c_str(), asGM_ONLY_IF_EXISTS);
    if (r < 0 || !mod || inc.used) {
        if (!s.errorCount) addMessage(s, Message::Error, "", "the script did not compile");
        if (mod) mod->Discard();
        return false;
    }
    s.module = mod;
    mod->SetUserData(&s);

    // global variables: initialisers run here, under the budget (4x)
    {
        asIScriptContext* ctx = engine_->RequestContext();
        ctx->SetLineCallback(asMETHOD(Manager, lineCallback), this, asCALL_THISCALL);
        Manager* prevM = g_manager;
        Script* prevS = g_script;
        setCurrent(this, &s);
        budget_.start(cfg.callLimitMs * 4);
        const int gr = mod->ResetGlobalVars(ctx);
        setCurrent(prevM, prevS);
        const bool aborted = ctx->GetState() == asEXECUTION_ABORTED;
        const bool exception = ctx->GetState() == asEXECUTION_EXCEPTION;
        std::string ex = exception && ctx->GetExceptionString() ? ctx->GetExceptionString() : "";
        int col = 0;
        const char* section = nullptr;
        const int line = exception ? ctx->GetExceptionLineNumber(&col, &section) : 0;
        engine_->ReturnContext(ctx);
        if (gr < 0) {
            if (aborted) addMessage(s, Message::Error, "", "a global variable's initialiser ran too long (an endless loop?)");
            else addMessage(s, Message::Error, where(section, line, col), "a global variable's initialiser failed" + (ex.empty() ? "" : ": " + ex));
            mod->Discard();
            s.module = nullptr;
            return false;
        }
    }

    // [Setting] globals
    s.settings.clear();
    for (asUINT i = 0; i < mod->GetGlobalVarCount(); i++) {
        const char* name = nullptr;
        int typeId = 0;
        bool isConst = false;
        mod->GetGlobalVar(i, &name, nullptr, &typeId, &isConst);
        for (const std::string& meta : b.GetMetadataForVar(static_cast<int>(i))) {
            SettingMeta sm;
            std::string err;
            if (!parseSettingMeta(meta, &sm, &err)) continue;
            if (!err.empty()) addMessage(s, Message::Warning, "", std::string("[Setting] on ") + name + ": " + err);
            if (!(s.manifest.perms & PermSettings)) {
                addMessage(s, Message::Warning, "", std::string("[Setting] on ") + name + " is ignored: add \"settings\" to permissions in info.toml");
                break;
            }
            const bool supported = typeId == asTYPEID_BOOL || typeId == asTYPEID_INT32 || typeId == asTYPEID_UINT32 || typeId == asTYPEID_FLOAT ||
                                   typeId == asTYPEID_DOUBLE || typeId == stringTypeId_;
            if (!supported || isConst) {
                addMessage(s, Message::Warning, "", std::string("[Setting] on ") + name + " is ignored: settings are bool, int, uint, float, double or string (not const)");
                break;
            }
            ScriptSetting st;
            st.var = name;
            st.typeId = typeId;
            st.addr = mod->GetAddressOfGlobalVar(i);
            st.meta = sm;
            st.defaultText = settingToText(typeId, st.addr, stringTypeId_);
            char buf[4096];
            const std::string key = settingKey(s, st.var);
            if (host_ && host_->get_string(host_->plugin, key.c_str(), "\x01", buf, sizeof(buf)) >= 0 && buf[0] != '\x01') {
                if (!settingFromText(typeId, st.addr, buf, stringTypeId_))
                    addMessage(s, Message::Warning, "", "saved value of " + st.var + " does not fit its type; the default is used");
            }
            clampSetting(st);
            st.savedText = settingToText(typeId, st.addr, stringTypeId_);
            s.settings.push_back(st);
            break;
        }
    }
    return true;
}

void Manager::bindCallbacks(Script& s) {
    for (int i = 0; i < CbCount; i++) {
        s.cb[i] = nullptr;
        for (const char* d : kDecls[i])
            if (d && !s.cb[i]) s.cb[i] = s.module->GetFunctionByDecl(d);
    }
    // a function with a callback's name but another signature is never called: say so
    for (const CbName& c : kCbNames) {
        bool named = false, matched = false;
        for (asUINT i = 0; i < s.module->GetFunctionCount(); i++) {
            asIScriptFunction* f = s.module->GetFunctionByIndex(i);
            if (strcmp(f->GetName(), c.name) != 0 || (f->GetNamespace() && *f->GetNamespace())) continue;
            named = true;
            for (int k = 0; k < CbCount; k++)
                if (s.cb[k] == f) matched = true;
        }
        if (named && !matched) addMessage(s, Message::Warning, "", std::string(c.name) + " has another signature and is never called; expected " + c.expected);
        if (matched && c.perm && !(s.manifest.perms & c.perm))
            addMessage(s, Message::Warning, "", std::string(c.name) + "() is never called: add \"" + permName(c.perm) + "\" to permissions in info.toml");
    }
}

void Manager::loadScript(Script& s) {
    s.messages.clear();
    s.errorCount = s.warningCount = 0;
    s.settings.clear();
    s.lastMs = s.maxMs = 0;
    const std::vector<FileStamp> files = scriptFiles(s.folder);
    s.loadedSig = signature(files);
    s.pendingSig = 0;

    // manifest
    std::string text, err;
    s.hasManifest = false;
    if (readFile(s.folder + L"info.toml", &text, 64 * 1024)) {
        s.hasManifest = parseManifestToml(text, &s.manifest, &err);
        if (!s.hasManifest) addMessage(s, Message::Error, "info.toml", err);
    } else if (readFile(s.folder + L"info.json", &text, 64 * 1024)) {
        s.hasManifest = parseManifestJson(text, &s.manifest, &err);
        if (!s.hasManifest) addMessage(s, Message::Error, "info.json", err);
    } else {
        addMessage(s, Message::Error, "", "info.toml (or info.json) is missing");
    }
    if (!s.hasManifest) {
        s.manifest = Manifest();
        s.state = ScriptState::Failed;
        s.stateText = "The manifest has an error.";
        return;
    }
    for (const std::string& w : s.manifest.warnings) addMessage(s, Message::Warning, "info", w);

    const Gate g = decide(s.manifest, gate_);
    if (g.state == GateState::WrongGame || g.state == GateState::BlockedCp) {
        s.state = g.state == GateState::WrongGame ? ScriptState::WrongGame : ScriptState::BlockedCp;
        s.stateText = gateText(g.state);
        if (g.state == GateState::WrongGame) s.stateText = "Made for " + gamesText(s.manifest.games) + ".";
        return;
    }
    if (!s.enabled) {
        s.state = ScriptState::Off;
        s.stateText.clear();
        return;
    }
    if (!compile(s)) {
        s.state = ScriptState::Failed;
        s.stateText = "Does not compile: see the errors.";
        return;
    }
    bindCallbacks(s);
    s.loads++;
    s.loadedAtMs = nowMs;
    s.state = ScriptState::Running;
    s.stateText.clear();
    s.mainDone = false;
    addMessage(s, Message::Info, "", "loaded (" + std::to_string(s.files.size()) + (s.files.size() == 1 ? " file)" : " files)"));
    applyGate(s); // runs Main() unless the script starts paused (then once it resumes)
}

void Manager::unloadScript(Script& s, bool callUnload) {
    if (!s.module) return;
    if (callUnload && s.cb[CbOnUnload] && (s.state == ScriptState::Running || s.state == ScriptState::Paused))
        call(s, s.cb[CbOnUnload], "OnUnload()", cfg.callLimitMs, false, nullptr, nullptr);
    saveChangedSettings(s);
    for (auto& c : s.cb) c = nullptr;
    s.settings.clear();
    s.widgets.clear();
    s.module->Discard();
    s.module = nullptr;
    if (engine_) engine_->GarbageCollect();
}

void Manager::applyGate(Script& s) {
    if (s.state != ScriptState::Running && s.state != ScriptState::Paused) return;
    const Gate g = decide(s.manifest, gate_);
    s.gameControlNow = g.gameControl;
    if (g.state == GateState::BlockedCp) {
        // Competition Patch showed up after the script was loaded
        unloadScript(s, true); // OnUnload may still save its own data files
        s.state = ScriptState::BlockedCp;
        s.stateText = gateText(g.state);
        return;
    }
    if (g.state == GateState::PausedOnline) {
        s.state = ScriptState::Paused;
        s.stateText = gateText(g.state);
    } else {
        s.state = ScriptState::Running;
        s.stateText.clear();
        if (!s.mainDone) {
            s.mainDone = true;
            if (s.cb[CbMain]) call(s, s.cb[CbMain], "Main()", cfg.callLimitMs * 4, false, nullptr, nullptr);
        }
    }
}

void Manager::setGateInput(const GateInput& in) {
    const bool changed = in.online != gate_.online || in.cpLoaded != gate_.cpLoaded || in.hostAllows != gate_.hostAllows;
    gate_ = in;
    gate_.game = game;
    online = in.online;
    if (changed)
        for (auto& s : scripts_) applyGate(*s);
}

void Manager::reload(Script& s) {
    unloadScript(s, true);
    loadScript(s);
}

void Manager::reloadAll() {
    for (auto& s : scripts_) reload(*s);
}

void Manager::setEnabled(Script& s, bool on) {
    if (s.enabled == on) return;
    s.enabled = on;
    if (host_) host_->set_int(host_->plugin, ("enabled." + s.id).c_str(), on ? 1 : 0);
    if (on) {
        loadScript(s);
    } else {
        unloadScript(s, true);
        if (s.state != ScriptState::WrongGame && s.state != ScriptState::BlockedCp && s.state != ScriptState::Failed) {
            s.state = ScriptState::Off;
            s.stateText.clear();
        }
    }
}

void Manager::rescan() {
    const std::vector<std::string> names = subfolders(scriptsDir_);
    // removed folders
    for (size_t i = 0; i < scripts_.size();) {
        if (std::find(names.begin(), names.end(), scripts_[i]->id) == names.end()) {
            unloadScript(*scripts_[i], true);
            if (host_) host_->log(host_->plugin, HORIZON_LOG_INFO, "[%s] removed", scripts_[i]->id.c_str());
            scripts_.erase(scripts_.begin() + static_cast<ptrdiff_t>(i));
        } else {
            i++;
        }
    }
    for (const std::string& name : names) {
        Script* s = find(name);
        if (!s) {
            if (!validScriptId(name)) continue;
            auto ns = std::make_unique<Script>();
            ns->id = name;
            ns->folder = scriptsDir_ + widen(name) + L"\\";
            ns->dataFolder = dataDir_ + widen(name) + L"\\";
            ns->enabled = !host_ || host_->get_int(host_->plugin, ("enabled." + name).c_str(), 1) != 0;
            s = ns.get();
            scripts_.push_back(std::move(ns));
            std::sort(scripts_.begin(), scripts_.end(), [](const std::unique_ptr<Script>& a, const std::unique_ptr<Script>& b) { return a->id < b->id; });
            loadScript(*s);
            continue;
        }
        // hot reload: a changed file list must stay the same for one poll (debounce) before reloading
        const uint64_t sig = signature(scriptFiles(s->folder));
        if (sig == s->loadedSig) {
            s->pendingSig = 0;
        } else if (sig != s->pendingSig) {
            s->pendingSig = sig;
            s->pendingSince = nowMs;
        } else if (nowMs - s->pendingSince >= 300) {
            if (host_) host_->log(host_->plugin, HORIZON_LOG_INFO, "[%s] files changed, reloading", s->id.c_str());
            reload(*s);
        }
    }
}

void Manager::tick(uint64_t now) {
    nowMs = now;
    if (!engine_) return;
    if (now >= nextPollMs_) {
        nextPollMs_ = now + 500;
        rescan();
    }
    if (now >= nextSettingsMs_) {
        nextSettingsMs_ = now + 2000;
        for (auto& s : scripts_) saveChangedSettings(*s);
    }
}

void Manager::saveChangedSettings(Script& s) {
    if (!host_) return;
    for (ScriptSetting& st : s.settings) {
        const std::string t = settingToText(st.typeId, st.addr, stringTypeId_);
        if (t == st.savedText) continue;
        st.savedText = t;
        host_->set_string(host_->plugin, settingKey(s, st.var).c_str(), t.c_str());
    }
}

void Manager::settingChanged(Script& s, ScriptSetting& st) {
    (void)st;
    saveChangedSettings(s);
}

void Manager::resetSettings(Script& s) {
    for (ScriptSetting& st : s.settings) {
        settingFromText(st.typeId, st.addr, st.defaultText, stringTypeId_);
        clampSetting(st);
    }
    saveChangedSettings(s);
}

// ---------------------------------------------------------------- callbacks -----------------------

void Manager::render() {
    for (auto& sp : scripts_) {
        Script& s = *sp;
        if (s.state != ScriptState::Running || !s.cb[CbRender] || !(s.manifest.perms & PermUi)) continue;
        call(s, s.cb[CbRender], "Render()", cfg.callLimitMs, true, nullptr, nullptr);
    }
}

void Manager::peek() {
    for (auto& sp : scripts_) {
        Script& s = *sp;
        if (s.state != ScriptState::Running || !s.cb[CbOnPeek]) continue;
        call(s, s.cb[CbOnPeek], "OnPeek()", cfg.callLimitMs, false, nullptr, nullptr);
    }
}

void Manager::renderWidget(Script& s, const std::string& id) {
    if (s.state != ScriptState::Running || !s.cb[CbRenderWidget] || !(s.manifest.perms & PermUi)) return;
    uiSetInWidget(true);
    call(s, s.cb[CbRenderWidget], "RenderWidget()", cfg.callLimitMs, true,
         [](asIScriptContext* ctx, const void* p) { ctx->SetArgAddress(0, const_cast<void*>(p)); }, &id);
    uiSetInWidget(false);
}

void Manager::renderMenu(Script& s) {
    if (s.state != ScriptState::Running || !s.cb[CbRenderMenu] || !(s.manifest.perms & PermUi)) return;
    call(s, s.cb[CbRenderMenu], "RenderMenu()", cfg.callLimitMs, true, nullptr, nullptr);
}

namespace {
struct Args2 {
    int a;
    int b;
};
void argsInt2(asIScriptContext* ctx, const void* p) {
    const Args2* a = static_cast<const Args2*>(p);
    ctx->SetArgDWord(0, static_cast<asDWORD>(a->a));
    ctx->SetArgDWord(1, static_cast<asDWORD>(a->b));
}
void argsInt1(asIScriptContext* ctx, const void* p) { ctx->SetArgDWord(0, static_cast<asDWORD>(static_cast<const Args2*>(p)->a)); }
void argsIntBool(asIScriptContext* ctx, const void* p) {
    const Args2* a = static_cast<const Args2*>(p);
    ctx->SetArgDWord(0, static_cast<asDWORD>(a->a));
    ctx->SetArgByte(1, a->b ? 1 : 0);
}
void argsMap(asIScriptContext* ctx, const void* p) { ctx->SetArgObject(0, const_cast<void*>(p)); }
} // namespace

void Manager::dispatch(const HorizonEvent& e) {
    if (e.type == HORIZON_EVENT_MAP_LOADED && e.map) {
        const HorizonMapInfo* m = e.map;
        auto str = [](const char* s) { return std::string(s ? s : ""); };
        map.name = str(m->name);
        map.author = str(m->author);
        map.uid = str(m->uid);
        map.path = str(m->path);
        map.environment = str(m->environment);
        map.authorTime = m->author_time;
        map.goldTime = m->gold_time;
        map.silverTime = m->silver_time;
        map.bronzeTime = m->bronze_time;
        map.authorScore = m->size >= offsetof(HorizonMapInfo, author_score) + sizeof(int32_t) ? m->author_score : -1;
        map.isStunts = m->size >= offsetof(HorizonMapInfo, is_stunts) + sizeof(uint32_t) && m->is_stunts != 0;
        hasMap = true;
    }
    if (e.type == HORIZON_EVENT_RACE_START) raceStartMs = static_cast<int64_t>(nowMs);
    if (e.type == HORIZON_EVENT_MENU_ENTERED) {
        hasMap = false;
        raceStartMs = -1;
    }
    if (e.type == HORIZON_EVENT_GAME_STATE) gameState = static_cast<int>(e.game_state);
    reflectionBeginFrame(cfg.readLimit);
    for (auto& sp : scripts_) {
        Script& s = *sp;
        if (s.state != ScriptState::Running || !(s.manifest.perms & PermReadGame)) continue;
        Args2 a = {0, 0};
        switch (e.type) {
        case HORIZON_EVENT_MAP_LOADED:
            if (s.cb[CbOnMapLoaded]) call(s, s.cb[CbOnMapLoaded], "OnMapLoaded()", cfg.callLimitMs, false, argsMap, hasMap ? &map : nullptr);
            else if (s.cb[CbOnMapLoadedNoArg]) call(s, s.cb[CbOnMapLoadedNoArg], "OnMapLoaded()", cfg.callLimitMs, false, nullptr, nullptr);
            break;
        case HORIZON_EVENT_RACE_START: call(s, s.cb[CbOnRaceStart], "OnRaceStart()", cfg.callLimitMs, false, nullptr, nullptr); break;
        case HORIZON_EVENT_CHECKPOINT:
            a = {e.time_ms, e.checkpoint_index};
            call(s, s.cb[CbOnCheckpoint], "OnCheckpoint()", cfg.callLimitMs, false, argsInt2, &a);
            break;
        case HORIZON_EVENT_RESPAWN:
            a = {e.respawns, 0};
            call(s, s.cb[CbOnRespawn], "OnRespawn()", cfg.callLimitMs, false, argsInt1, &a);
            break;
        case HORIZON_EVENT_FINISH:
            a = {e.time_ms, e.is_pb ? 1 : 0};
            call(s, s.cb[CbOnFinish], "OnFinish()", cfg.callLimitMs, false, argsIntBool, &a);
            break;
        case HORIZON_EVENT_RACE_END: call(s, s.cb[CbOnRaceEnd], "OnRaceEnd()", cfg.callLimitMs, false, nullptr, nullptr); break;
        case HORIZON_EVENT_MENU_ENTERED: call(s, s.cb[CbOnMenu], "OnMenu()", cfg.callLimitMs, false, nullptr, nullptr); break;
        case HORIZON_EVENT_GAME_STATE:
            a = {static_cast<int>(e.game_state), 0};
            call(s, s.cb[CbOnGameState], "OnGameState()", cfg.callLimitMs, false, argsInt1, &a);
            break;
        default: break;
        }
    }
}

} // namespace hs
