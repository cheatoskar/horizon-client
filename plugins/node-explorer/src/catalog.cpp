#include "catalog.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "reflect/reflect.h"
#include "safe_read.h"

namespace nx {
namespace {

struct Raw {
    const reflect::ClassInfo* ci;
    const reflect::ClassInfo* parent;
    std::string name;
    uint32_t id;
};

bool collect(const reflect::ClassInfo* c, void* user) {
    auto* out = static_cast<std::vector<Raw>*>(user);
    const char* n = reflect::className(c);
    Raw r;
    r.ci = c;
    r.parent = reflect::parent(c);
    r.id = reflect::classId(c);
    if (n && *n) {
        r.name = n;
    } else {
        char b[40];
        snprintf(b, sizeof(b), "(unnamed %08X)", r.id);
        r.name = b;
    }
    out->push_back(r);
    return out->size() < 20000;
}

const char* shapeName(Shape s) {
    switch (s) {
    case Shape::Buffer: return "buffer";
    case Shape::BufferCat: return "buffercat";
    case Shape::Array: return "array";
    case Shape::Range: return "range";
    default: return "single";
    }
}

void jsonString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            out += b;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
}

const size_t kMaxList = 10000000;

bool readFailed(Row& r, ObjectView* out) {
    if (budgetHit()) {
        r.state = RowState::Skipped;
        r.value = "-";
        out->skipped++;
    } else {
        r.state = RowState::Unreadable;
        r.value = "unreadable";
    }
    return false;
}

// CFastString / CFastStringInt: {length, pointer}. Falls back to {pointer, length} if the first
// order does not look like a string. Returns false when unreadable (or the budget ran out).
bool readString(uintptr_t addr, bool wide, size_t max, std::string* text) {
    uint32_t w[2];
    if (!readT(addr, &w)) return false;
    for (int order = 0; order < 2; order++) {
        const uint32_t len = order == 0 ? w[0] : w[1];
        const uint32_t ptr = order == 0 ? w[1] : w[0];
        if (len == 0) {
            *text = "\"\"";
            return true;
        }
        if (len > (1u << 20) || ptr < 0x10000) continue;
        const size_t n = len < max ? len : max;
        if (wide) {
            std::vector<wchar_t> buf(n);
            if (!read(ptr, buf.data(), n * sizeof(wchar_t))) return false;
            *text = fmtWText(buf.data(), n, n) + (n < len ? "..." : "");
        } else {
            std::vector<char> buf(n);
            if (!read(ptr, buf.data(), n)) return false;
            *text = fmtText(buf.data(), n, n) + (n < len ? "..." : "");
        }
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------- catalog -----------------------

bool Catalog::load(Engine engine) {
    engine_ = engine;
    classes_.clear();
    byInfo_.clear();
    byName_.clear();
    members_.clear();
    std::vector<Raw> raw;
    reflect::forEachClass(collect, &raw);
    if (raw.empty()) return false;
    std::sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) { return a.name < b.name; });
    classes_.resize(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        classes_[i].ci = raw[i].ci;
        classes_[i].name = raw[i].name;
        classes_[i].id = raw[i].id;
        classes_[i].memberCount = reflect::paramCount(raw[i].ci);
        byInfo_[raw[i].ci] = i;
        if (!byName_.count(raw[i].name)) byName_[raw[i].name] = i;
    }
    for (size_t i = 0; i < raw.size(); i++) classes_[i].parent = byInfo(raw[i].parent);
    return true;
}

const ClassEntry* Catalog::byName(const std::string& name) const {
    auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : &classes_[it->second];
}

const ClassEntry* Catalog::byInfo(const void* ci) const {
    if (!ci) return nullptr;
    auto it = byInfo_.find(ci);
    return it == byInfo_.end() ? nullptr : &classes_[it->second];
}

const std::vector<MemberEntry>& Catalog::members(const ClassEntry* c) {
    static const std::vector<MemberEntry> kNone;
    if (!c) return kNone;
    auto it = members_.find(c);
    if (it != members_.end()) return it->second;
    std::vector<MemberEntry>& v = members_[c];
    const size_t n = reflect::paramCount(c->ci);
    for (size_t i = 0; i < n; i++) {
        const reflect::ParamInfo* p = reflect::paramAt(c->ci, i);
        if (!p) continue;
        MemberEntry m;
        const char* name = reflect::memberName(p);
        m.name = (name && *name) ? name : "?";
        m.type = reflect::memberType(p);
        m.ti = classify(m.type, engine_);
        m.offset = isMethod(m.ti.kind) ? -1 : reflect::fieldOffset(p);
        m.owner = c;
        if (m.ti.kind == Kind::Nod) {
            // The member info names the nod's class: +0x24 in ESWC/Sunrise, +0x1c in Forever. Only
            // accepted when it is one of the registered classes.
            for (uintptr_t off : {static_cast<uintptr_t>(0x24), static_cast<uintptr_t>(0x1c)}) {
                uint32_t ci = 0;
                if (rawReadT(reinterpret_cast<uintptr_t>(p) + off, &ci) && (m.declared = byInfo(reinterpret_cast<const void*>(static_cast<uintptr_t>(ci)))) != nullptr)
                    break;
            }
        }
        v.push_back(m);
    }
    return v;
}

std::vector<const ClassEntry*> Catalog::chain(const ClassEntry* c) const {
    std::vector<const ClassEntry*> v;
    for (int depth = 0; c && depth < 32; c = c->parent, depth++) v.push_back(c);
    return v;
}

const MemberEntry* Catalog::findMember(const ClassEntry* c, const std::string& name) {
    for (const ClassEntry* k : chain(c))
        for (const MemberEntry& m : members(k))
            if (m.name == name) return &m;
    return nullptr;
}

const ClassEntry* Catalog::classOf(uintptr_t obj) const {
    if (obj < 0x10000 || (obj & 3)) return nullptr;
    uint32_t vtable = 0;
    if (!readT(obj, &vtable) || vtable < 0x10000) return nullptr;
    uint32_t slots[4];
    if (!readT(vtable, &slots)) return nullptr;
    for (uint32_t fn : slots) {
        uint8_t code[6];
        if (fn < 0x10000 || !read(fn, code, sizeof(code))) continue;
        if (code[0] != 0xb8 || code[5] != 0xc3) continue; // mov eax, imm32; ret
        uint32_t ci;
        memcpy(&ci, code + 1, 4);
        if (const ClassEntry* c = byInfo(reinterpret_cast<const void*>(static_cast<uintptr_t>(ci)))) return c;
    }
    return nullptr;
}

// ---------------------------------------------------------------- inspector ---------------------

bool readObject(Catalog& cat, uintptr_t obj, const ReadOptions& opt, ObjectView* out) {
    const int before = readsUsed();
    *out = ObjectView();
    out->obj = obj;
    out->cls = cat.classOf(obj);
    if (!out->cls) {
        out->reads = readsUsed() - before;
        return false;
    }
    for (const ClassEntry* c : cat.chain(out->cls)) {
        for (const MemberEntry& m : cat.members(c)) {
            if (isMethod(m.ti.kind) && !opt.methods) continue;
            Row r;
            r.m = &m;
            const Kind k = m.ti.kind;
            if (isMethod(k)) {
                r.state = RowState::Method;
                r.value = "method (not called)";
            } else if (m.offset < 0) {
                r.state = RowState::Getter;
                r.value = "getter (not read)";
            } else if (m.ti.shape == Shape::Array || k == Kind::Unknown) {
                r.state = RowState::Unsupported;
                r.value = m.ti.shape == Shape::Array ? "array (layout unknown)" : "type " + std::to_string(m.type);
            } else if (isList(m.ti.shape)) {
                uint32_t hdr[2]; // {count, items}
                if (!readT(obj + static_cast<uintptr_t>(m.offset), &hdr)) {
                    readFailed(r, out);
                } else if (hdr[0] > kMaxList || (hdr[0] && hdr[1] < 0x10000)) {
                    r.state = RowState::Unreadable;
                    r.value = "not a list (" + fmtHex(hdr[0]) + ", " + fmtHex(hdr[1]) + ")";
                } else {
                    r.count = static_cast<int>(hdr[0]);
                    r.value = r.count == 1 ? "1 item" : std::to_string(r.count) + " items";
                    const size_t es = elemSize(k);
                    const int shown = r.count < opt.listItems ? r.count : opt.listItems;
                    for (int i = 0; i < shown && es; i++) {
                        const uintptr_t a = hdr[1] + static_cast<uintptr_t>(i) * es;
                        ItemRow it;
                        if (k == Kind::Nod) {
                            uint32_t p = 0;
                            if (!readT(a, &p)) break;
                            it.nod = p;
                            it.nodClass = p ? cat.classOf(p) : nullptr;
                            it.text = !p ? "null" : it.nodClass ? it.nodClass->name : fmtPtr(p) + " (not a nod)";
                            if (!it.nodClass) it.nod = 0;
                        } else if (k == Kind::String || k == Kind::WString) {
                            if (!readString(a, k == Kind::WString, opt.textMax, &it.text)) it.text = budgetHit() ? "-" : "unreadable";
                        } else {
                            uint8_t buf[48];
                            if (!read(a, buf, es)) break;
                            it.text = fmtValue(k, buf, es);
                        }
                        r.items.push_back(it);
                    }
                    if (static_cast<int>(r.items.size()) < shown && budgetHit()) out->skipped++;
                }
            } else if (k == Kind::Nod) {
                uint32_t p = 0;
                if (!readT(obj + static_cast<uintptr_t>(m.offset), &p)) {
                    readFailed(r, out);
                } else if (!p) {
                    r.value = "null";
                } else if ((r.nodClass = cat.classOf(p)) != nullptr) {
                    r.nod = p;
                    r.value = r.nodClass->name;
                    r.tip = fmtPtr(p);
                } else {
                    r.value = fmtPtr(p) + " (not a nod)";
                }
            } else if (k == Kind::String || k == Kind::WString) {
                if (!readString(obj + static_cast<uintptr_t>(m.offset), k == Kind::WString, opt.textMax, &r.value)) readFailed(r, out);
            } else {
                const size_t es = elemSize(k);
                uint8_t buf[48];
                if (!es || !read(obj + static_cast<uintptr_t>(m.offset), buf, es)) {
                    readFailed(r, out);
                } else {
                    r.value = fmtValue(k, buf, es);
                    r.tip = fmtValueLong(k, buf, es);
                    if (k == Kind::Color) {
                        memcpy(r.color, buf, sizeof(r.color));
                        r.hasColor = true;
                    }
                }
            }
            out->rows.push_back(r);
        }
    }
    out->reads = readsUsed() - before;
    return true;
}

bool resolvePath(Catalog& cat, Path* path, int* failedStep) {
    *failedStep = -1;
    uintptr_t obj = path->rootObj;
    if (!obj || !cat.classOf(obj)) return false;
    for (size_t i = 0; i < path->steps.size(); i++) {
        Step& s = path->steps[i];
        *failedStep = static_cast<int>(i);
        s.obj = 0;
        const ClassEntry* c = cat.classOf(obj);
        const MemberEntry* m = c ? cat.findMember(c, s.member) : nullptr;
        if (!m || m->ti.kind != Kind::Nod || m->offset < 0) return false;
        const uintptr_t at = obj + static_cast<uintptr_t>(m->offset);
        uint32_t p = 0;
        if (s.index < 0) {
            if (m->ti.shape != Shape::Single || !readT(at, &p)) return false;
        } else {
            uint32_t hdr[2];
            if (!isList(m->ti.shape) || !readT(at, &hdr) || hdr[0] > kMaxList || static_cast<uint32_t>(s.index) >= hdr[0]) return false;
            if (!readT(hdr[1] + static_cast<uintptr_t>(s.index) * 4u, &p)) return false;
        }
        if (!p || !cat.classOf(p)) return false;
        s.obj = p;
        obj = p;
    }
    *failedStep = -2; // none
    return true;
}

// ---------------------------------------------------------------- dump --------------------------

int dumpClasses(Catalog& cat, const DumpInfo& info, const std::wstring& path) {
    if (!cat.loaded()) return -1;
    std::string j = "{\n  \"format\": 1,\n  \"source\": \"Horizon Node Explorer (runtime reflection, read-only)\",\n  \"game\": ";
    jsonString(j, info.game);
    j += ",\n  \"exe_version\": ";
    jsonString(j, info.exeVersion);
    j += ",\n  \"exe_sha256\": ";
    jsonString(j, info.exeSha256);
    char b[64];
    snprintf(b, sizeof(b), ",\n  \"exe_timestamp\": \"%08x\"", info.exeTimestamp);
    j += b;
    j += ",\n  \"host_version\": ";
    jsonString(j, info.hostVersion);
    j += ",\n  \"type_numbers\": ";
    jsonString(j, cat.engine() == Engine::Forever ? "forever" : "eswc-sunrise");
    j += ",\n  \"classes\": [";
    int n = 0;
    for (const ClassEntry& c : cat.classes()) {
        j += n ? ",\n    {" : "\n    {";
        j += "\"name\": ";
        jsonString(j, c.name);
        snprintf(b, sizeof(b), ", \"id\": \"%08x\", \"parent\": ", c.id);
        j += b;
        if (c.parent) jsonString(j, c.parent->name);
        else j += "null";
        j += ", \"members\": [";
        bool first = true;
        for (const MemberEntry& m : cat.members(&c)) {
            j += first ? "\n      {" : ",\n      {";
            first = false;
            j += "\"name\": ";
            jsonString(j, m.name);
            snprintf(b, sizeof(b), ", \"type\": %u, \"kind\": \"%s\", \"shape\": \"%s\", \"offset\": %d", m.type, kindName(m.ti.kind),
                     shapeName(m.ti.shape), m.offset);
            j += b;
            if (m.declared) {
                j += ", \"class\": ";
                jsonString(j, m.declared->name);
            }
            j += "}";
        }
        j += first ? "]}" : "\n    ]}";
        n++;
    }
    j += "\n  ]\n}\n";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return -1;
    const bool ok = fwrite(j.data(), 1, j.size(), f) == j.size();
    fclose(f);
    return ok ? n : -1;
}

} // namespace nx
