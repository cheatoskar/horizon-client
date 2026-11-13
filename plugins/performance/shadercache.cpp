// See shadercache.h. The engine side (static analysis 2006 engine, 2026-11-13; docs/performance-smooth.md):
//   - the shader's Compile method (ESWC 0x00581220, Sunrise 0x00703620, Original 0x007028a0; reached through the
//     shader vtables) builds the defines (the shader's own plus DGbxVersion), an include handler, and tries the
//     profiles from the highest the device supports down, calling D3DXCompileShader once per profile until one
//     compiles. On success it copies the bytecode into the shader, keeps the constant table and the error buffer is
//     released unread; on failure of every profile it logs "Compilation of EntryPoint ... failed".
//   - D3DXCompileShader is the D3DX library code linked into the exe (byte-identical in all three exes, hot-patch
//     prologue `mov edi,edi; push ebp; mov ebp,esp`). Its constant table is D3DXGetShaderConstantTable(bytecode),
//     called by the compiler on its own output, so a table made from cached bytecode is the same object state.
//   - The include handler: system includes (<Common.VHlsl.txt>, <Common.PHlsl.txt>) are copies of text the engine
//     holds in memory (no side effect); local includes ("...") load a file AND add it to the shader's dependency list.
//     So only compiles that used no local include are cached, and every hit opens its system includes again (through
//     the same handler) and compares their hash.
#include "shadercache.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace perf {
namespace shadercache {

bool Key::operator==(const Key& o) const { return memcmp(b, o.b, sizeof(b)) == 0; }

// ------------------------------------------------------------------ hashing
namespace {

BCRYPT_ALG_HANDLE g_alg = nullptr;
INIT_ONCE g_algOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK openAlg(PINIT_ONCE, PVOID, PVOID*) {
    if (BCryptOpenAlgorithmProvider(&g_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) g_alg = nullptr;
    return TRUE;
}

// Incremental SHA-256 (BCrypt). ok() is false if any step failed.
class Hasher {
  public:
    Hasher() {
        InitOnceExecuteOnce(&g_algOnce, openAlg, nullptr, nullptr);
        ok_ = g_alg && BCryptCreateHash(g_alg, &h_, nullptr, 0, nullptr, 0, 0) == 0;
    }
    ~Hasher() {
        if (h_) BCryptDestroyHash(h_);
    }
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;
    void add(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        while (ok_ && n) {
            const ULONG chunk = n > 0x40000000u ? 0x40000000u : static_cast<ULONG>(n);
            ok_ = BCryptHashData(h_, const_cast<PUCHAR>(b), chunk, 0) == 0;
            b += chunk;
            n -= chunk;
        }
    }
    void u32(uint32_t v) { add(&v, 4); }
    // length-prefixed text; NULL is a length no text can have
    void str(const char* s) {
        if (!s) {
            u32(0xffffffffu);
            return;
        }
        const size_t n = strlen(s);
        u32(static_cast<uint32_t>(n));
        add(s, n);
    }
    bool finish(uint8_t out[32]) {
        if (ok_) ok_ = BCryptFinishHash(h_, out, 32, 0) == 0;
        return ok_;
    }
    bool ok() const { return ok_; }

  private:
    BCRYPT_HASH_HANDLE h_ = nullptr;
    bool ok_ = false;
};

} // namespace

bool sha256(const void* data, size_t size, uint8_t out[32]) {
    Hasher h;
    h.add(data, size);
    return h.finish(out);
}

bool makeKey(const char* exeId, const char* src, uint32_t srcLen, const Macro* defines, const char* entry, const char* profile,
             uint32_t flags, Key* out) {
    if (!src || !out) return false;
    Hasher h;
    h.add("HZSC-KEY", 8);
    h.u32(kFormat);
    h.str(exeId ? exeId : "");
    h.u32(srcLen);
    h.add(src, srcLen);
    uint32_t n = 0;
    if (defines)
        while (defines[n].name) n++;
    h.u32(n);
    for (uint32_t i = 0; i < n; i++) {
        h.str(defines[i].name);
        h.str(defines[i].definition);
    }
    h.str(entry);
    h.str(profile);
    h.u32(flags);
    return h.finish(out->b);
}

std::string keyHex(const Key& k) {
    static const char* d = "0123456789abcdef";
    std::string s(64, '0');
    for (int i = 0; i < 32; i++) {
        s[i * 2] = d[k.b[i] >> 4];
        s[i * 2 + 1] = d[k.b[i] & 15];
    }
    return s;
}

// ------------------------------------------------------------------ files
// Little endian: "HZSC" u32 format, key[32], u32 kind, i32 hr, u32 includes, u32 payload size, payload sha[32],
// includes {u32 type, u32 name length, name, sha[32]}..., payload.
namespace {
const char kMagic[4] = {'H', 'Z', 'S', 'C'};
constexpr size_t kHeader = 4 + 4 + 32 + 4 + 4 + 4 + 4 + 32;
constexpr uint32_t kMaxPayload = 4u << 20;
constexpr uint32_t kMaxIncludes = 64;

void putU32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
uint32_t getU32(const std::string& s, size_t at) {
    uint32_t v;
    memcpy(&v, s.data() + at, 4);
    return v;
}
} // namespace

std::string serialize(const Key& key, const Entry& e) {
    std::string s;
    s.reserve(kHeader + e.payload.size() + 64);
    s.append(kMagic, 4);
    putU32(s, kFormat);
    s.append(reinterpret_cast<const char*>(key.b), 32);
    putU32(s, e.kind);
    putU32(s, static_cast<uint32_t>(e.hr));
    putU32(s, static_cast<uint32_t>(e.includes.size()));
    putU32(s, static_cast<uint32_t>(e.payload.size()));
    uint8_t sha[32] = {};
    sha256(e.payload.data(), e.payload.size(), sha);
    s.append(reinterpret_cast<const char*>(sha), 32);
    for (const Include& inc : e.includes) {
        putU32(s, inc.type);
        putU32(s, static_cast<uint32_t>(inc.name.size()));
        s += inc.name;
        s.append(reinterpret_cast<const char*>(inc.sha), 32);
    }
    s += e.payload;
    return s;
}

const char* readName(Read r) {
    switch (r) {
    case Read::Ok: return "ok";
    case Read::BadMagic: return "not a cache file";
    case Read::OtherFormat: return "other format version";
    case Read::WrongKey: return "key differs";
    case Read::Truncated: return "truncated";
    case Read::BadHash: return "content damaged";
    case Read::BadBytecode: return "not shader bytecode";
    }
    return "?";
}

Read parse(const std::string& f, const Key& key, Entry* out) {
    if (f.size() < 4 || memcmp(f.data(), kMagic, 4) != 0) return Read::BadMagic;
    if (f.size() < 8) return Read::Truncated;
    if (getU32(f, 4) != kFormat) return Read::OtherFormat;
    if (f.size() < kHeader) return Read::Truncated;
    if (memcmp(f.data() + 8, key.b, 32) != 0) return Read::WrongKey;
    Entry e;
    e.kind = getU32(f, 40);
    e.hr = static_cast<int32_t>(getU32(f, 44));
    const uint32_t nInc = getU32(f, 48);
    const uint32_t size = getU32(f, 52);
    if (e.kind != KindBytecode && e.kind != KindFailed) return Read::BadMagic;
    if (nInc > kMaxIncludes || size > kMaxPayload) return Read::BadHash;
    const uint8_t* sha = reinterpret_cast<const uint8_t*>(f.data() + 56);
    size_t at = kHeader;
    for (uint32_t i = 0; i < nInc; i++) {
        if (f.size() < at + 8) return Read::Truncated;
        Include inc;
        inc.type = getU32(f, at);
        const uint32_t len = getU32(f, at + 4);
        at += 8;
        if (len > 1024 || f.size() < at + len + 32) return Read::Truncated;
        inc.name.assign(f.data() + at, len);
        at += len;
        memcpy(inc.sha, f.data() + at, 32);
        at += 32;
        e.includes.push_back(inc);
    }
    if (f.size() < at + size) return Read::Truncated;
    if (f.size() != at + size) return Read::BadHash; // trailing bytes: not what we wrote
    e.payload.assign(f.data() + at, size);
    uint8_t got[32];
    if (!sha256(e.payload.data(), e.payload.size(), got) || memcmp(got, sha, 32) != 0) return Read::BadHash;
    if (e.kind == KindBytecode && (e.hr < 0 || !bytecodeLooksValid(e.payload.data(), e.payload.size(), nullptr))) return Read::BadBytecode;
    if (e.kind == KindFailed && e.hr >= 0) return Read::BadBytecode;
    if (out) *out = std::move(e);
    return Read::Ok;
}

bool bytecodeLooksValid(const void* p, size_t n, const char* profile) {
    if (!p || n < 8 || (n & 3)) return false;
    uint32_t first, last;
    memcpy(&first, p, 4);
    memcpy(&last, static_cast<const char*>(p) + n - 4, 4);
    const uint32_t kind = first >> 16;
    if (kind != 0xfffe && kind != 0xffff) return false; // vs / ps version token
    if (last != 0x0000ffff) return false;               // end token
    if (profile && profile[0] == 'v' && kind != 0xfffe) return false;
    if (profile && profile[0] == 'p' && kind != 0xffff) return false;
    return true;
}

// ------------------------------------------------------------------ the three exes
// Found 2026-11-13 by static analysis (D3DX strings, the compiler's own call to D3DXGetShaderConstantTable, the
// engine's only call of D3DXCompileShader), the three D3DX functions compared instruction by instruction across the
// exes (identical except call distances). The byte strings are each function's start up to its first relative call.
namespace {
const char kCompileBytes[] = "8bff558bec81ec70030000535633f6578d8d90fcffff8975fc8975f8";
const char kCreateBufferBytes[] = "8bff558bec538b5d0c85db7507b86c087688eb49566a10";
const char kTableBytes[] = "8bff558bec538b5d0c5633f63bde57740289338b7d083bfe74043bde7507b86c";
const Spec kSpecs[] = {
    {"eswc", "TrackMania Nations ESWC 1.80", 0x5ce7272f, 0x0087aa6b, kCompileBytes, 0x008a110e, kCreateBufferBytes, 0x0087b669,
     kTableBytes},
    {"sunrise", "TrackMania Sunrise (Win10 repack)", 0x5cc8f0cf, 0x008e3292, kCompileBytes, 0x00915f23, kCreateBufferBytes,
     0x008e3e90, kTableBytes},
    {"original", "TrackMania Original (Online Patch v2)", 0x5d337477, 0x008dfd8f, kCompileBytes, 0x00912a24, kCreateBufferBytes,
     0x008e098d, kTableBytes},
};
} // namespace

const Spec* specFor(uint32_t timestamp) {
    for (const Spec& s : kSpecs)
        if (s.timestamp == timestamp) return &s;
    return nullptr;
}

// ------------------------------------------------------------------ hook
namespace {

using CompileFn = HRESULT(WINAPI*)(const char* src, UINT srcLen, const Macro* defines, void* include, const char* entry,
                                   const char* profile, DWORD flags, void** ppShader, void** ppErrors, void** ppTable);
using CreateBufferFn = HRESULT(WINAPI*)(DWORD size, void** ppBuffer);
using TableFn = HRESULT(WINAPI*)(const DWORD* bytecode, void** ppTable);
// ID3DXBuffer: IUnknown (0 QueryInterface, 1 AddRef, 2 Release), 3 GetBufferPointer, 4 GetBufferSize
using ReleaseFn = ULONG(WINAPI*)(void*);
using BufPtrFn = void*(WINAPI*)(void*);
using BufSizeFn = DWORD(WINAPI*)(void*);
// ID3DXInclude (no IUnknown): 0 Open(type, name, parent, ppData, pBytes), 1 Close(pData)
using IncOpenFn = HRESULT(WINAPI*)(void* self, UINT type, const char* name, const void* parent, const void** ppData, UINT* pBytes);
using IncCloseFn = HRESULT(WINAPI*)(void* self, const void* data);

inline void** vt(void* obj) { return *static_cast<void***>(obj); }
void releaseObj(void* o) {
    if (o) reinterpret_cast<ReleaseFn>(vt(o)[2])(o);
}
void* bufPtr(void* b) { return reinterpret_cast<BufPtrFn>(vt(b)[3])(b); }
DWORD bufSize(void* b) { return reinterpret_cast<BufSizeFn>(vt(b)[4])(b); }

Config g_cfg;
Spec g_spec = {};
CompileFn g_tramp = nullptr;
CreateBufferFn g_createBuffer = nullptr;
TableFn g_table = nullptr;
uint8_t* g_target = nullptr;
uint8_t g_orig[5] = {};
uint8_t g_jump[5] = {};
bool g_installed = false;
volatile LONG g_active = 0;
char g_refused[200] = "";
bool g_dirMade = false;
int g_logBudget = 8; // lines about single shaders (bad files, write errors) per start

SRWLOCK g_statsLock = SRWLOCK_INIT;
Stats g_stats;

thread_local int t_depth = 0;

void logf(int level, const char* fmt, ...) {
    if (!g_cfg.log) return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    g_cfg.log(level, "%s", b);
}
void shaderNote(const Key& k, const char* what) {
    if (g_logBudget <= 0) return;
    g_logBudget--;
    logf(1, "shader cache: %.12s %s%s", keyHex(k).c_str(), what, g_logBudget == 0 ? " (no more lines like this)" : "");
}

double nowMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

template <typename F> void stat(F f) {
    AcquireSRWLockExclusive(&g_statsLock);
    f(g_stats);
    ReleaseSRWLockExclusive(&g_statsLock);
}

// SEH-guarded memory compare (the address may be unmapped in a test or another exe)
bool bytesEqual(const void* at, const uint8_t* want, size_t n) {
    __try {
        return memcmp(at, want, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool fromHex(const char* hex, std::vector<uint8_t>& out) {
    out.clear();
    const size_t n = hex ? strlen(hex) : 0;
    if (!n || (n & 1)) return false;
    for (size_t i = 0; i < n; i += 2) {
        unsigned v = 0;
        if (sscanf_s(hex + i, "%2x", &v) != 1) return false;
        out.push_back(static_cast<uint8_t>(v));
    }
    return true;
}

bool readFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart <= static_cast<LONGLONG>(kMaxPayload) + 0x10000;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

void makeDirs(const std::wstring& folder) {
    for (size_t i = 3; i < folder.size(); i++)
        if (folder[i] == L'\\' || folder[i] == L'/') CreateDirectoryW(folder.substr(0, i).c_str(), nullptr);
}

bool writeEntry(const std::wstring& path, const std::string& data) {
    if (!g_dirMade) {
        makeDirs(g_cfg.folder);
        g_dirMade = true;
    }
    wchar_t suffix[32];
    swprintf_s(suffix, L".%lu.tmp", GetCurrentProcessId());
    const std::wstring tmp = path + suffix;
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// ---- include proxy: forwards to the engine's handler and notes what was opened
struct IncProxy {
    void** vtbl;
    void* inner;
    std::vector<Include>* opened;
    bool uncacheable; // a local include (side effect in the engine), no name, or a failed open
};
HRESULT WINAPI proxyOpen(void* self, UINT type, const char* name, const void* parent, const void** ppData, UINT* pBytes) {
    IncProxy* p = static_cast<IncProxy*>(self);
    const HRESULT hr = reinterpret_cast<IncOpenFn>(vt(p->inner)[0])(p->inner, type, name, parent, ppData, pBytes);
    if (type != 1 || !name || FAILED(hr) || !ppData || !pBytes || (!*ppData && *pBytes)) {
        p->uncacheable = true;
        return hr;
    }
    Include inc;
    inc.type = type;
    inc.name = name;
    if (!sha256(*ppData, *pBytes, inc.sha)) p->uncacheable = true;
    bool seen = false;
    for (const Include& o : *p->opened) seen = seen || (o.type == inc.type && o.name == inc.name && !memcmp(o.sha, inc.sha, 32));
    if (!seen) p->opened->push_back(inc);
    return hr;
}
HRESULT WINAPI proxyClose(void* self, const void* data) {
    IncProxy* p = static_cast<IncProxy*>(self);
    return reinterpret_cast<IncCloseFn>(vt(p->inner)[1])(p->inner, data);
}
void* g_proxyVtbl[2] = {reinterpret_cast<void*>(&proxyOpen), reinterpret_cast<void*>(&proxyClose)};

// Opens each system include of a hit again through the engine's handler: the text must be what it was.
bool includesUnchanged(void* include, const std::vector<Include>& incs) {
    if (incs.empty()) return true;
    if (!include) return false;
    for (const Include& inc : incs) {
        const void* data = nullptr;
        UINT bytes = 0;
        const HRESULT hr = reinterpret_cast<IncOpenFn>(vt(include)[0])(include, inc.type, inc.name.c_str(), nullptr, &data, &bytes);
        if (FAILED(hr)) return false;
        uint8_t sha[32];
        const bool same = sha256(data, bytes, sha) && !memcmp(sha, inc.sha, 32);
        reinterpret_cast<IncCloseFn>(vt(include)[1])(include, data);
        if (!same) return false;
    }
    return true;
}

// Answers a call from a cache entry exactly like the compiler would. false: nothing handed out, compile normally.
bool answer(const Entry& e, void** ppShader, void** ppErrors, void** ppTable) {
    void* buf = nullptr;
    if (!e.payload.empty()) {
        if (FAILED(g_createBuffer(static_cast<DWORD>(e.payload.size()), &buf)) || !buf) return false;
        if (bufSize(buf) < e.payload.size() || !bufPtr(buf)) {
            releaseObj(buf);
            return false;
        }
        memcpy(bufPtr(buf), e.payload.data(), e.payload.size());
    }
    if (e.kind == KindBytecode) {
        void* table = nullptr;
        if (!buf) return false;
        if (ppTable && (FAILED(g_table(static_cast<const DWORD*>(bufPtr(buf)), &table)) || !table)) {
            releaseObj(buf);
            return false;
        }
        if (ppShader) *ppShader = buf;
        else releaseObj(buf);
        if (ppTable) *ppTable = table;
        if (ppErrors) *ppErrors = nullptr;
    } else {
        if (ppShader) *ppShader = nullptr;
        if (ppTable) *ppTable = nullptr;
        if (ppErrors) *ppErrors = buf;
        else releaseObj(buf);
    }
    return true;
}

HRESULT WINAPI hookCompile(const char* src, UINT srcLen, const Macro* defines, void* include, const char* entry, const char* profile,
                           DWORD flags, void** ppShader, void** ppErrors, void** ppTable) {
    if (!g_active || t_depth > 0 || !src) return g_tramp(src, srcLen, defines, include, entry, profile, flags, ppShader, ppErrors, ppTable);
    struct Depth {
        Depth() { t_depth++; }
        ~Depth() { t_depth--; }
    } depth;
    const double t0 = nowMs();
    Key key;
    if (!makeKey(g_cfg.exeId.c_str(), src, srcLen, defines, entry, profile, flags, &key)) {
        stat([](Stats& s) { s.notCached++; });
        return g_tramp(src, srcLen, defines, include, entry, profile, flags, ppShader, ppErrors, ppTable);
    }
    const std::string hex = keyHex(key);
    const std::wstring path = g_cfg.folder + std::wstring(hex.begin(), hex.end()) + L".bin";

    std::string file;
    if (readFile(path, file)) {
        Entry e;
        const Read r = parse(file, key, &e);
        const char* why = nullptr;
        if (r != Read::Ok) why = readName(r);
        else if (e.kind == KindBytecode && !bytecodeLooksValid(e.payload.data(), e.payload.size(), profile)) why = "wrong shader type";
        else if (!includesUnchanged(include, e.includes)) why = "an include changed";
        else if (!answer(e, ppShader, ppErrors, ppTable)) why = "D3DX could not take it";
        if (!why) {
            const double ms = nowMs() - t0;
            stat([ms](Stats& s) {
                s.hits++;
                s.hitMs += ms;
            });
            return static_cast<HRESULT>(e.hr);
        }
        stat([](Stats& s) { s.fallbacks++; });
        char note[96];
        snprintf(note, sizeof(note), "not used (%s), compiled again", why);
        shaderNote(key, note);
    }

    // miss: the engine's compiler, with the include handler wrapped to see what it opens
    std::vector<Include> opened;
    IncProxy proxy = {g_proxyVtbl, include, &opened, false};
    const double c0 = nowMs();
    const HRESULT hr = g_tramp(src, srcLen, defines, include ? &proxy : nullptr, entry, profile, flags, ppShader, ppErrors, ppTable);
    const double compileMs = nowMs() - c0;

    Entry e;
    bool keep = !proxy.uncacheable;
    if (keep && SUCCEEDED(hr) && ppShader && *ppShader) {
        const void* p = bufPtr(*ppShader);
        const DWORD n = bufSize(*ppShader);
        keep = n <= kMaxPayload && bytecodeLooksValid(p, n, profile);
        if (keep) {
            e.kind = KindBytecode;
            e.payload.assign(static_cast<const char*>(p), n);
        }
    } else if (keep && hr == E_FAIL && ppErrors && *ppErrors && bufSize(*ppErrors) <= kMaxPayload) {
        // a compile error (this profile does not fit the shader): the engine tries the next profile; keep the answer
        e.kind = KindFailed;
        e.payload.assign(static_cast<const char*>(bufPtr(*ppErrors)), bufSize(*ppErrors));
    } else {
        keep = false;
    }
    bool stored = false;
    if (keep) {
        e.hr = hr;
        e.includes = opened;
        stored = writeEntry(path, serialize(key, e));
        if (!stored) shaderNote(key, "could not be written");
    }
    stat([&](Stats& s) {
        s.misses++;
        s.compileMs += compileMs;
        if (stored) s.stored++;
        else s.notCached++;
    });
    return hr;
}

bool refuse(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_refused, sizeof(g_refused), fmt, ap);
    va_end(ap);
    logf(2, "shader cache: off, %s", g_refused);
    return false;
}

} // namespace

bool install(const Spec& spec, const Config& cfg) {
    if (g_installed) return true;
    g_cfg = cfg;
    g_refused[0] = 0;
    struct Fn {
        const char* what;
        uintptr_t link;
        const char* hex;
    } fns[] = {{"D3DXCompileShader", spec.compile, spec.compileBytes},
               {"D3DXCreateBuffer", spec.createBuffer, spec.createBufferBytes},
               {"D3DXGetShaderConstantTable", spec.constantTable, spec.constantTableBytes}};
    for (const Fn& f : fns) {
        std::vector<uint8_t> want;
        if (!f.link || !fromHex(f.hex, want)) return refuse("no address for %s", f.what);
        const void* at = reinterpret_cast<const void*>(f.link + cfg.delta);
        if (!bytesEqual(at, want.data(), want.size()))
            return refuse("%s at 0x%08lx has other bytes (another exe build, or another mod hooked it)", f.what,
                          static_cast<unsigned long>(f.link));
    }
    static const uint8_t kHotPatch[5] = {0x8b, 0xff, 0x55, 0x8b, 0xec};
    uint8_t* target = reinterpret_cast<uint8_t*>(spec.compile + cfg.delta);
    if (!bytesEqual(target, kHotPatch, 5)) return refuse("D3DXCompileShader does not start with the hot-patch prologue");

    uint8_t* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return refuse("no memory for the trampoline");
    memcpy(tramp, target, 5); // mov edi,edi; push ebp; mov ebp,esp: no relative operands
    tramp[5] = 0xe9;
    const int32_t back = static_cast<int32_t>(reinterpret_cast<intptr_t>(target + 5) - reinterpret_cast<intptr_t>(tramp + 10));
    memcpy(tramp + 6, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 16);

    // the hook stays reachable for the life of the process, even if the plugin is unloaded
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&hookCompile), &self);

    g_spec = spec;
    g_tramp = reinterpret_cast<CompileFn>(tramp);
    g_createBuffer = reinterpret_cast<CreateBufferFn>(spec.createBuffer + cfg.delta);
    g_table = reinterpret_cast<TableFn>(spec.constantTable + cfg.delta);
    g_target = target;
    memcpy(g_orig, target, 5);
    g_jump[0] = 0xe9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(&hookCompile) - reinterpret_cast<intptr_t>(target + 5));
    memcpy(g_jump + 1, &rel, 4);

    DWORD old = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) return refuse("VirtualProtect failed (%lu)", GetLastError());
    // the hot-patch prologue is 5 bytes of whole instructions; the game thread is the caller of install
    memcpy(target, g_jump, 5);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    g_installed = true;
    logf(1, "shader cache: on for %s, D3DXCompileShader 0x%08lx hooked (bytes checked), files in %ls", spec.name,
         static_cast<unsigned long>(spec.compile), cfg.folder.c_str());
    return true;
}

bool installed() { return g_installed; }
const char* refusedWhy() { return g_refused; }
void setActive(bool on) { InterlockedExchange(&g_active, on ? 1 : 0); }
bool active() { return g_active != 0; }

void uninstall() {
    setActive(false);
    if (!g_installed || !g_target) return;
    if (!bytesEqual(g_target, g_jump, 5)) {
        logf(2, "shader cache: another hook sits on D3DXCompileShader now; ours stays as a pass-through");
        return;
    }
    DWORD old = 0;
    if (VirtualProtect(g_target, 5, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(g_target, g_orig, 5);
        VirtualProtect(g_target, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), g_target, 5);
        g_installed = false; // the trampoline stays allocated: it is 16 bytes and someone may still hold it
    }
}

Stats peek() {
    Stats s;
    stat([&](Stats& g) { s = g; });
    return s;
}
Stats take() {
    Stats s;
    stat([&](Stats& g) {
        s = g;
        g = Stats();
    });
    return s;
}

std::string statsLine(const Stats& s) {
    char b[200];
    snprintf(b, sizeof(b), "%u hits, %u misses (%u stored", s.hits, s.misses, s.stored);
    std::string out = b;
    if (s.notCached) {
        snprintf(b, sizeof(b), ", %u not cacheable", s.notCached);
        out += b;
    }
    out += ")";
    if (s.fallbacks) {
        snprintf(b, sizeof(b), ", %u files not usable", s.fallbacks);
        out += b;
    }
    snprintf(b, sizeof(b), ", compile %.0f ms, cache %.0f ms", s.compileMs, s.hitMs);
    out += b;
    return out;
}

} // namespace shadercache
} // namespace perf
