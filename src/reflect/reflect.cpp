#include "reflect.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace reflect {
namespace {

// CMwClassInfo: +04 class id, +08 parent, +14 name, +18 next, +1c factory, +20 params, +24 count
// SMwParamInfo: +00 type, +04 member id, +0c field offset (-1 = none), +10 name, +24 method (ESWC/Sunrise)
struct Exe {
    uint32_t timestamp;
    uintptr_t first; // CMwEngineManager::First
    bool methodPtrAt24;
};
const Exe kExes[] = {
    {0x5ce7272f, 0x00c96100, true},  // TMN ESWC unofficial 1.80
    {0x5cc8f0cf, 0x00b8abec, true},  // TM Sunrise Win10 repack
    {0x4d431494, 0x00d73ba8, false}, // TMNF (methods use another layout)
};
const Exe* g_exe = nullptr;

template <typename T> bool readAt(const void* base, size_t off, T* out) {
    __try {
        *out = *reinterpret_cast<const T*>(static_cast<const char*>(base) + off);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* safeString(const char* s) {
    if (!s) return nullptr;
    __try {
        for (int i = 0; i < 256; i++)
            if (s[i] == 0) return s;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

bool equals(const char* a, const char* b) {
    __try {
        return a && strcmp(a, b) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const ClassInfo* firstClass() {
    const ClassInfo* c = nullptr;
    if (!g_exe || !readAt(reinterpret_cast<const void*>(g_exe->first), 0, &c)) return nullptr;
    return c;
}

const ClassInfo* nextClass(const ClassInfo* c) {
    const ClassInfo* n = nullptr;
    return readAt(c, 0x18, &n) ? n : nullptr;
}

void jsonString(FILE* f, const char* s) {
    fputc('"', f);
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        if (static_cast<unsigned char>(*s) >= 0x20) fputc(*s, f);
    }
    fputc('"', f);
}

} // namespace

bool init(uint32_t exeTimestamp) {
    g_exe = nullptr;
    for (const Exe& e : kExes)
        if (e.timestamp == exeTimestamp) g_exe = &e;
    return g_exe != nullptr;
}

bool ready() { return firstClass() != nullptr; }

const char* className(const ClassInfo* c) {
    const char* n = nullptr;
    return readAt(c, 0x14, &n) ? safeString(n) : nullptr;
}

uint32_t classId(const ClassInfo* c) {
    uint32_t id = 0;
    readAt(c, 0x04, &id);
    return id;
}

const ClassInfo* parent(const ClassInfo* c) {
    const ClassInfo* p = nullptr;
    return readAt(c, 0x08, &p) ? p : nullptr;
}

size_t paramCount(const ClassInfo* c) {
    uint32_t n = 0;
    return readAt(c, 0x24, &n) && n < 0x1000 ? n : 0;
}

const ParamInfo* paramAt(const ClassInfo* c, size_t i) {
    const ParamInfo* const* arr = nullptr;
    const ParamInfo* p = nullptr;
    if (i >= paramCount(c) || !readAt(c, 0x20, &arr) || !arr) return nullptr;
    return readAt(arr, i * sizeof(void*), &p) ? p : nullptr;
}

const char* memberName(const ParamInfo* p) {
    const char* n = nullptr;
    return readAt(p, 0x10, &n) ? safeString(n) : nullptr;
}

uint32_t memberType(const ParamInfo* p) {
    uint32_t t = 0xffffffff;
    readAt(p, 0x00, &t);
    return t;
}

int fieldOffset(const ParamInfo* p) {
    uint32_t off = 0xffffffff;
    if (!readAt(p, 0x0c, &off) || off == 0xffffffff || off > 0x100000) return -1;
    return static_cast<int>(off);
}

void* methodAddress(const ParamInfo* p) {
    if (!g_exe || !g_exe->methodPtrAt24 || memberType(p) != 0) return nullptr;
    void* fn = nullptr;
    return readAt(p, 0x24, &fn) ? fn : nullptr;
}

const ClassInfo* findClass(const char* name) {
    int guard = 0;
    for (const ClassInfo* c = firstClass(); c && guard < 10000; c = nextClass(c), guard++)
        if (equals(className(c), name)) return c;
    return nullptr;
}

const ParamInfo* findMember(const ClassInfo* c, const char* name) {
    for (int depth = 0; c && depth < 32; c = parent(c), depth++) {
        size_t n = paramCount(c);
        for (size_t i = 0; i < n; i++) {
            const ParamInfo* p = paramAt(c, i);
            if (p && equals(memberName(p), name)) return p;
        }
    }
    return nullptr;
}

int offsetOf(const char* cls, const char* member) {
    const ClassInfo* c = findClass(cls);
    const ParamInfo* p = c ? findMember(c, member) : nullptr;
    return p ? fieldOffset(p) : -1;
}

void* methodOf(const char* cls, const char* member) {
    const ClassInfo* c = findClass(cls);
    const ParamInfo* p = c ? findMember(c, member) : nullptr;
    return p ? methodAddress(p) : nullptr;
}

void forEachClass(bool (*fn)(const ClassInfo*, void*), void* user) {
    int guard = 0;
    for (const ClassInfo* c = firstClass(); c && guard < 10000; c = nextClass(c), guard++)
        if (!fn(c, user)) return;
}

int dumpJson(const wchar_t* path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return -1;
    int classes = 0;
    fputs("[\n", f);
    int guard = 0;
    for (const ClassInfo* c = firstClass(); c && guard < 10000; c = nextClass(c), guard++) {
        const ClassInfo* par = parent(c);
        fprintf(f, "%s{\"id\":\"%08x\",\"name\":", classes ? ",\n" : "", classId(c));
        jsonString(f, className(c));
        fputs(",\"parent\":", f);
        jsonString(f, par ? className(par) : nullptr);
        fputs(",\"members\":[", f);
        size_t n = paramCount(c);
        for (size_t i = 0; i < n; i++) {
            const ParamInfo* p = paramAt(c, i);
            if (!p) continue;
            fprintf(f, "%s{\"name\":", i ? "," : "");
            jsonString(f, memberName(p));
            fprintf(f, ",\"type\":%u,\"offset\":%d", memberType(p), fieldOffset(p));
            if (void* m = methodAddress(p)) fprintf(f, ",\"method\":\"%08x\"", static_cast<unsigned>(reinterpret_cast<uintptr_t>(m)));
            fputc('}', f);
        }
        fputs("]}", f);
        classes++;
    }
    fputs("\n]\n", f);
    fclose(f);
    return classes;
}

} // namespace reflect
