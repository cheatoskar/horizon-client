// See d3d_hooks.h. How it works:
//   - Each hooked slot keeps `next` (what the slot held before us: d3d9 itself or another mod's hook) and
//     `orig` (the first value we saw). A detour counts, maybe filters, then calls `next`.
//   - If another mod later writes over a slot, recheck() puts ours in front again and calls what it found.
//     A hook that chains back into ours on the same thread (re-entry) goes straight to `orig`, so nothing
//     runs twice and nothing loops.
//   - Resource method tables (buffers, textures, surfaces, queries, state blocks) are shared by every object
//     of that class inside d3d9.dll. They are found through objects we create once at attach and through the
//     game's own Create* calls.
//   - The state cache only filters while nothing can change states behind our back: no state block is being
//     recorded, the device was not Reset, no state block Apply since the last call, and no other hook sits
//     in a state slot (another mod might call d3d9 directly and bypass us).
#include "d3d_hooks.h"

#include <windows.h>
#include <d3d9.h>
#include <psapi.h>

#include <cstring>

namespace perf {
namespace d3d {
namespace {

LogFn g_log = nullptr;
#define DLOG(level, ...) \
    do {                 \
        if (g_log) g_log(level, __VA_ARGS__); \
    } while (0)

double g_qpcToMs = 0;
inline double nowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * g_qpcToMs;
}

bool writeSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *slot = value;
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}
bool readSlot(void** slot, void*& out) {
    __try {
        out = *slot;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ------------------------------------------------------------------ hooked tables
enum Kind { kDevice, kVB, kIB, kTex, kSurf, kQuery, kSB, kKinds };
struct Hook {
    Kind kind;
    int index;          // slot in the method table
    void* detour;
    const char* name;
};
struct TableHook {
    void** table = nullptr;
    void* next = nullptr;
    void* orig = nullptr;
    bool active = false;
};
constexpr int kMaxTables = 6;

// filled below the detours
extern const Hook kHooks[];
extern const int kHookCount;
TableHook g_tables[64][kMaxTables]; // [hook][table]
void** g_known[kKinds][kMaxTables] = {};

bool g_attached = false;
bool g_count = false, g_filterWanted = false, g_overlay = false, g_recording = false;
bool g_softwareVP = false;
const char* g_filterWhy = "";
HMODULE g_d3d9 = nullptr;
DeviceFacts g_facts;
CallCounts g_c;
StateCache g_cache;
thread_local int t_depth[64];

TableHook* findHook(int hook, void* self) {
    void** table = nullptr;
    if (!self) return nullptr;
    table = *reinterpret_cast<void***>(self);
    TableHook* t = g_tables[hook];
    for (int i = 0; i < kMaxTables; i++)
        if (t[i].table == table && t[i].next) return &t[i];
    return nullptr;
}

bool insideD3d9(void* p) {
    MODULEINFO mi = {};
    if (!g_d3d9 || !GetModuleInformation(GetCurrentProcess(), g_d3d9, &mi, sizeof(mi))) return false;
    const auto a = reinterpret_cast<uintptr_t>(p), b = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    return a >= b && a < b + mi.SizeOfImage;
}

void hookTable(Kind kind, void** table) {
    if (!table) return;
    int free = -1;
    for (int i = 0; i < kMaxTables; i++) {
        if (g_known[kind][i] == table) return;
        if (!g_known[kind][i] && free < 0) free = i;
    }
    if (free < 0) return;
    g_known[kind][free] = table;
    int n = 0;
    for (int h = 0; h < kHookCount; h++) {
        if (kHooks[h].kind != kind) continue;
        void** slot = &table[kHooks[h].index];
        void* cur = nullptr;
        if (!readSlot(slot, cur) || !cur) continue;
        TableHook& t = g_tables[h][free];
        t.table = table;
        t.orig = t.next = cur;
        if (!insideD3d9(cur)) g_facts.foreignHooks = true;
        if (writeSlot(slot, kHooks[h].detour)) {
            t.active = true;
            n++;
        }
    }
    static const char* kNames[] = {"device", "vertex buffer", "index buffer", "texture", "surface", "query", "state block"};
    DLOG(1, "d3d: %d slots hooked in the %s method table %p", n, kNames[kind], static_cast<void*>(table));
}

template <class T>
void hookObject(Kind kind, T* obj) {
    if (obj) hookTable(kind, *reinterpret_cast<void***>(obj));
}

// call `next`, or `orig` on re-entry (another hook that chains back into ours)
#define NEXT(hookIdx, Fn, self) \
    (t_depth[hookIdx] > 1 ? reinterpret_cast<Fn>(th->orig) : reinterpret_cast<Fn>(th->next))

struct Depth {
    int i;
    explicit Depth(int idx) : i(idx) { t_depth[i]++; }
    ~Depth() { t_depth[i]--; }
};

inline bool counting() { return g_count && !g_overlay; }
inline bool filtering() { return g_filterWanted && !g_filterWhy[0] && !g_recording; }

// ------------------------------------------------------------------ hook indices (order of kHooks)
enum {
    H_SetRenderState, H_SetSamplerState, H_SetTextureStageState, H_SetTexture, H_SetVertexShader, H_SetPixelShader,
    H_SetVSConstF, H_SetPSConstF, H_SetStreamSource, H_SetIndices, H_SetVertexDeclaration, H_SetFVF, H_SetTransform,
    H_SetRenderTarget, H_Clear, H_BeginScene, H_DrawPrimitive, H_DrawIndexedPrimitive, H_DrawPrimitiveUP,
    H_DrawIndexedPrimitiveUP, H_SetSoftwareVP, H_GetRenderTargetData, H_GetFrontBufferData, H_BeginStateBlock,
    H_EndStateBlock, H_CreateStateBlock, H_CreateQuery, H_CreateVertexBuffer, H_CreateIndexBuffer, H_CreateTexture,
    H_VBLock, H_IBLock, H_TexLockRect, H_SurfLockRect, H_QueryGetData, H_SBApply, H_COUNT
};

// ------------------------------------------------------------------ device detours
HRESULT STDMETHODCALLTYPE dSetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD v) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
    TableHook* th = findHook(H_SetRenderState, dev);
    Depth d(H_SetRenderState);
    if (!th) return D3DERR_INVALIDCALL;
    if (t_depth[H_SetRenderState] == 1) {
        const bool same = !g_recording && g_cache.renderStateSame(s, v);
        if (counting()) {
            g_c.renderStates++;
            if (same) g_c.renderStatesSame++;
        }
        if (same && filtering()) {
            if (counting()) g_c.filtered++;
            return D3D_OK;
        }
    }
    const HRESULT hr = NEXT(H_SetRenderState, Fn, dev)(dev, s, v);
    if (t_depth[H_SetRenderState] == 1 && !g_recording) {
        if (SUCCEEDED(hr)) g_cache.setRenderState(s, v);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE dSetSamplerState(IDirect3DDevice9* dev, DWORD smp, D3DSAMPLERSTATETYPE t, DWORD v) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
    TableHook* th = findHook(H_SetSamplerState, dev);
    Depth d(H_SetSamplerState);
    if (!th) return D3DERR_INVALIDCALL;
    if (t_depth[H_SetSamplerState] == 1) {
        const bool same = !g_recording && g_cache.samplerStateSame(smp, t, v);
        if (counting()) {
            g_c.samplerStates++;
            if (same) g_c.samplerStatesSame++;
        }
        if (same && filtering()) {
            if (counting()) g_c.filtered++;
            return D3D_OK;
        }
    }
    const HRESULT hr = NEXT(H_SetSamplerState, Fn, dev)(dev, smp, t, v);
    if (t_depth[H_SetSamplerState] == 1 && !g_recording && SUCCEEDED(hr)) g_cache.setSamplerState(smp, t, v);
    return hr;
}

HRESULT STDMETHODCALLTYPE dSetTextureStageState(IDirect3DDevice9* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
    TableHook* th = findHook(H_SetTextureStageState, dev);
    Depth d(H_SetTextureStageState);
    if (!th) return D3DERR_INVALIDCALL;
    if (t_depth[H_SetTextureStageState] == 1) {
        const bool same = !g_recording && g_cache.stageStateSame(stage, t, v);
        if (counting()) {
            g_c.textureStageStates++;
            if (same) g_c.textureStageStatesSame++;
        }
        if (same && filtering()) {
            if (counting()) g_c.filtered++;
            return D3D_OK;
        }
    }
    const HRESULT hr = NEXT(H_SetTextureStageState, Fn, dev)(dev, stage, t, v);
    if (t_depth[H_SetTextureStageState] == 1 && !g_recording && SUCCEEDED(hr)) g_cache.setStageState(stage, t, v);
    return hr;
}

HRESULT STDMETHODCALLTYPE dSetTexture(IDirect3DDevice9* dev, DWORD smp, IDirect3DBaseTexture9* tex) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
    TableHook* th = findHook(H_SetTexture, dev);
    Depth d(H_SetTexture);
    if (!th) return D3DERR_INVALIDCALL;
    if (t_depth[H_SetTexture] == 1) {
        const bool same = !g_recording && g_cache.textureSame(smp, tex);
        if (counting()) {
            g_c.textures++;
            if (same) g_c.texturesSame++;
        }
        if (same && filtering()) {
            if (counting()) g_c.filtered++;
            return D3D_OK;
        }
    }
    const HRESULT hr = NEXT(H_SetTexture, Fn, dev)(dev, smp, tex);
    if (t_depth[H_SetTexture] == 1 && !g_recording && SUCCEEDED(hr)) g_cache.setTexture(smp, tex);
    return hr;
}

HRESULT STDMETHODCALLTYPE dSetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
    TableHook* th = findHook(H_SetVertexShader, dev);
    Depth d(H_SetVertexShader);
    if (!th) return D3DERR_INVALIDCALL;
    if (t_depth[H_SetVertexShader] == 1) {
        const bool same = !g_recording && g_cache.vertexShaderSame(vs);
        if (counting()) {
            g_c.vertexShaders++;
            if (same) g_c.vertexShadersSame++;
        }
        if (same && filtering()) {
            if (counting()) g_c.filtered++;
            return D3D_OK;
        }
    }
    const HRESULT hr = NEXT(H_SetVertexShader, Fn, dev)(dev, vs);
    if (t_depth[H_SetVertexShader] == 1 && !g_recording && SUCCEEDED(hr)) g_cache.setVertexShader(vs);
    return hr;
}

HRESULT STDMETHODCALLTYPE dSetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
    TableHook* th = findHook(H_SetPixelShader, dev);
    Depth d(H_SetPixelShader);
    if (!th) return D3DERR_INVALIDCALL;
    if (t_depth[H_SetPixelShader] == 1) {
        const bool same = !g_recording && g_cache.pixelShaderSame(ps);
        if (counting()) {
            g_c.pixelShaders++;
            if (same) g_c.pixelShadersSame++;
        }
        if (same && filtering()) {
            if (counting()) g_c.filtered++;
            return D3D_OK;
        }
    }
    const HRESULT hr = NEXT(H_SetPixelShader, Fn, dev)(dev, ps);
    if (t_depth[H_SetPixelShader] == 1 && !g_recording && SUCCEEDED(hr)) g_cache.setPixelShader(ps);
    return hr;
}

// count-only detours: same shape, one counter
#define COUNT_ONLY(name, H, counterExpr, PARAMS, ARGS, TYPES)                          \
    HRESULT STDMETHODCALLTYPE name PARAMS {                                             \
        using Fn = HRESULT(STDMETHODCALLTYPE*) TYPES;                                    \
        TableHook* th = findHook(H, dev);                                                \
        Depth d(H);                                                                      \
        if (!th) return D3DERR_INVALIDCALL;                                              \
        if (t_depth[H] == 1 && counting()) { counterExpr; }                              \
        return NEXT(H, Fn, dev) ARGS;                                                    \
    }

COUNT_ONLY(dSetVSConstF, H_SetVSConstF, (g_c.vsConstCalls++, g_c.vsConstVec4 += n),
           (IDirect3DDevice9 * dev, UINT r, const float* f, UINT n), (dev, r, f, n), (IDirect3DDevice9*, UINT, const float*, UINT))
COUNT_ONLY(dSetPSConstF, H_SetPSConstF, (g_c.psConstCalls++, g_c.psConstVec4 += n),
           (IDirect3DDevice9 * dev, UINT r, const float* f, UINT n), (dev, r, f, n), (IDirect3DDevice9*, UINT, const float*, UINT))
COUNT_ONLY(dSetStreamSource, H_SetStreamSource, g_c.streams++,
           (IDirect3DDevice9 * dev, UINT s, IDirect3DVertexBuffer9* vb, UINT off, UINT stride), (dev, s, vb, off, stride),
           (IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT))
COUNT_ONLY(dSetIndices, H_SetIndices, g_c.indices++, (IDirect3DDevice9 * dev, IDirect3DIndexBuffer9* ib), (dev, ib),
           (IDirect3DDevice9*, IDirect3DIndexBuffer9*))
COUNT_ONLY(dSetVertexDeclaration, H_SetVertexDeclaration, g_c.declsAndFvf++,
           (IDirect3DDevice9 * dev, IDirect3DVertexDeclaration9* v), (dev, v), (IDirect3DDevice9*, IDirect3DVertexDeclaration9*))
COUNT_ONLY(dSetFVF, H_SetFVF, g_c.declsAndFvf++, (IDirect3DDevice9 * dev, DWORD fvf), (dev, fvf), (IDirect3DDevice9*, DWORD))
COUNT_ONLY(dSetTransform, H_SetTransform, g_c.transforms++,
           (IDirect3DDevice9 * dev, D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m), (dev, s, m),
           (IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*))
COUNT_ONLY(dSetRenderTarget, H_SetRenderTarget, g_c.renderTargets++,
           (IDirect3DDevice9 * dev, DWORD i, IDirect3DSurface9* s), (dev, i, s), (IDirect3DDevice9*, DWORD, IDirect3DSurface9*))
COUNT_ONLY(dClear, H_Clear, g_c.clears++,
           (IDirect3DDevice9 * dev, DWORD n, const D3DRECT* r, DWORD f, D3DCOLOR c, float z, DWORD s), (dev, n, r, f, c, z, s),
           (IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD))
COUNT_ONLY(dBeginScene, H_BeginScene, g_c.scenes++, (IDirect3DDevice9 * dev), (dev), (IDirect3DDevice9*))
COUNT_ONLY(dDrawPrimitive, H_DrawPrimitive, (g_c.draws++, g_c.primitives += n, g_c.drawsSoftwareVP += g_softwareVP ? 1 : 0),
           (IDirect3DDevice9 * dev, D3DPRIMITIVETYPE t, UINT start, UINT n), (dev, t, start, n),
           (IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT))
COUNT_ONLY(dDrawIndexedPrimitive, H_DrawIndexedPrimitive,
           (g_c.draws++, g_c.primitives += n, g_c.drawsSoftwareVP += g_softwareVP ? 1 : 0),
           (IDirect3DDevice9 * dev, D3DPRIMITIVETYPE t, INT base, UINT minI, UINT nv, UINT si, UINT n), (dev, t, base, minI, nv, si, n),
           (IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT))
COUNT_ONLY(dDrawPrimitiveUP, H_DrawPrimitiveUP, (g_c.drawsUP++, g_c.primitives += n, g_c.drawsSoftwareVP += g_softwareVP ? 1 : 0),
           (IDirect3DDevice9 * dev, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT stride), (dev, t, n, v, stride),
           (IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT))
COUNT_ONLY(dDrawIndexedPrimitiveUP, H_DrawIndexedPrimitiveUP,
           (g_c.drawsUP++, g_c.primitives += n, g_c.drawsSoftwareVP += g_softwareVP ? 1 : 0),
           (IDirect3DDevice9 * dev, D3DPRIMITIVETYPE t, UINT minI, UINT nv, UINT n, const void* idx, D3DFORMAT f, const void* v, UINT stride),
           (dev, t, minI, nv, n, idx, f, v, stride),
           (IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT))

HRESULT STDMETHODCALLTYPE dSetSoftwareVP(IDirect3DDevice9* dev, BOOL sw) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, BOOL);
    TableHook* th = findHook(H_SetSoftwareVP, dev);
    Depth d(H_SetSoftwareVP);
    if (!th) return D3DERR_INVALIDCALL;
    const HRESULT hr = NEXT(H_SetSoftwareVP, Fn, dev)(dev, sw);
    if (SUCCEEDED(hr) && !g_recording) g_softwareVP = sw != FALSE;
    return hr;
}

HRESULT STDMETHODCALLTYPE dGetRenderTargetData(IDirect3DDevice9* dev, IDirect3DSurface9* a, IDirect3DSurface9* b) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
    TableHook* th = findHook(H_GetRenderTargetData, dev);
    Depth d(H_GetRenderTargetData);
    if (!th) return D3DERR_INVALIDCALL;
    if (t_depth[H_GetRenderTargetData] > 1 || !counting()) return NEXT(H_GetRenderTargetData, Fn, dev)(dev, a, b);
    const double t0 = nowMs();
    const HRESULT hr = NEXT(H_GetRenderTargetData, Fn, dev)(dev, a, b);
    g_c.readbacks++;
    g_c.readbackMs += nowMs() - t0;
    return hr;
}

HRESULT STDMETHODCALLTYPE dGetFrontBufferData(IDirect3DDevice9* dev, UINT sc, IDirect3DSurface9* s) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, IDirect3DSurface9*);
    TableHook* th = findHook(H_GetFrontBufferData, dev);
    Depth d(H_GetFrontBufferData);
    if (!th) return D3DERR_INVALIDCALL;
    if (t_depth[H_GetFrontBufferData] > 1 || !counting()) return NEXT(H_GetFrontBufferData, Fn, dev)(dev, sc, s);
    const double t0 = nowMs();
    const HRESULT hr = NEXT(H_GetFrontBufferData, Fn, dev)(dev, sc, s);
    g_c.readbacks++;
    g_c.readbackMs += nowMs() - t0;
    return hr;
}

HRESULT STDMETHODCALLTYPE dBeginStateBlock(IDirect3DDevice9* dev) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
    TableHook* th = findHook(H_BeginStateBlock, dev);
    Depth d(H_BeginStateBlock);
    if (!th) return D3DERR_INVALIDCALL;
    const HRESULT hr = NEXT(H_BeginStateBlock, Fn, dev)(dev);
    if (SUCCEEDED(hr)) g_recording = true;
    return hr;
}

HRESULT STDMETHODCALLTYPE dEndStateBlock(IDirect3DDevice9* dev, IDirect3DStateBlock9** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DStateBlock9**);
    TableHook* th = findHook(H_EndStateBlock, dev);
    Depth d(H_EndStateBlock);
    if (!th) return D3DERR_INVALIDCALL;
    const HRESULT hr = NEXT(H_EndStateBlock, Fn, dev)(dev, out);
    g_recording = false;
    if (SUCCEEDED(hr) && out && *out) hookObject(kSB, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE dCreateStateBlock(IDirect3DDevice9* dev, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**);
    TableHook* th = findHook(H_CreateStateBlock, dev);
    Depth d(H_CreateStateBlock);
    if (!th) return D3DERR_INVALIDCALL;
    const HRESULT hr = NEXT(H_CreateStateBlock, Fn, dev)(dev, t, out);
    if (SUCCEEDED(hr) && out && *out) hookObject(kSB, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE dCreateQuery(IDirect3DDevice9* dev, D3DQUERYTYPE t, IDirect3DQuery9** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
    TableHook* th = findHook(H_CreateQuery, dev);
    Depth d(H_CreateQuery);
    if (!th) return D3DERR_INVALIDCALL;
    const HRESULT hr = NEXT(H_CreateQuery, Fn, dev)(dev, t, out);
    if (SUCCEEDED(hr) && out && *out) hookObject(kQuery, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE dCreateVertexBuffer(IDirect3DDevice9* dev, UINT len, DWORD use, DWORD fvf, D3DPOOL pool,
                                              IDirect3DVertexBuffer9** out, HANDLE* sh) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
    TableHook* th = findHook(H_CreateVertexBuffer, dev);
    Depth d(H_CreateVertexBuffer);
    if (!th) return D3DERR_INVALIDCALL;
    const HRESULT hr = NEXT(H_CreateVertexBuffer, Fn, dev)(dev, len, use, fvf, pool, out, sh);
    if (SUCCEEDED(hr) && out && *out) hookObject(kVB, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE dCreateIndexBuffer(IDirect3DDevice9* dev, UINT len, DWORD use, D3DFORMAT f, D3DPOOL pool,
                                             IDirect3DIndexBuffer9** out, HANDLE* sh) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
    TableHook* th = findHook(H_CreateIndexBuffer, dev);
    Depth d(H_CreateIndexBuffer);
    if (!th) return D3DERR_INVALIDCALL;
    const HRESULT hr = NEXT(H_CreateIndexBuffer, Fn, dev)(dev, len, use, f, pool, out, sh);
    if (SUCCEEDED(hr) && out && *out) hookObject(kIB, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE dCreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT lv, DWORD use, D3DFORMAT f, D3DPOOL pool,
                                         IDirect3DTexture9** out, HANDLE* sh) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
    TableHook* th = findHook(H_CreateTexture, dev);
    Depth d(H_CreateTexture);
    if (!th) return D3DERR_INVALIDCALL;
    const HRESULT hr = NEXT(H_CreateTexture, Fn, dev)(dev, w, h, lv, use, f, pool, out, sh);
    if (SUCCEEDED(hr) && out && *out) hookObject(kTex, *out);
    return hr;
}

// ------------------------------------------------------------------ resource detours
inline void countLock(DWORD flags, double ms) {
    g_c.locks++;
    if (!(flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) g_c.locksPlain++;
    g_c.lockMs += ms;
}

#define LOCK_DETOUR(name, H, T, PARAMS, ARGS, TYPES, FLAGS)                                   \
    HRESULT STDMETHODCALLTYPE name PARAMS {                                                    \
        using Fn = HRESULT(STDMETHODCALLTYPE*) TYPES;                                           \
        TableHook* th = findHook(H, self);                                                      \
        Depth d(H);                                                                             \
        if (!th) return D3DERR_INVALIDCALL;                                                     \
        if (t_depth[H] > 1 || !counting()) return NEXT(H, Fn, self) ARGS;                       \
        const double t0 = nowMs();                                                              \
        const HRESULT hr = NEXT(H, Fn, self) ARGS;                                              \
        countLock(FLAGS, nowMs() - t0);                                                         \
        return hr;                                                                              \
    }

LOCK_DETOUR(dVBLock, H_VBLock, IDirect3DVertexBuffer9, (IDirect3DVertexBuffer9 * self, UINT o, UINT s, void** p, DWORD fl),
            (self, o, s, p, fl), (IDirect3DVertexBuffer9*, UINT, UINT, void**, DWORD), fl)
LOCK_DETOUR(dIBLock, H_IBLock, IDirect3DIndexBuffer9, (IDirect3DIndexBuffer9 * self, UINT o, UINT s, void** p, DWORD fl),
            (self, o, s, p, fl), (IDirect3DIndexBuffer9*, UINT, UINT, void**, DWORD), fl)
LOCK_DETOUR(dTexLockRect, H_TexLockRect, IDirect3DTexture9,
            (IDirect3DTexture9 * self, UINT lv, D3DLOCKED_RECT* r, const RECT* rc, DWORD fl), (self, lv, r, rc, fl),
            (IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD), fl)
LOCK_DETOUR(dSurfLockRect, H_SurfLockRect, IDirect3DSurface9, (IDirect3DSurface9 * self, D3DLOCKED_RECT* r, const RECT* rc, DWORD fl),
            (self, r, rc, fl), (IDirect3DSurface9*, D3DLOCKED_RECT*, const RECT*, DWORD), fl)

// The engine's GPU sync (TMNF CVisionViewportDx9::SyncGpuWaitQuery) polls an event query with
// D3DGETDATA_FLUSH and Sleep(0) until the GPU is done: the time from the first "not yet" to "done" is the wait.
IDirect3DQuery9* g_busyQuery = nullptr;
double g_busySince = 0;
HRESULT STDMETHODCALLTYPE dQueryGetData(IDirect3DQuery9* self, void* data, DWORD size, DWORD flags) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DQuery9*, void*, DWORD, DWORD);
    TableHook* th = findHook(H_QueryGetData, self);
    Depth d(H_QueryGetData);
    if (!th) return D3DERR_INVALIDCALL;
    if (t_depth[H_QueryGetData] > 1 || !counting()) return NEXT(H_QueryGetData, Fn, self)(self, data, size, flags);
    const double t0 = nowMs();
    const HRESULT hr = NEXT(H_QueryGetData, Fn, self)(self, data, size, flags);
    const double t1 = nowMs();
    g_c.queryPolls++;
    if (hr == S_FALSE) {
        g_c.queryBusy++;
        if (g_busyQuery != self) {
            g_busyQuery = self;
            g_busySince = t0;
        }
    } else if (g_busyQuery == self) {
        g_c.queryWaitMs += t1 - g_busySince;
        g_busyQuery = nullptr;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE dSBApply(IDirect3DStateBlock9* self) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DStateBlock9*);
    TableHook* th = findHook(H_SBApply, self);
    Depth d(H_SBApply);
    if (!th) return D3DERR_INVALIDCALL;
    const HRESULT hr = NEXT(H_SBApply, Fn, self)(self);
    if (!g_recording) {
        g_cache.invalidate(); // the block set states we did not see
        g_softwareVP = false; // re-read below
        IDirect3DDevice9* dev = nullptr;
        if (SUCCEEDED(self->GetDevice(&dev)) && dev) {
            g_softwareVP = dev->GetSoftwareVertexProcessing() != FALSE;
            dev->Release();
        }
    }
    return hr;
}

} // namespace

namespace {
const Hook kHooks[] = {
    {kDevice, 57, reinterpret_cast<void*>(&dSetRenderState), "SetRenderState"},
    {kDevice, 69, reinterpret_cast<void*>(&dSetSamplerState), "SetSamplerState"},
    {kDevice, 67, reinterpret_cast<void*>(&dSetTextureStageState), "SetTextureStageState"},
    {kDevice, 65, reinterpret_cast<void*>(&dSetTexture), "SetTexture"},
    {kDevice, 92, reinterpret_cast<void*>(&dSetVertexShader), "SetVertexShader"},
    {kDevice, 107, reinterpret_cast<void*>(&dSetPixelShader), "SetPixelShader"},
    {kDevice, 94, reinterpret_cast<void*>(&dSetVSConstF), "SetVertexShaderConstantF"},
    {kDevice, 109, reinterpret_cast<void*>(&dSetPSConstF), "SetPixelShaderConstantF"},
    {kDevice, 100, reinterpret_cast<void*>(&dSetStreamSource), "SetStreamSource"},
    {kDevice, 104, reinterpret_cast<void*>(&dSetIndices), "SetIndices"},
    {kDevice, 87, reinterpret_cast<void*>(&dSetVertexDeclaration), "SetVertexDeclaration"},
    {kDevice, 89, reinterpret_cast<void*>(&dSetFVF), "SetFVF"},
    {kDevice, 44, reinterpret_cast<void*>(&dSetTransform), "SetTransform"},
    {kDevice, 37, reinterpret_cast<void*>(&dSetRenderTarget), "SetRenderTarget"},
    {kDevice, 43, reinterpret_cast<void*>(&dClear), "Clear"},
    {kDevice, 41, reinterpret_cast<void*>(&dBeginScene), "BeginScene"},
    {kDevice, 81, reinterpret_cast<void*>(&dDrawPrimitive), "DrawPrimitive"},
    {kDevice, 82, reinterpret_cast<void*>(&dDrawIndexedPrimitive), "DrawIndexedPrimitive"},
    {kDevice, 83, reinterpret_cast<void*>(&dDrawPrimitiveUP), "DrawPrimitiveUP"},
    {kDevice, 84, reinterpret_cast<void*>(&dDrawIndexedPrimitiveUP), "DrawIndexedPrimitiveUP"},
    {kDevice, 77, reinterpret_cast<void*>(&dSetSoftwareVP), "SetSoftwareVertexProcessing"},
    {kDevice, 32, reinterpret_cast<void*>(&dGetRenderTargetData), "GetRenderTargetData"},
    {kDevice, 33, reinterpret_cast<void*>(&dGetFrontBufferData), "GetFrontBufferData"},
    {kDevice, 60, reinterpret_cast<void*>(&dBeginStateBlock), "BeginStateBlock"},
    {kDevice, 61, reinterpret_cast<void*>(&dEndStateBlock), "EndStateBlock"},
    {kDevice, 59, reinterpret_cast<void*>(&dCreateStateBlock), "CreateStateBlock"},
    {kDevice, 118, reinterpret_cast<void*>(&dCreateQuery), "CreateQuery"},
    {kDevice, 26, reinterpret_cast<void*>(&dCreateVertexBuffer), "CreateVertexBuffer"},
    {kDevice, 27, reinterpret_cast<void*>(&dCreateIndexBuffer), "CreateIndexBuffer"},
    {kDevice, 23, reinterpret_cast<void*>(&dCreateTexture), "CreateTexture"},
    {kVB, 11, reinterpret_cast<void*>(&dVBLock), "VertexBuffer::Lock"},
    {kIB, 11, reinterpret_cast<void*>(&dIBLock), "IndexBuffer::Lock"},
    {kTex, 19, reinterpret_cast<void*>(&dTexLockRect), "Texture::LockRect"},
    {kSurf, 13, reinterpret_cast<void*>(&dSurfLockRect), "Surface::LockRect"},
    {kQuery, 7, reinterpret_cast<void*>(&dQueryGetData), "Query::GetData"},
    {kSB, 5, reinterpret_cast<void*>(&dSBApply), "StateBlock::Apply"},
};
const int kHookCount = sizeof(kHooks) / sizeof(kHooks[0]);
static_assert(sizeof(kHooks) / sizeof(kHooks[0]) == H_COUNT, "kHooks must follow the H_ order");

// resources we make once to reach the method tables the game's objects share
void probeResourceTables(IDirect3DDevice9* dev) {
    IDirect3DVertexBuffer9* vb = nullptr;
    if (SUCCEEDED(dev->CreateVertexBuffer(64, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr))) {
        hookObject(kVB, vb);
        vb->Release();
    }
    if (SUCCEEDED(dev->CreateVertexBuffer(64, D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &vb, nullptr))) {
        hookObject(kVB, vb);
        vb->Release();
    }
    IDirect3DIndexBuffer9* ib = nullptr;
    if (SUCCEEDED(dev->CreateIndexBuffer(64, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr))) {
        hookObject(kIB, ib);
        ib->Release();
    }
    if (SUCCEEDED(dev->CreateIndexBuffer(64, D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, nullptr))) {
        hookObject(kIB, ib);
        ib->Release();
    }
    IDirect3DTexture9* tex = nullptr;
    if (SUCCEEDED(dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
        hookObject(kTex, tex);
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(tex->GetSurfaceLevel(0, &s))) {
            hookObject(kSurf, s);
            s->Release();
        }
        tex->Release();
    }
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        hookObject(kSurf, bb);
        bb->Release();
    }
    IDirect3DQuery9* q = nullptr;
    if (SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &q))) {
        hookObject(kQuery, q);
        q->Release();
    }
    if (SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q))) {
        hookObject(kQuery, q);
        q->Release();
    }
    IDirect3DStateBlock9* sb = nullptr;
    if (SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) {
        hookObject(kSB, sb);
        sb->Release();
    }
}

void decideFilter() {
    g_filterWhy = "";
    if (g_facts.foreignHooks) g_filterWhy = "another mod hooks Direct3D before Horizon (it may bypass the filter)";
    else if (g_facts.wrapper) g_filterWhy = "a wrapper d3d9.dll is in use (TM Vibrant Shaders or a repack proxy)";
    else if (g_facts.d3d9Path[0]) {
        char sys[MAX_PATH] = {};
        GetSystemDirectoryA(sys, MAX_PATH);
        if (_strnicmp(g_facts.d3d9Path, sys, strlen(sys)) != 0)
            g_filterWhy = "a wrapper d3d9.dll is in use (TM Vibrant Shaders or a repack proxy)";
    }
}
} // namespace

bool attach(IDirect3DDevice9* dev, LogFn log) {
    g_log = log;
    if (!dev) return false;
    if (g_qpcToMs == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcToMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    // which d3d9.dll made this device: the module its Present lives in
    void** table = *reinterpret_cast<void***>(dev);
    HMODULE m = nullptr;
    if (!g_d3d9) {
        // The method table itself may live on the heap (current d3d9 builds copy it), so the module comes from
        // GetDeviceCaps (slot 7): a slot no mod hooks.
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(table[7]), &m))
            g_d3d9 = m;
        GetModuleFileNameA(g_d3d9 ? g_d3d9 : GetModuleHandleW(L"d3d9.dll"), g_facts.d3d9Path, sizeof(g_facts.d3d9Path));
        // a wrapper d3d9.dll in the game folder (TM Vibrant Shaders, the Sunrise repack's proxy) is loaded too
        char first[MAX_PATH] = "";
        GetModuleFileNameA(GetModuleHandleW(L"d3d9.dll"), first, sizeof(first));
        if (_stricmp(first, g_facts.d3d9Path) != 0) g_facts.wrapper = true;
    }
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    if (SUCCEEDED(dev->GetCreationParameters(&cp))) {
        g_facts.behaviorFlags = cp.BehaviorFlags;
        g_facts.pure = (cp.BehaviorFlags & D3DCREATE_PUREDEVICE) != 0;
        g_facts.mixed = (cp.BehaviorFlags & D3DCREATE_MIXED_VERTEXPROCESSING) != 0;
        g_facts.software = (cp.BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0;
        g_facts.multithreaded = (cp.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0;
    }
    g_softwareVP = g_facts.software || (g_facts.mixed && dev->GetSoftwareVertexProcessing());
    hookTable(kDevice, table);
    probeResourceTables(dev);
    g_attached = true;
    g_cache.invalidate();
    decideFilter();
    DLOG(1, "d3d: device flags 0x%08lx (%s%s%s%s), d3d9 %s%s", static_cast<unsigned long>(g_facts.behaviorFlags),
         g_facts.pure ? "pure " : "", g_facts.mixed ? "mixed VP" : g_facts.software ? "software VP" : "hardware VP",
         g_facts.multithreaded ? ", multithreaded" : "", "", g_facts.d3d9Path,
         g_facts.foreignHooks ? "; another mod's hook sat in a slot before ours" : "");
    return true;
}

void detach() {
    for (int h = 0; h < kHookCount; h++)
        for (int i = 0; i < kMaxTables; i++) {
            TableHook& t = g_tables[h][i];
            if (!t.active) continue;
            void** slot = &t.table[kHooks[h].index];
            void* cur = nullptr;
            if (readSlot(slot, cur) && cur == kHooks[h].detour) writeSlot(slot, t.next);
            // else: someone chained after us; our detour stays reachable and passes through (counting off)
            t.active = false;
        }
    g_count = g_filterWanted = false;
    g_attached = false;
}

bool attached() { return g_attached; }

void setMode(bool count, bool filter) {
    g_count = count;
    if (filter && !g_filterWanted) g_cache.invalidate();
    g_filterWanted = filter;
}

const char* filterBlockedWhy() { return g_filterWhy; }

void endFrame(CallCounts& out) {
    out = g_c;
    g_c = CallCounts{};
    g_cache.forgetPointers();
    g_busyQuery = nullptr;
}

void setOverlay(bool overlay) { g_overlay = overlay; }

void recheck() {
    for (int h = 0; h < kHookCount; h++)
        for (int i = 0; i < kMaxTables; i++) {
            TableHook& t = g_tables[h][i];
            if (!t.active) continue;
            void** slot = &t.table[kHooks[h].index];
            void* cur = nullptr;
            if (!readSlot(slot, cur) || cur == kHooks[h].detour) continue;
            t.next = cur; // the other mod's hook runs after ours
            writeSlot(slot, kHooks[h].detour);
            if (!g_facts.foreignHooks) {
                g_facts.foreignHooks = true;
                decideFilter();
            }
            static int s_lines = 0;
            if (s_lines++ < 10) DLOG(2, "d3d: %s was written over by another mod: ours in front again", kHooks[h].name);
        }
}

void onReset() {
    g_cache.invalidate();
    g_recording = false;
}

const DeviceFacts& facts() { return g_facts; }

} // namespace d3d
} // namespace perf
