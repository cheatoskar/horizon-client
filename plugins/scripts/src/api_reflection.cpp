// Reflection namespace and the Nod type: READ-ONLY access to the engine's objects by member NAME,
// through the Node Explorer's read layer (class catalog from the engine reflection, page-checked and
// SEH-guarded reads with a per-frame read limit). No writes, no getter or method calls, no hooks: it
// stays the same next to Competition Patch and online.
//
//   Nod challenge = Reflection::GetNod("App.Challenge");
//   string name = challenge.GetString("ChallengeName");
//   int time = Reflection::GetInt("App.CurrentPlayground.GameTerminals[0].ControlledPlayer.CurRaceTime");
#include <windows.h>
#include <string.h>

#include <new>
#include <string>
#include <vector>

#include "angelscript.h"
#include "api_types.h"
#include "catalog.h"
#include "engine.h"
#include "fsutil.h"
#include "path.h"
#include "reflect/reflect.h"
#include "safe_read.h"
#include "values.h"

namespace hs {
namespace {

struct Nod {
    uint32_t ptr; // a live engine object (checked again on every use), 0 = null
};

nx::Catalog g_cat;
bool g_catTried = false;
ULONGLONG g_catRetryAt = 0;
std::string g_lastError;

bool ensureCatalog() {
    if (g_cat.loaded()) return true;
    const ULONGLONG now = GetTickCount64();
    if (g_catTried && now < g_catRetryAt) return false;
    g_catTried = true;
    g_catRetryAt = now + 2000;
    Manager* m = currentManager();
    const HorizonHostApi* h = m ? m->host() : nullptr;
    const HorizonGameInfo* gi = h && h->game_info ? h->game_info() : nullptr;
    if (!gi) return false;
    const nx::Engine engine = (gi->game & HORIZON_GAME_FOREVER) ? nx::Engine::Forever : nx::Engine::Legacy;
    // reflect's per-exe table has link addresses; a relocated exe (TMLoader) or an unknown build goes
    // through the scan of the registration code instead (timestamp 0), like in the Node Explorer.
    const bool atLinkBase = gi->exe_base == 0x400000 || gi->exe_base == 0;
    auto tryLoad = [engine](uint32_t ts) { return reflect::init(ts) && reflect::ready() && g_cat.load(engine) && g_cat.byName("CMwNod") != nullptr; };
    if (!(atLinkBase && gi->exe_timestamp && tryLoad(gi->exe_timestamp)) && !tryLoad(0)) {
        g_cat = nx::Catalog();
        return false;
    }
    if (h) h->log(h->plugin, HORIZON_LOG_INFO, "reflection: %u classes", static_cast<unsigned>(g_cat.classes().size()));
    return true;
}

bool fail(const std::string& why) {
    g_lastError = nx::budgetHit() ? "the read limit for this frame is used up" : why;
    return false;
}

bool ready(const char* what) {
    if (!requirePerm(PermReadGame, what)) return false;
    if (!ensureCatalog()) return fail("the engine's class list is not available in this game build");
    return true;
}

// "Name" or "Name[3]" segments separated by '.'.
bool parseSteps(const std::string& text, std::vector<nx::Step>* out) {
    out->clear();
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('.', p);
        if (e == std::string::npos) e = text.size();
        std::string seg = text.substr(p, e - p);
        nx::Step st;
        const size_t br = seg.find('[');
        if (br != std::string::npos) {
            if (seg.back() != ']' || br + 2 > seg.size() - 1) return false;
            const std::string num = seg.substr(br + 1, seg.size() - br - 2);
            if (num.empty() || num.size() > 7 || num.find_first_not_of("0123456789") != std::string::npos) return false;
            st.index = atoi(num.c_str());
            seg = seg.substr(0, br);
        }
        if (seg.empty()) return false;
        st.member = seg;
        out->push_back(st);
        if (e == text.size()) break;
        p = e + 1;
    }
    return !out->empty();
}

uintptr_t rootObject(const std::string& root) {
    const uint32_t id = nx::rootEngineId(root);
    Manager* m = currentManager();
    const HorizonHostApi* h = m ? m->host() : nullptr;
    return id && h ? reinterpret_cast<uintptr_t>(h->engine_pointer(id)) : 0;
}

// Walks the first `count` steps (all nod members) from start. 0 on failure (g_lastError set).
uintptr_t walk(uintptr_t start, const std::vector<nx::Step>& steps, size_t count) {
    nx::Path p;
    p.root = "App"; // unused by resolvePath beyond rootObj
    p.rootObj = start;
    p.steps.assign(steps.begin(), steps.begin() + static_cast<ptrdiff_t>(count));
    int failed = -2;
    if (!nx::resolvePath(g_cat, &p, &failed)) {
        if (failed < 0) {
            fail("the start object is not available");
            return 0;
        }
        // say what went wrong at the failing step
        const nx::Step& st = p.steps[static_cast<size_t>(failed)];
        const uintptr_t before = failed == 0 ? start : p.steps[static_cast<size_t>(failed) - 1].obj;
        const nx::ClassEntry* c = g_cat.classOf(before);
        const nx::MemberEntry* m = c ? g_cat.findMember(c, st.member) : nullptr;
        if (!c) fail("the object is gone");
        else if (!m) fail(c->name + " has no member " + st.member);
        else if (m->ti.kind != nx::Kind::Nod) fail(st.member + " is not an object (use it as the last part of the path)");
        else if (m->offset < 0) fail(st.member + " is a getter (not read in this version)");
        else if (st.index >= 0) fail(st.member + "[" + std::to_string(st.index) + "] is past the end, null or not an engine object");
        else fail(st.member + " is null or not an engine object");
        return 0;
    }
    return p.object();
}

struct Value {
    nx::Kind kind = nx::Kind::Unknown;
    uint8_t raw[48] = {};
    size_t size = 0;
    std::string text; // strings, decoded
    uintptr_t nod = 0;
    int count = -1;   // list member without index
};

bool readString(uintptr_t addr, bool wide, std::string* out) {
    uint32_t w[2];
    if (!nx::readT(addr, &w)) return false;
    for (int order = 0; order < 2; order++) {
        const uint32_t len = order == 0 ? w[0] : w[1];
        const uint32_t ptr = order == 0 ? w[1] : w[0];
        if (len == 0) {
            out->clear();
            return true;
        }
        if (len > 4096 || ptr < 0x10000) continue;
        if (wide) {
            std::wstring buf(len, L'\0');
            if (!nx::read(ptr, &buf[0], len * sizeof(wchar_t))) return false;
            *out = narrow(buf);
        } else {
            out->assign(len, '\0');
            if (!nx::read(ptr, &(*out)[0], len)) return false;
        }
        return true;
    }
    return false;
}

// Reads member `st` of obj (with its list index). False with g_lastError.
bool readMember(uintptr_t obj, const nx::Step& st, Value* v) {
    const nx::ClassEntry* c = g_cat.classOf(obj);
    if (!c) return fail("the object is gone");
    const nx::MemberEntry* m = g_cat.findMember(c, st.member);
    if (!m) return fail(c->name + " has no member " + st.member);
    if (m->offset < 0) return fail(st.member + " is a getter or method (not read in this version)");
    if (m->ti.shape == nx::Shape::Array || m->ti.kind == nx::Kind::Unknown) return fail(st.member + " has a type that is not read yet");
    v->kind = m->ti.kind;
    uintptr_t at = obj + static_cast<uintptr_t>(m->offset);
    if (nx::isList(m->ti.shape)) {
        uint32_t hdr[2];
        if (!nx::readT(at, &hdr) || hdr[0] > 10000000u) return fail(st.member + " could not be read");
        if (st.index < 0) {
            v->count = static_cast<int>(hdr[0]);
            return true;
        }
        if (static_cast<uint32_t>(st.index) >= hdr[0]) return fail(st.member + "[" + std::to_string(st.index) + "] is past the end");
        at = hdr[1] + static_cast<uintptr_t>(st.index) * nx::elemSize(m->ti.kind);
    } else if (st.index >= 0) {
        return fail(st.member + " is not a list");
    }
    if (v->kind == nx::Kind::Nod) {
        uint32_t p = 0;
        if (!nx::readT(at, &p)) return fail(st.member + " could not be read");
        if (p && !g_cat.classOf(p)) return fail(st.member + " is not an engine object");
        v->nod = p;
        return true;
    }
    if (v->kind == nx::Kind::String || v->kind == nx::Kind::WString) {
        if (!readString(at, v->kind == nx::Kind::WString, &v->text)) return fail(st.member + " could not be read");
        return true;
    }
    v->size = nx::elemSize(v->kind);
    if (!v->size || v->size > sizeof(v->raw) || !nx::read(at, v->raw, v->size)) return fail(st.member + " could not be read");
    return true;
}

// Path relative to obj; the last step is read as a value.
bool readPath(uintptr_t obj, const std::string& path, Value* v) {
    std::vector<nx::Step> steps;
    if (!parseSteps(path, &steps)) return fail("bad path \"" + path + "\"");
    const uintptr_t holder = steps.size() > 1 ? walk(obj, steps, steps.size() - 1) : (g_cat.classOf(obj) ? obj : 0);
    if (!holder) {
        if (steps.size() == 1) fail("the object is gone");
        return false;
    }
    return readMember(holder, steps.back(), v);
}

// Absolute path: "App.Challenge.ChallengeName" (first segment = root).
bool readAbs(const std::string& path, Value* v, uintptr_t* rootOnly) {
    const size_t dot = path.find('.');
    const std::string root = path.substr(0, dot);
    const uintptr_t obj = rootObject(root);
    if (!nx::rootEngineId(root)) return fail("a path starts with App, Race or Challenge");
    if (!obj) return fail(root + " is not available right now");
    if (dot == std::string::npos) {
        *rootOnly = obj;
        return true;
    }
    return readPath(obj, path.substr(dot + 1), v);
}

bool toInt(const Value& v, int64_t* out) {
    switch (v.kind) {
    case nx::Kind::Int: *out = *reinterpret_cast<const int32_t*>(v.raw); return true;
    case nx::Kind::Bool:
    case nx::Kind::Natural:
    case nx::Kind::Enum:
    case nx::Kind::Id: *out = *reinterpret_cast<const uint32_t*>(v.raw); return true;
    case nx::Kind::Real: *out = static_cast<int64_t>(*reinterpret_cast<const float*>(v.raw)); return true;
    default: return false;
    }
}

bool toFloat(const Value& v, float* out) {
    int64_t i = 0;
    if (v.kind == nx::Kind::Real) {
        *out = *reinterpret_cast<const float*>(v.raw);
        return true;
    }
    if (toInt(v, &i)) {
        *out = static_cast<float>(i);
        return true;
    }
    return false;
}

std::string toText(const Value& v) {
    if (v.kind == nx::Kind::String || v.kind == nx::Kind::WString) return v.text;
    if (v.kind == nx::Kind::Nod) {
        const nx::ClassEntry* c = v.nod ? g_cat.classOf(v.nod) : nullptr;
        return c ? c->name : "null";
    }
    if (v.count >= 0) return std::to_string(v.count) + " items";
    return nx::fmtValue(v.kind, v.raw, v.size);
}

// ---- typed reads, absolute and on a Nod ----
template <typename T, typename F> T absRead(const std::string& path, const char* what, T fallback, F convert) {
    if (!ready(what)) return fallback;
    g_lastError.clear();
    Value v;
    uintptr_t rootOnly = 0;
    if (!readAbs(path, &v, &rootOnly)) return fallback;
    if (rootOnly) {
        v.kind = nx::Kind::Nod;
        v.nod = rootOnly;
    }
    T out = fallback;
    if (!convert(v, &out)) {
        fail(path + " has another type");
        return fallback;
    }
    return out;
}

template <typename T, typename F> T nodRead(const Nod* self, const std::string& path, const char* what, T fallback, F convert) {
    if (!ready(what)) return fallback;
    g_lastError.clear();
    if (!self->ptr) {
        fail("the Nod is null");
        return fallback;
    }
    Value v;
    if (!readPath(self->ptr, path, &v)) return fallback;
    T out = fallback;
    if (!convert(v, &out)) {
        fail(path + " has another type");
        return fallback;
    }
    return out;
}

bool cvInt(const Value& v, int* o) {
    int64_t i;
    if (!toInt(v, &i)) return false;
    *o = static_cast<int>(i);
    return true;
}
bool cvUint(const Value& v, uint32_t* o) {
    int64_t i;
    if (!toInt(v, &i)) return false;
    *o = static_cast<uint32_t>(i);
    return true;
}
bool cvFloat(const Value& v, float* o) { return toFloat(v, o); }
bool cvBool(const Value& v, bool* o) {
    int64_t i;
    if (!toInt(v, &i)) return false;
    *o = i != 0;
    return true;
}
bool cvString(const Value& v, std::string* o) {
    *o = toText(v);
    return true;
}
bool cvVec3(const Value& v, Vec3* o) {
    const float* f = reinterpret_cast<const float*>(v.raw);
    switch (v.kind) {
    case nx::Kind::Vec3:
    case nx::Kind::Color: *o = Vec3{f[0], f[1], f[2]}; return true;
    case nx::Kind::Vec2: *o = Vec3{f[0], f[1], 0}; return true;
    case nx::Kind::Iso4: *o = Vec3{f[9], f[10], f[11]}; return true; // position of the 3x4 matrix
    default: return false;
    }
}
bool cvNod(const Value& v, Nod* o) {
    if (v.kind != nx::Kind::Nod) return false;
    o->ptr = static_cast<uint32_t>(v.nod);
    return true;
}
bool cvCount(const Value& v, int* o) {
    if (v.count < 0) return false;
    *o = v.count;
    return true;
}

int absInt(const std::string& p) { return absRead<int>(p, "Reflection::GetInt", 0, cvInt); }
uint32_t absUint(const std::string& p) { return absRead<uint32_t>(p, "Reflection::GetUint", 0u, cvUint); }
float absFloat(const std::string& p) { return absRead<float>(p, "Reflection::GetFloat", 0.0f, cvFloat); }
bool absBool(const std::string& p) { return absRead<bool>(p, "Reflection::GetBool", false, cvBool); }
std::string absString(const std::string& p) { return absRead<std::string>(p, "Reflection::GetString", std::string(), cvString); }
Vec3 absVec3(const std::string& p) { return absRead<Vec3>(p, "Reflection::GetVec3", Vec3{0, 0, 0}, cvVec3); }
Nod absNod(const std::string& p) { return absRead<Nod>(p, "Reflection::GetNod", Nod{0}, cvNod); }
int absCount(const std::string& p) { return absRead<int>(p, "Reflection::GetCount", -1, cvCount); }

int nodInt(const std::string& p, const Nod* s) { return nodRead<int>(s, p, "Nod::GetInt", 0, cvInt); }
uint32_t nodUint(const std::string& p, const Nod* s) { return nodRead<uint32_t>(s, p, "Nod::GetUint", 0u, cvUint); }
float nodFloat(const std::string& p, const Nod* s) { return nodRead<float>(s, p, "Nod::GetFloat", 0.0f, cvFloat); }
bool nodBool(const std::string& p, const Nod* s) { return nodRead<bool>(s, p, "Nod::GetBool", false, cvBool); }
std::string nodString(const std::string& p, const Nod* s) { return nodRead<std::string>(s, p, "Nod::GetString", std::string(), cvString); }
Vec3 nodVec3(const std::string& p, const Nod* s) { return nodRead<Vec3>(s, p, "Nod::GetVec3", Vec3{0, 0, 0}, cvVec3); }
Nod nodNod(const std::string& p, const Nod* s) { return nodRead<Nod>(s, p, "Nod::GetNod", Nod{0}, cvNod); }
int nodCount(const std::string& p, const Nod* s) { return nodRead<int>(s, p, "Nod::GetCount", -1, cvCount); }

Nod root(const std::string& name) {
    if (!ready("Reflection::Root")) return Nod{0};
    g_lastError.clear();
    if (!nx::rootEngineId(name)) {
        fail("roots are App, Race and Challenge");
        return Nod{0};
    }
    const uintptr_t p = rootObject(name);
    if (!p || !g_cat.classOf(p)) {
        fail(name + " is not available right now");
        return Nod{0};
    }
    return Nod{static_cast<uint32_t>(p)};
}

bool nodIsNull(const Nod* s) { return s->ptr == 0; }
std::string nodClassName(const Nod* s) {
    if (!ready("Nod::ClassName") || !s->ptr) return std::string();
    const nx::ClassEntry* c = g_cat.classOf(s->ptr);
    return c ? c->name : std::string();
}
bool nodIsKindOf(const std::string& cls, const Nod* s) {
    if (!ready("Nod::IsKindOf") || !s->ptr) return false;
    for (const nx::ClassEntry* c = g_cat.classOf(s->ptr); c; c = c->parent)
        if (c->name == cls) return true;
    return false;
}
bool nodEquals(const Nod& o, const Nod* s) { return o.ptr == s->ptr; }
void nodDefault(Nod* self) { new (self) Nod{0}; }

bool available() {
    if (!requirePerm(PermReadGame, "Reflection::Available")) return false;
    return ensureCatalog();
}
std::string lastError() { return g_lastError; }

} // namespace

void reflectionBeginFrame(int readLimit) { nx::beginFrame(readLimit); }

void reflectionShutdown() {
    g_cat = nx::Catalog();
    g_catTried = false;
    g_lastError.clear();
}

#define R(x)                                                                                                                                         \
    do {                                                                                                                                             \
        if ((x) < 0) return false;                                                                                                                   \
    } while (0)

bool registerReflectionApi(asIScriptEngine* e) {
    R(e->RegisterObjectType("Nod", sizeof(Nod), asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_ALLINTS | asGetTypeTraits<Nod>()));
    R(e->RegisterObjectBehaviour("Nod", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(nodDefault), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("Nod", "bool IsNull() const", asFUNCTION(nodIsNull), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("Nod", "string ClassName() const", asFUNCTION(nodClassName), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("Nod", "bool IsKindOf(const string &in className) const", asFUNCTION(nodIsKindOf), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("Nod", "bool opEquals(const Nod &in) const", asFUNCTION(nodEquals), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("Nod", "Nod GetNod(const string &in path) const", asFUNCTION(nodNod), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("Nod", "int GetInt(const string &in path) const", asFUNCTION(nodInt), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("Nod", "uint GetUint(const string &in path) const", asFUNCTION(nodUint), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("Nod", "float GetFloat(const string &in path) const", asFUNCTION(nodFloat), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("Nod", "bool GetBool(const string &in path) const", asFUNCTION(nodBool), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("Nod", "string GetString(const string &in path) const", asFUNCTION(nodString), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("Nod", "vec3 GetVec3(const string &in path) const", asFUNCTION(nodVec3), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("Nod", "int GetCount(const string &in path) const", asFUNCTION(nodCount), asCALL_CDECL_OBJLAST));

    R(e->SetDefaultNamespace("Reflection"));
    R(e->RegisterGlobalFunction("bool Available()", asFUNCTION(available), asCALL_CDECL));
    R(e->RegisterGlobalFunction("string LastError()", asFUNCTION(lastError), asCALL_CDECL));
    R(e->RegisterGlobalFunction("Nod Root(const string &in name)", asFUNCTION(root), asCALL_CDECL));
    R(e->RegisterGlobalFunction("Nod GetNod(const string &in path)", asFUNCTION(absNod), asCALL_CDECL));
    R(e->RegisterGlobalFunction("int GetInt(const string &in path)", asFUNCTION(absInt), asCALL_CDECL));
    R(e->RegisterGlobalFunction("uint GetUint(const string &in path)", asFUNCTION(absUint), asCALL_CDECL));
    R(e->RegisterGlobalFunction("float GetFloat(const string &in path)", asFUNCTION(absFloat), asCALL_CDECL));
    R(e->RegisterGlobalFunction("bool GetBool(const string &in path)", asFUNCTION(absBool), asCALL_CDECL));
    R(e->RegisterGlobalFunction("string GetString(const string &in path)", asFUNCTION(absString), asCALL_CDECL));
    R(e->RegisterGlobalFunction("vec3 GetVec3(const string &in path)", asFUNCTION(absVec3), asCALL_CDECL));
    R(e->RegisterGlobalFunction("int GetCount(const string &in path)", asFUNCTION(absCount), asCALL_CDECL));
    R(e->SetDefaultNamespace(""));
    return true;
}

} // namespace hs
