// Shader disk cache for the 2006-engine games (Nations ESWC, Sunrise, Original). Never on TMNF/TMUF (Competition
// Patch: no hooks in the exe there).
//
// The 2006 engine compiles HLSL when a shader is bound the first time, with the D3DX compiler linked into the exe
// (D3DXCompileShader, one call site: the shader's Compile method). This module puts an inline hook on that
// D3DXCompileShader (5-byte hot-patch prologue, every byte of the start checked per exe) and keeps the compiled
// bytecode on disk, keyed by a SHA-256 of everything the result depends on: source text, defines, entry point,
// profile, flags and the exe build. A hit hands the engine the same bytes the compiler made, in a buffer made by
// the exe's own D3DXCreateBuffer, and the constant table from the exe's own D3DXGetShaderConstantTable (the call the
// compiler itself makes on its output). Anything unexpected (bad file, include changed, a D3DX call failed) falls
// back to the normal compile. Addresses and the checks: docs/performance-smooth.md, "Shader cache".
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace perf {
namespace shadercache {

constexpr uint32_t kFormat = 1; // file format and key version; a different value is a miss (old files are rewritten)

// ---------------------------------------------------------------- keys and files (pure, unit-tested)
struct Key {
    uint8_t b[32] = {};
    bool operator==(const Key& o) const;
};
struct Macro { // D3DXMACRO
    const char* name;
    const char* definition;
};

bool sha256(const void* data, size_t size, uint8_t out[32]);
// exeId: names the compiler build (game key + PE timestamp). defines: NULL-name terminated, may be NULL.
bool makeKey(const char* exeId, const char* src, uint32_t srcLen, const Macro* defines, const char* entry, const char* profile,
             uint32_t flags, Key* out);
std::string keyHex(const Key& k);

enum Kind : uint32_t { KindBytecode = 0, KindFailed = 1 };
struct Include {
    uint32_t type = 0; // D3DXINCLUDE_TYPE: 0 local ("..."), 1 system (<...>)
    std::string name;
    uint8_t sha[32] = {};
};
struct Entry {
    uint32_t kind = KindBytecode;
    int32_t hr = 0;                  // what D3DXCompileShader returned
    std::vector<Include> includes;   // system includes the compile opened (checked again on every hit)
    std::string payload;             // bytecode (KindBytecode) or the error buffer (KindFailed), exact size
};

std::string serialize(const Key& key, const Entry& e);
enum class Read { Ok, BadMagic, OtherFormat, WrongKey, Truncated, BadHash, BadBytecode };
const char* readName(Read r);
Read parse(const std::string& file, const Key& key, Entry* out);
// A complete D3D9 shader token stream: version token (vs or ps as `profile` says, if given), end token.
bool bytecodeLooksValid(const void* p, size_t n, const char* profile);

// ---------------------------------------------------------------- the hook
// One exe build. Addresses are link-time VAs (base 0x00400000); bytes are the start of each function as hex.
struct Spec {
    const char* game;          // "eswc", "sunrise", "original" (also the cache sub-folder)
    const char* name;          // for the log
    uint32_t timestamp;        // PE TimeDateStamp
    uintptr_t compile;         // D3DXCompileShader (stdcall, 10 args)
    const char* compileBytes;  // starts with 8b ff 55 8b ec (the 5 bytes the hook replaces)
    uintptr_t createBuffer;    // D3DXCreateBuffer (stdcall, 2 args)
    const char* createBufferBytes;
    uintptr_t constantTable;   // D3DXGetShaderConstantTable (stdcall, 2 args)
    const char* constantTableBytes;
};
// The three known exes; nullptr for any other timestamp.
const Spec* specFor(uint32_t timestamp);

using LogFn = void (*)(int level, const char* fmt, ...); // levels as HORIZON_LOG_*
struct Config {
    std::wstring folder;  // cache folder, trailing backslash (created when the first entry is written)
    std::string exeId;    // part of every key
    LogFn log = nullptr;
    intptr_t delta = 0;   // module base - 0x00400000 (0 for the games: no relocations)
};

// Checks every function's bytes, then hooks D3DXCompileShader. false (and one log line) when refused; nothing is
// written then. Once installed the hook stays for the life of the process (the DLL is pinned); setActive(false)
// makes it a plain pass-through.
bool install(const Spec& spec, const Config& cfg);
bool installed();
const char* refusedWhy(); // "" when installed or not tried
void setActive(bool on);
bool active();
// Puts the original 5 bytes back if the jump there is still ours (shutdown). Pass-through either way.
void uninstall();

struct Stats {
    uint32_t hits = 0, misses = 0, stored = 0;
    uint32_t notCached = 0;  // compiled normally and not stored (local #include, no output, unusual result)
    uint32_t fallbacks = 0;  // a file was there but could not be used (bad file, include changed, D3DX call failed)
    double compileMs = 0;    // time in the engine's compiler (misses)
    double hitMs = 0;        // time spent answering hits
};
Stats peek();       // the current map's numbers
Stats take();       // returns them and starts a new count
std::string statsLine(const Stats& s); // "34 hits, 2 misses (2 stored), compile 180 ms, cache 6 ms"

} // namespace shadercache
} // namespace perf
