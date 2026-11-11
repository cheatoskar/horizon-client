// Class catalog (from the engine reflection, via the vendored reflect module) and the read-only
// object inspector. Everything here only reads: no writes, no method or getter calls, no hooks.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "path.h"
#include "values.h"

namespace reflect {
struct ClassInfo;
struct ParamInfo;
} // namespace reflect

namespace nx {

struct ClassEntry;

struct MemberEntry {
    std::string name;
    uint32_t type = 0;       // raw SMwParamInfo type number
    TypeInfo ti;
    int offset = -1;         // field offset, -1: getter or method (never called here)
    const ClassEntry* owner = nullptr;
    const ClassEntry* declared = nullptr; // nod members: class named in the member info, if found
};

struct ClassEntry {
    const reflect::ClassInfo* ci = nullptr;
    std::string name;
    uint32_t id = 0;                     // shown for orientation only; lookups go by name
    const ClassEntry* parent = nullptr;
    size_t memberCount = 0;              // own members
};

class Catalog {
public:
    // Walks the class list once. False when the reflection is not available (unknown exe, too early).
    bool load(Engine engine);
    bool loaded() const { return !classes_.empty(); }
    Engine engine() const { return engine_; }
    const std::vector<ClassEntry>& classes() const { return classes_; } // sorted by name
    const ClassEntry* byName(const std::string& name) const;
    const ClassEntry* byInfo(const void* ci) const;
    // Own members of c in reflection order (cached on first use).
    const std::vector<MemberEntry>& members(const ClassEntry* c);
    // c, its parent, ... up to the root.
    std::vector<const ClassEntry*> chain(const ClassEntry* c) const;
    // Member by name in c or a parent (first match from c upwards).
    const MemberEntry* findMember(const ClassEntry* c, const std::string& name);
    // Class of a live object: vtable slot with `mov eax, imm32; ret` (MwGetClassInfo, like
    // reflect::classOfObject) whose imm32 is a class of this catalog. Budgeted reads. nullptr if not a nod.
    const ClassEntry* classOf(uintptr_t obj) const;

private:
    Engine engine_ = Engine::Legacy;
    std::vector<ClassEntry> classes_;
    std::unordered_map<const void*, size_t> byInfo_;
    std::unordered_map<std::string, size_t> byName_;
    std::unordered_map<const ClassEntry*, std::vector<MemberEntry>> members_;
};

// ---- object inspector ----

struct ItemRow {
    std::string text;                 // value, or class name for nods
    uintptr_t nod = 0;                // nod items: the object (0 if null or not a nod)
    const ClassEntry* nodClass = nullptr;
};

enum class RowState { Value, Getter, Method, Unreadable, Skipped, Unsupported };

struct Row {
    const MemberEntry* m = nullptr;
    RowState state = RowState::Value;
    std::string value;                // short text for the value column
    std::string tip;                  // longer text (matrices, hex)
    uintptr_t nod = 0;                // single nod member: the object
    const ClassEntry* nodClass = nullptr;
    int count = -1;                   // lists: number of items
    std::vector<ItemRow> items;       // lists: the first N items
    bool hasColor = false;
    float color[3] = {0, 0, 0};
};

struct ObjectView {
    uintptr_t obj = 0;
    const ClassEntry* cls = nullptr;
    std::vector<Row> rows;            // own class first, then the parents' members
    int skipped = 0;                  // rows not read because the read limit was reached
    int reads = 0;                    // reads this view took
};

struct ReadOptions {
    int listItems = 8;                // first N items of a list
    size_t textMax = 160;             // bytes of a string shown
    bool methods = false;             // include method rows
};

// Reads every member of obj (budgeted). False when obj is not a readable nod.
bool readObject(Catalog& cat, uintptr_t obj, const ReadOptions& opt, ObjectView* out);

// Re-walks path by member names from rootObj (rootObj set by the caller). Updates every step's obj.
// On failure returns false, *failedStep = index of the step that did not resolve (-1: the root).
bool resolvePath(Catalog& cat, Path* path, int* failedStep);

// Writes every class with its members as JSON (UTF-8): game, exe, type names. Returns the class count, -1 on error.
struct DumpInfo {
    std::string game, exeVersion, exeSha256, hostVersion;
    uint32_t exeTimestamp = 0;
};
int dumpClasses(Catalog& cat, const DumpInfo& info, const std::wstring& path);

} // namespace nx
