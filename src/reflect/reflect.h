// Runtime access to the Nadeo engine reflection (CMwClassInfo / SMwParamInfo) of 32-bit TrackMania games,
// by class name and member name. Host-agnostic: no game hooks, no allocations in the game, only reads.
// Layout and per-exe registry heads: tm-modding-knowledge/modding/reflection.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace reflect {

struct ClassInfo;  // CMwClassInfo (opaque)
struct ParamInfo;  // SMwParamInfo (opaque)

// Picks the registry head for the running exe (PE timestamp). False if the exe is unknown.
bool init(uint32_t exeTimestamp);
bool ready();

const ClassInfo* findClass(const char* className);
const char* className(const ClassInfo* c);
uint32_t classId(const ClassInfo* c);
const ClassInfo* parent(const ClassInfo* c);

// Member lookup, searching parent classes too. nullptr if not found.
const ParamInfo* findMember(const ClassInfo* c, const char* memberName);
const char* memberName(const ParamInfo* p);
uint32_t memberType(const ParamInfo* p);
// Field offset inside the object, or -1 when the member is not a plain field (methods, getters).
int fieldOffset(const ParamInfo* p);
// Method entry point for "proc" members (thiscall, no arguments) in the ESWC/Sunrise layout, else nullptr.
void* methodAddress(const ParamInfo* p);

// Shorthands: offset of className.memberName, -1 if missing; method address, nullptr if missing.
int offsetOf(const char* cls, const char* member);
void* methodOf(const char* cls, const char* member);

// Class of a live engine object (CMwNod*), read from vtable slot 2 (MwGetClassInfo, `mov eax, imm32; ret`)
// without calling anything. nullptr if obj is not a readable nod or the slot looks different.
const ClassInfo* classOfObject(const void* obj);
// True if c is the class `name` or derives from it.
bool isKindOf(const ClassInfo* c, const char* name);

// Calls fn(classInfo, user) for every registered class; stops when fn returns false.
void forEachClass(bool (*fn)(const ClassInfo*, void*), void* user);
size_t paramCount(const ClassInfo* c);
const ParamInfo* paramAt(const ClassInfo* c, size_t i);

// Writes all classes and members as JSON (UTF-8) to path. Returns the number of classes, -1 on error.
int dumpJson(const wchar_t* path);

} // namespace reflect
