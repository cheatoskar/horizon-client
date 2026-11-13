// Core script API: string, array<T>, math (AngelScript add-ons), vec2/vec3/vec4, print/Log, Time.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <new>
#include <string>

#include "angelscript.h"
#include "api_types.h"
#include "engine.h"
#include "scriptarray/scriptarray.h"
#include "scriptmath/scriptmath.h"
#include "scriptstdstring/scriptstdstring.h"

namespace hs {
namespace {

// ---- vectors ----
void v2Init(float x, float y, Vec2* self) { new (self) Vec2{x, y}; }
void v2Default(Vec2* self) { new (self) Vec2{0, 0}; }
void v3Init(float x, float y, float z, Vec3* self) { new (self) Vec3{x, y, z}; }
void v3Default(Vec3* self) { new (self) Vec3{0, 0, 0}; }
void v4Init(float x, float y, float z, float w, Vec4* self) { new (self) Vec4{x, y, z, w}; }
void v4Default(Vec4* self) { new (self) Vec4{0, 0, 0, 0}; }

Vec2 v2Add(const Vec2& o, const Vec2* a) { return Vec2{a->x + o.x, a->y + o.y}; }
Vec2 v2Sub(const Vec2& o, const Vec2* a) { return Vec2{a->x - o.x, a->y - o.y}; }
Vec2 v2Mul(float f, const Vec2* a) { return Vec2{a->x * f, a->y * f}; }
Vec3 v3Add(const Vec3& o, const Vec3* a) { return Vec3{a->x + o.x, a->y + o.y, a->z + o.z}; }
Vec3 v3Sub(const Vec3& o, const Vec3* a) { return Vec3{a->x - o.x, a->y - o.y, a->z - o.z}; }
Vec3 v3Mul(float f, const Vec3* a) { return Vec3{a->x * f, a->y * f, a->z * f}; }
float v3Length(const Vec3* a) { return sqrtf(a->x * a->x + a->y * a->y + a->z * a->z); }
float v2Length(const Vec2* a) { return sqrtf(a->x * a->x + a->y * a->y); }
Vec4 v4Mul(float f, const Vec4* a) { return Vec4{a->x * f, a->y * f, a->z * f, a->w * f}; }

// ---- log ----
void logAt(int level, const std::string& text) {
    Manager* m = currentManager();
    Script* s = currentScript();
    if (m && s) m->scriptLog(*s, level, text);
}
void print(const std::string& t) { logAt(HORIZON_LOG_INFO, t); }
void warn(const std::string& t) { logAt(HORIZON_LOG_WARN, t); }
void error(const std::string& t) { logAt(HORIZON_LOG_ERROR, t); }

// ---- time ----
uint64_t timeNow() {
    Manager* m = currentManager();
    return m ? m->nowMs - m->startMs : 0;
}
float timeDelta() {
    Manager* m = currentManager();
    return m ? m->frameMs : 0.0f;
}
int timeRace() {
    Manager* m = currentManager();
    if (!m || m->raceStartMs < 0) return -1;
    return static_cast<int>(static_cast<int64_t>(m->nowMs) - m->raceStartMs);
}
uint64_t timeScript() {
    Manager* m = currentManager();
    Script* s = currentScript();
    return m && s ? m->nowMs - s->loadedAtMs : 0;
}
int64_t timeStamp() { return static_cast<int64_t>(::time(nullptr)); }

// Local wall-clock time with a strftime subset: %H %M %S %I %p %d %m %Y %y %a %A %b %B %j %%.
// Anything else is copied as is (no unchecked format string reaches the C runtime).
std::string timeFormat(const std::string& fmt) {
    const time_t now = ::time(nullptr);
    struct tm lt;
    if (localtime_s(&lt, &now) != 0) return std::string();
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) {
            out += fmt[i];
            continue;
        }
        const char c = fmt[++i];
        if (!strchr("HMSIpdmYyaAbBj%", c)) {
            out += '%';
            out += c;
            continue;
        }
        char spec[3] = {'%', c, 0};
        char b[64];
        const size_t n = strftime(b, sizeof(b), spec, &lt);
        out.append(b, n);
        if (out.size() > 256) break;
    }
    return out;
}

// Race time like the game shows it: "0:42.37", "1:02:03.45" over an hour; -1 (unknown) -> "-:--.--".
std::string formatRace(int ms) {
    if (ms < 0) return "-:--.--";
    const int cs = (ms / 10) % 100, s = (ms / 1000) % 60, m = (ms / 60000) % 60, h = ms / 3600000;
    char b[32];
    if (h) snprintf(b, sizeof(b), "%d:%02d:%02d.%02d", h, m, s, cs);
    else snprintf(b, sizeof(b), "%d:%02d.%02d", m, s, cs);
    return b;
}

// Durations: "12:34" under an hour, "1:02:03" above.
std::string formatDuration(uint64_t ms) {
    const uint64_t total = ms / 1000;
    const unsigned s = static_cast<unsigned>(total % 60), m = static_cast<unsigned>((total / 60) % 60);
    const unsigned long long h = total / 3600;
    char b[40];
    if (h) snprintf(b, sizeof(b), "%llu:%02u:%02u", h, m, s);
    else snprintf(b, sizeof(b), "%u:%02u", m, s);
    return b;
}

} // namespace

#define R(x)                                                                                                                                         \
    do {                                                                                                                                             \
        if ((x) < 0) return false;                                                                                                                   \
    } while (0)

bool registerCoreApi(asIScriptEngine* e) {
    RegisterStdString(e);
    RegisterScriptArray(e, true);
    RegisterStdStringUtils(e);
    RegisterScriptMath(e);

    R(e->RegisterObjectType("vec2", sizeof(Vec2), asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_ALLFLOATS | asGetTypeTraits<Vec2>()));
    R(e->RegisterObjectBehaviour("vec2", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(v2Default), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectBehaviour("vec2", asBEHAVE_CONSTRUCT, "void f(float x, float y)", asFUNCTION(v2Init), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectProperty("vec2", "float x", asOFFSET(Vec2, x)));
    R(e->RegisterObjectProperty("vec2", "float y", asOFFSET(Vec2, y)));
    R(e->RegisterObjectMethod("vec2", "vec2 opAdd(const vec2 &in) const", asFUNCTION(v2Add), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("vec2", "vec2 opSub(const vec2 &in) const", asFUNCTION(v2Sub), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("vec2", "vec2 opMul(float) const", asFUNCTION(v2Mul), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("vec2", "float Length() const", asFUNCTION(v2Length), asCALL_CDECL_OBJLAST));

    R(e->RegisterObjectType("vec3", sizeof(Vec3), asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_ALLFLOATS | asGetTypeTraits<Vec3>()));
    R(e->RegisterObjectBehaviour("vec3", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(v3Default), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectBehaviour("vec3", asBEHAVE_CONSTRUCT, "void f(float x, float y, float z)", asFUNCTION(v3Init), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectProperty("vec3", "float x", asOFFSET(Vec3, x)));
    R(e->RegisterObjectProperty("vec3", "float y", asOFFSET(Vec3, y)));
    R(e->RegisterObjectProperty("vec3", "float z", asOFFSET(Vec3, z)));
    R(e->RegisterObjectMethod("vec3", "vec3 opAdd(const vec3 &in) const", asFUNCTION(v3Add), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("vec3", "vec3 opSub(const vec3 &in) const", asFUNCTION(v3Sub), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("vec3", "vec3 opMul(float) const", asFUNCTION(v3Mul), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectMethod("vec3", "float Length() const", asFUNCTION(v3Length), asCALL_CDECL_OBJLAST));

    R(e->RegisterObjectType("vec4", sizeof(Vec4), asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_ALLFLOATS | asGetTypeTraits<Vec4>()));
    R(e->RegisterObjectBehaviour("vec4", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(v4Default), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectBehaviour("vec4", asBEHAVE_CONSTRUCT, "void f(float x, float y, float z, float w)", asFUNCTION(v4Init), asCALL_CDECL_OBJLAST));
    R(e->RegisterObjectProperty("vec4", "float x", asOFFSET(Vec4, x)));
    R(e->RegisterObjectProperty("vec4", "float y", asOFFSET(Vec4, y)));
    R(e->RegisterObjectProperty("vec4", "float z", asOFFSET(Vec4, z)));
    R(e->RegisterObjectProperty("vec4", "float w", asOFFSET(Vec4, w)));
    R(e->RegisterObjectMethod("vec4", "vec4 opMul(float) const", asFUNCTION(v4Mul), asCALL_CDECL_OBJLAST));

    // log: global print/warn/error (familiar from Openplanet) and the Log namespace
    R(e->RegisterGlobalFunction("void print(const string &in text)", asFUNCTION(print), asCALL_CDECL));
    R(e->RegisterGlobalFunction("void warn(const string &in text)", asFUNCTION(warn), asCALL_CDECL));
    R(e->RegisterGlobalFunction("void error(const string &in text)", asFUNCTION(error), asCALL_CDECL));
    R(e->SetDefaultNamespace("Log"));
    R(e->RegisterGlobalFunction("void Info(const string &in text)", asFUNCTION(print), asCALL_CDECL));
    R(e->RegisterGlobalFunction("void Warn(const string &in text)", asFUNCTION(warn), asCALL_CDECL));
    R(e->RegisterGlobalFunction("void Error(const string &in text)", asFUNCTION(error), asCALL_CDECL));

    R(e->SetDefaultNamespace("Time"));
    R(e->RegisterGlobalFunction("uint64 Now()", asFUNCTION(timeNow), asCALL_CDECL));
    R(e->RegisterGlobalFunction("uint64 ScriptTime()", asFUNCTION(timeScript), asCALL_CDECL));
    R(e->RegisterGlobalFunction("float Delta()", asFUNCTION(timeDelta), asCALL_CDECL));
    R(e->RegisterGlobalFunction("int RaceTime()", asFUNCTION(timeRace), asCALL_CDECL));
    R(e->RegisterGlobalFunction("int64 Stamp()", asFUNCTION(timeStamp), asCALL_CDECL));
    R(e->RegisterGlobalFunction("string Format(const string &in format)", asFUNCTION(timeFormat), asCALL_CDECL));
    R(e->RegisterGlobalFunction("string FormatRace(int ms)", asFUNCTION(formatRace), asCALL_CDECL));
    R(e->RegisterGlobalFunction("string FormatDuration(uint64 ms)", asFUNCTION(formatDuration), asCALL_CDECL));
    R(e->SetDefaultNamespace(""));
    return true;
}

} // namespace hs
