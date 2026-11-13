// IO namespace: text files in the script's own data folder (horizon\data\scripts\<id>\), nothing else.
// Names are checked (safeDataName: no drive, no "..", no backslash); 1 MB per file, 16 MB and 256
// files per script.
#include <windows.h>

#include <algorithm>
#include <string>

#include "angelscript.h"
#include "engine.h"
#include "fsutil.h"
#include "scriptarray/scriptarray.h"

namespace hs {
namespace {

const size_t kMaxFile = 1024 * 1024;
const uint64_t kMaxFolder = 16ull * 1024 * 1024;
const size_t kMaxFiles = 256;

// Full path for a checked name, or "" (exception set) for a bad one.
std::wstring pathFor(const std::string& name, const char* what) {
    Script* s = currentScript();
    if (!s) return std::wstring();
    if (!safeDataName(name)) {
        scriptError(std::string(what) + ": \"" + name + "\" is not a valid file name (letters, digits, space, _ . -, '/' for sub-folders)");
        return std::wstring();
    }
    std::wstring rel = widen(name);
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    return s->dataFolder + rel;
}

std::string readText(const std::string& name) {
    const std::wstring p = pathFor(name, "IO::ReadText");
    std::string out;
    if (!p.empty() && !readFile(p, &out, kMaxFile)) out.clear();
    return out;
}

bool exists(const std::string& name) {
    const std::wstring p = pathFor(name, "IO::Exists");
    return !p.empty() && fileExists(p);
}

bool writeImpl(const std::string& name, const std::string& text, bool append) {
    const std::wstring p = pathFor(name, append ? "IO::AppendText" : "IO::WriteText");
    if (p.empty()) return false;
    Script* s = currentScript();
    if (text.size() > kMaxFile) {
        scriptError("IO: a file may hold at most 1 MB");
        return false;
    }
    uint64_t total = 0;
    const std::vector<std::string> files = listFiles(s->dataFolder, &total);
    const bool isNew = !fileExists(p);
    if (isNew && files.size() >= kMaxFiles) {
        scriptError("IO: at most 256 files per script");
        return false;
    }
    uint64_t oldSize = 0;
    if (!isNew) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) oldSize = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    }
    if (append && oldSize + text.size() > kMaxFile) {
        scriptError("IO: a file may hold at most 1 MB");
        return false;
    }
    const uint64_t after = total - (append ? 0 : oldSize) + text.size();
    if (after > kMaxFolder) {
        scriptError("IO: the script's data folder may hold at most 16 MB");
        return false;
    }
    const size_t cut = p.find_last_of(L'\\');
    if (cut != std::wstring::npos && !makeDirs(p.substr(0, cut))) return false;
    return writeFile(p, text, append);
}

bool writeText(const std::string& name, const std::string& text) { return writeImpl(name, text, false); }
bool appendText(const std::string& name, const std::string& text) { return writeImpl(name, text, true); }

bool removeFile(const std::string& name) {
    const std::wstring p = pathFor(name, "IO::Delete");
    return !p.empty() && DeleteFileW(p.c_str()) != 0;
}

CScriptArray* list() {
    Script* s = currentScript();
    asIScriptContext* ctx = asGetActiveContext();
    asITypeInfo* t = ctx ? ctx->GetEngine()->GetTypeInfoByDecl("array<string>") : nullptr;
    if (!t) return nullptr;
    std::vector<std::string> files;
    if (s && dirExists(s->dataFolder)) files = listFiles(s->dataFolder, nullptr);
    CScriptArray* a = CScriptArray::Create(t, static_cast<asUINT>(files.size()));
    for (asUINT i = 0; a && i < files.size(); i++) *static_cast<std::string*>(a->At(i)) = files[i];
    return a;
}

} // namespace

#define R(x)                                                                                                                                         \
    do {                                                                                                                                             \
        if ((x) < 0) return false;                                                                                                                   \
    } while (0)

bool registerIoApi(asIScriptEngine* e) {
    R(e->SetDefaultNamespace("IO"));
    R(e->RegisterGlobalFunction("string ReadText(const string &in name)", asFUNCTION(readText), asCALL_CDECL));
    R(e->RegisterGlobalFunction("bool WriteText(const string &in name, const string &in text)", asFUNCTION(writeText), asCALL_CDECL));
    R(e->RegisterGlobalFunction("bool AppendText(const string &in name, const string &in text)", asFUNCTION(appendText), asCALL_CDECL));
    R(e->RegisterGlobalFunction("bool Exists(const string &in name)", asFUNCTION(exists), asCALL_CDECL));
    R(e->RegisterGlobalFunction("bool Delete(const string &in name)", asFUNCTION(removeFile), asCALL_CDECL));
    R(e->RegisterGlobalFunction("array<string>@ List()", asFUNCTION(list), asCALL_CDECL));
    R(e->SetDefaultNamespace(""));
    return true;
}

} // namespace hs
