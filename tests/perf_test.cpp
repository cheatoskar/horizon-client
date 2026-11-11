// Tests of the Horizon Performance plugin without a game.
//
//   perf_test unit                         statistics, limiter maths, state cache, benchmark request, files
//   perf_test host <dir> <out.txt> [shot.png] [stall]
//       The real host (test mode) with the performance plugin installed in <dir>, a D3D9 device made like the
//       game's (HAL, mixed vertex processing), 150 frames of a test scene with repeated states, locks and an
//       event query polled like the engine's GPU sync. Writes the scene checksum and the measured frame time
//       to <out.txt>; with "stall" the main thread also sleeps 2.5 s once (the hang log must catch it).
//   perf_test compare <a.txt> <b.txt>      same scene checksum (the state filter changes nothing on screen)
//   perf_test limit <a.txt> <fps>          measured average frame time within 4 % of 1000/fps
// Exit code 0 = passed.
#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../plugins/performance/perf_core.h"

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (cond) g_passed++;                                          \
        else {                                                         \
            g_failed++;                                                \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
        }                                                              \
    } while (0)
static bool approx(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// ------------------------------------------------------------------ unit
static void testStats() {
    using namespace perf;
    // 1000 frames: 990 at 10 ms, 10 at 50 ms
    std::vector<FrameSample> f(1000);
    for (size_t i = 0; i < f.size(); i++) {
        f[i].frameMs = i % 100 == 99 ? 50.0 : 10.0;
        f[i].presentMs = 2;
        f[i].cpuMs = f[i].frameMs - 2;
    }
    Summary s = summarize(f);
    CHECK(s.frames == 1000);
    CHECK(approx(s.avgMs, (990 * 10.0 + 10 * 50.0) / 1000, 1e-9));
    CHECK(approx(s.medianMs, 10, 1e-9));
    CHECK(approx(s.p99Ms, 10, 1e-9));   // rank 990 is still 10 ms
    CHECK(approx(s.p999Ms, 50, 1e-9));
    CHECK(approx(s.maxMs, 50, 1e-9));
    CHECK(approx(s.low1Fps, 20, 1e-9));  // slowest 10 frames average 50 ms
    CHECK(approx(s.low01Fps, 20, 1e-9)); // slowest 1 frame: 50 ms
    CHECK(s.stutters == 10);           // > 2 x median
    CHECK(approx(s.presentMsAvg, 2, 1e-9));
    CHECK(s.drawsPerFrame < 0);        // counting was off
    CHECK(approx(s.seconds, 10.4, 1e-9));

    // empty and single frame
    CHECK(summarize({}).frames == 0);
    std::vector<FrameSample> one(1);
    one[0].frameMs = 16;
    s = summarize(one);
    CHECK(approx(s.avgFps, 62.5, 1e-9) && s.stutters == 0 && approx(s.low01Fps, 62.5, 1e-9));

    // calls per frame
    std::vector<FrameSample> c(4);
    for (auto& x : c) {
        x.frameMs = 5;
        x.hasCalls = true;
        x.calls.draws = 100;
        x.calls.drawsUP = 20;
    }
    CHECK(approx(summarize(c).drawsPerFrame, 120, 1e-9));

    // percentile edge cases
    std::vector<double> v = {1, 2, 3, 4};
    CHECK(percentileSorted(v, 50) == 2 && percentileSorted(v, 75) == 3 && percentileSorted(v, 100) == 4 && percentileSorted(v, 0) == 1);
}

static void testRing() {
    perf::FrameRing r(4);
    for (int i = 1; i <= 6; i++) {
        perf::FrameSample s;
        s.frameMs = i;
        r.push(s);
    }
    CHECK(r.size() == 4);
    CHECK(r.at(0).frameMs == 3 && r.newest().frameMs == 6);
    auto l = r.last(2);
    CHECK(l.size() == 2 && l[0].frameMs == 5 && l[1].frameMs == 6);
    CHECK(r.last(10).size() == 4);
}

static void testPacer() {
    perf::Pacer p;
    CHECK(p.next(5.0) == 5.0); // off: no wait
    p.setFps(100);             // 10 ms
    double t = 0.0;
    const double d1 = p.next(t);
    CHECK(approx(d1, 0.010, 1e-12));
    // a frame that took 4 ms waits until the next grid point
    CHECK(approx(p.next(0.014), 0.020, 1e-12));
    // slightly late (2 ms): the grid stays, the next deadline is the next grid point
    CHECK(approx(p.next(0.022), 0.030, 1e-12));
    // a hitch of 50 ms: no burst of catch-up frames, a new grid from now
    CHECK(approx(p.next(0.080), 0.090, 1e-12));
    CHECK(approx(p.next(0.0905), 0.100, 1e-12));
    p.reset();
    CHECK(approx(p.next(1.0), 1.010, 1e-12));
}

static void testStateCache() {
    perf::StateCache c;
    CHECK(!c.renderStateSame(7, 1)); // unknown at first: never filtered
    c.setRenderState(7, 1);
    CHECK(c.renderStateSame(7, 1));
    CHECK(!c.renderStateSame(7, 0));
    CHECK(!c.renderStateSame(300, 0)); // out of range: never filtered
    c.setRenderState(300, 0);
    c.setSamplerState(0, 5, 2);
    CHECK(c.samplerStateSame(0, 5, 2) && !c.samplerStateSame(1, 5, 2));
    c.setSamplerState(257, 5, 2); // vertex sampler 1
    CHECK(c.samplerStateSame(257, 5, 2) && !c.samplerStateSame(258, 5, 2));
    CHECK(perf::StateCache::samplerSlot(256) == 16 && perf::StateCache::samplerSlot(260) == 20 && perf::StateCache::samplerSlot(16) == -1);
    c.setSamplerState(100, 1, 1); // not a sampler: ignored
    CHECK(!c.samplerStateSame(100, 1, 1));
    int tex = 0, vs = 0;
    c.setTexture(3, &tex);
    CHECK(c.textureSame(3, &tex) && !c.textureSame(3, nullptr) && !c.textureSame(2, &tex));
    c.setTexture(3, nullptr);
    CHECK(c.textureSame(3, nullptr));
    c.setVertexShader(&vs);
    CHECK(c.vertexShaderSame(&vs) && !c.pixelShaderSame(nullptr));
    c.setStageState(1, 4, 9);
    CHECK(c.stageStateSame(1, 4, 9) && !c.stageStateSame(1, 4, 8) && !c.stageStateSame(8, 4, 9));
    c.forgetPointers();
    CHECK(!c.textureSame(3, nullptr) && !c.vertexShaderSame(&vs));
    CHECK(c.renderStateSame(7, 1)); // values survive forgetPointers
    c.invalidate();
    CHECK(!c.renderStateSame(7, 1) && !c.samplerStateSame(0, 5, 2) && !c.stageStateSame(1, 4, 9));
}

static void testRequest() {
    perf::BenchRequest r;
    std::string why;
    CHECK(perf::parseBenchRequest(
        R"({"version":1,"mode":"map","map":"C:\\maps\\A01.Challenge.Gbx","seconds":30,"warmup":5,"label":"state cache!","runs":3,"returnToMenu":false})",
        r, why));
    CHECK(r.mode == "map" && r.map == "C:\\maps\\A01.Challenge.Gbx" && r.seconds == 30 && r.warmup == 5 && r.runs == 3 && !r.returnToMenu);
    CHECK(r.label == "state_cache_");
    CHECK(perf::parseBenchRequest(R"({"version":1,"mode":"replay"})", r, why) && r.mode == "replay" && r.runs == 1);
    CHECK(!perf::parseBenchRequest(R"({"version":1,"mode":"map"})", r, why));          // no map
    CHECK(!perf::parseBenchRequest(R"({"version":2,"mode":"replay"})", r, why));        // newer format
    CHECK(!perf::parseBenchRequest(R"({"version":1,"mode":"replay","runs":50})", r, why));
    CHECK(!perf::parseBenchRequest(R"({"version":1,"mode":"replay","seconds":1})", r, why));
    CHECK(!perf::parseBenchRequest("not json", r, why) && !why.empty());
    CHECK(!perf::parseBenchRequest("[1,2]", r, why));
    CHECK(perf::safeLabel("") == "run" && perf::safeLabel("..\\x") == "x" && perf::safeLabel(std::string(80, 'a')).size() == 40);
}

static void testFiles() {
    std::vector<perf::FrameSample> f(3);
    f[0].frameMs = 10;
    f[1].frameMs = 12;
    f[1].hasCalls = true;
    f[1].calls.draws = 590;
    f[2].frameMs = 30;
    const std::string csv = perf::framesCsv(f);
    CHECK(csv.find("frame,frame_ms,present_ms") == 0);
    CHECK(csv.find("\n1,12.000,") != std::string::npos && csv.find(",590,") != std::string::npos);
    size_t lines = 0;
    for (char ch : csv) lines += ch == '\n';
    CHECK(lines == 4);
    // every line has the same number of columns
    size_t cols = 0, pos = 0;
    bool same = true;
    while (pos < csv.size()) {
        const size_t e = csv.find('\n', pos);
        size_t n = 1;
        for (size_t i = pos; i < e; i++) n += csv[i] == ',';
        if (!cols) cols = n;
        same = same && n == cols;
        pos = e + 1;
    }
    CHECK(same && cols == 34);
    perf::ResultMeta m;
    m.game = "tmnf";
    m.label = "baseline";
    m.request = "map";
    m.started = "2026-11-11T14:00:00";
    m.options = {{"limitFps", "0"}, {"filter", "1"}};
    const std::string js = perf::summaryJson(perf::summarize(f), m);
    CHECK(js.find("\"game\": \"tmnf\"") != std::string::npos);
    CHECK(js.find("\"frames\": 3") != std::string::npos);
    CHECK(js.find("\"options\": {\"limitFps\": 0, \"filter\": 1}") != std::string::npos);
    CHECK(js.find("\"drawsPerFrame\": 590.0") != std::string::npos);
}

static int unit() {
    testStats();
    testRing();
    testPacer();
    testStateCache();
    testRequest();
    testFiles();
    printf("perf unit tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

// ------------------------------------------------------------------ host
struct Vtx {
    float x, y, z, rhw;
    DWORD color;
};

// the test scene into an offscreen target, returns a checksum of its pixels
static uint32_t drawScene(IDirect3DDevice9* dev, IDirect3DSurface9* target, IDirect3DSurface9* sysmem, IDirect3DTexture9* tex,
                          IDirect3DVertexBuffer9* dynVb, IDirect3DQuery9* query, bool readBack) {
    IDirect3DSurface9* back = nullptr;
    dev->GetRenderTarget(0, &back);
    dev->SetRenderTarget(0, target);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(10, 20, 30), 1.0f, 0);
    dev->BeginScene();
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    for (int i = 0; i < 200; i++) {
        // the engine sets the same states again and again; the filter may skip the repeats
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, (i % 3) == 0);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetTexture(0, (i % 2) ? tex : nullptr);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, (i % 2) ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        const float x = static_cast<float>((i * 37) % 600), y = static_cast<float>((i * 53) % 400);
        const DWORD c = D3DCOLOR_ARGB(128, (i * 40) & 255, (i * 90) & 255, (i * 15) & 255);
        Vtx v[3] = {{x, y, 0.5f, 1, c}, {x + 60, y, 0.5f, 1, c}, {x, y + 60, 0.5f, 1, c}};
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vtx));
    }
    // a dynamic buffer like the engine's particles: one DISCARD lock, one plain lock
    void* p = nullptr;
    if (SUCCEEDED(dynVb->Lock(0, 0, &p, D3DLOCK_DISCARD))) dynVb->Unlock();
    if (SUCCEEDED(dynVb->Lock(0, 64, &p, 0))) dynVb->Unlock();
    dev->EndScene();
    // GPU sync like CVisionViewportDx9::SyncGpuWaitQuery: issue, poll with FLUSH, Sleep(0)
    query->Issue(D3DISSUE_END);
    BOOL done = FALSE;
    while (query->GetData(&done, sizeof(done), D3DGETDATA_FLUSH) == S_FALSE) Sleep(0);
    uint32_t sum = 0;
    if (readBack && SUCCEEDED(dev->GetRenderTargetData(target, sysmem))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sysmem->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            for (int yy = 0; yy < 480; yy++) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const char*>(lr.pBits) + yy * lr.Pitch);
                for (int xx = 0; xx < 680; xx++) sum = sum * 16777619u ^ (row[xx] & 0xffffffu);
            }
            sysmem->UnlockRect();
        }
    }
    dev->SetRenderTarget(0, back);
    // show it on the back buffer, so the screenshot has the scene behind the menu
    RECT dst = {0, 0, 680, 480};
    dev->StretchRect(target, nullptr, back, &dst, D3DTEXF_NONE);
    back->Release();
    return sum;
}

static bool savePng(IDirect3DDevice9* dev, const wchar_t* path) {
    IDirect3DSurface9* back = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return false;
    D3DSURFACE_DESC desc;
    back->GetDesc(&desc);
    IDirect3DSurface9* sys = nullptr;
    bool ok = false;
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(back, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            std::vector<BYTE> rgb(desc.Width * desc.Height * 3);
            for (UINT y = 0; y < desc.Height; y++)
                for (UINT x = 0; x < desc.Width; x++) {
                    const BYTE* s = static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch + x * 4;
                    memcpy(&rgb[(y * desc.Width + x) * 3], s, 3);
                }
            sys->UnlockRect();
            IWICImagingFactory* f = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
                IWICStream* st = nullptr;
                IWICBitmapEncoder* enc = nullptr;
                IWICBitmapFrameEncode* fr = nullptr;
                if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) &&
                    SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
                    SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
                    SUCCEEDED(fr->SetSize(desc.Width, desc.Height))) {
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
                    fr->SetPixelFormat(&fmt);
                    ok = SUCCEEDED(fr->WritePixels(desc.Height, desc.Width * 3, static_cast<UINT>(rgb.size()), rgb.data())) &&
                         SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
                }
                if (fr) fr->Release();
                if (enc) enc->Release();
                if (st) st->Release();
                f->Release();
            }
        }
    }
    if (sys) sys->Release();
    back->Release();
    return ok;
}

static std::string readAll(const std::wstring& path) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(f, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
    CloseHandle(f);
    return out;
}
static std::string newestLog(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\horizon\\logs\\*.log").c_str(), &fd);
    std::wstring best;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (best.empty() || wcscmp(fd.cFileName, best.c_str()) > 0) best = fd.cFileName;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return best.empty() ? std::string() : readAll(dir + L"\\horizon\\logs\\" + best);
}
static bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

__declspec(noinline) static void stallInsideTestFunction() { Sleep(2500); }

static int host(const std::wstring& dir, const std::wstring& outTxt, const std::wstring& shot, bool stall) {
    struct Info {
        unsigned api, size, flags;
        const wchar_t* path;
    };
    SetEnvironmentVariableW(L"HORIZON_TEST_MENU", shot.empty() ? L"0" : L"1");
    SetEnvironmentVariableW(L"HORIZON_TEST_PAGE", L"plugin:performance");
    HMODULE hostDll = LoadLibraryW((dir + L"\\horizon\\host.dll").c_str());
    CHECK(hostDll != nullptr);
    if (!hostDll) return 1;
    using InitFn = int(__cdecl*)(const Info*);
    auto init = reinterpret_cast<InitFn>(GetProcAddress(hostDll, "HorizonHostInit"));
    Info info = {1, sizeof(Info), 2u, L""};
    CHECK(init && init(&info) == 1);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HorizonPerfTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"perf test", WS_OVERLAPPEDWINDOW, 0, 0, 1600, 900, nullptr, nullptr, wc.hInstance, nullptr);
    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    using Create9 = IDirect3D9*(WINAPI*)(UINT);
    auto create = reinterpret_cast<Create9>(GetProcAddress(d3d9, "Direct3DCreate9"));
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    CHECK(d3d != nullptr);
    if (!d3d) return 1;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 1600;
    pp.BackBufferHeight = 900;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    // like the game: mixed vertex processing (flags 0x80), not pure
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_MIXED_VERTEXPROCESSING, &pp, &dev);
    CHECK(SUCCEEDED(hr) && dev);
    if (!dev) return 1;
    IDirect3DSurface9 *target = nullptr, *sysmem = nullptr;
    IDirect3DTexture9* tex = nullptr;
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DQuery9* query = nullptr;
    dev->CreateRenderTarget(680, 480, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr);
    dev->CreateOffscreenPlainSurface(680, 480, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, nullptr);
    dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    dev->CreateVertexBuffer(4096, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr);
    dev->CreateQuery(D3DQUERYTYPE_EVENT, &query);
    CHECK(target && sysmem && tex && vb && query);
    if (!(target && sysmem && tex && vb && query)) return 1;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    uint32_t checksum = 0;
    double measuredMs = 0;
    int measured = 0;
    for (int i = 0; i < 150; i++) {
        const uint32_t c = drawScene(dev, target, sysmem, tex, vb, query, i == 149);
        if (i == 149) checksum = c;
        QueryPerformanceCounter(&t0);
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        if (i >= 50 && i <= 148) { // Present to Present over ~100 frames (not the last: it reads the image back)
            static LARGE_INTEGER prev = {};
            if (prev.QuadPart) {
                measuredMs += (t0.QuadPart - prev.QuadPart) * 1000.0 / freq.QuadPart;
                measured++;
            }
            prev = t0;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (stall && i == 100) stallInsideTestFunction();
    }
    (void)t1;
    if (stall) { // the measured average is not meaningful with a 2.5 s pause in it
        measured = 0;
        Sleep(400); // let the watcher write the "stall end" line
    }
    if (!shot.empty()) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CHECK(savePng(dev, shot.c_str()));
    }
    char out[200];
    snprintf(out, sizeof(out), "checksum %08x\nframe_ms %.4f\n", checksum, measured ? measuredMs / measured : 0.0);
    FILE* f = _wfopen(outTxt.c_str(), L"wb");
    if (f) {
        fputs(out, f);
        fclose(f);
    }
    printf("%s", out);

    const std::string log = newestLog(dir);
    CHECK(has(log, "[performance] started"));
    CHECK(has(log, "post_present yes"));
    CHECK(!has(log, "crashed"));
    const std::string settings = readAll(dir + L"\\horizon\\settings\\performance.json");
    if (has(settings, "\"count\": 1")) {
        CHECK(has(log, "slots hooked in the device method table"));
        CHECK(has(log, "slots hooked in the query method table"));
        CHECK(has(log, "device flags 0x00000080 (mixed VP"));
        CHECK(has(log, "stats: "));
        CHECK(has(log, "draws (0 software VP)")); // the scene draws with hardware vertex processing
        CHECK(!has(log, "another mod's hook sat in a slot")); // only the host's own Present/Reset hooks are there
        if (has(settings, "\"filter\": 1")) CHECK(!has(log, "filtered 0\r\n") && !has(log, "filtered 0\n"));
        else CHECK(has(log, "filtered 0"));
    }
    if (stall) {
        const std::string hangs = readAll(dir + L"\\horizon\\data\\performance\\hangs.log");
        CHECK(has(hangs, "stall: no frame for"));
        CHECK(has(hangs, "stall end:"));
        CHECK(has(hangs, "sample 1 at"));
        CHECK(has(hangs, "Sleep")); // the sampled stack shows the wait (KERNELBASE!SleepEx or ntdll)
        printf("--- hangs.log ---\n%s-----------------\n", hangs.c_str());
    }
    query->Release();
    vb->Release();
    tex->Release();
    sysmem->Release();
    target->Release();
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    printf("perf host test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

static bool readOut(const wchar_t* path, unsigned* checksum, double* ms) {
    const std::string s = readAll(path);
    return sscanf_s(s.c_str(), "checksum %x\nframe_ms %lf", checksum, ms) == 2;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 2 && !wcscmp(argv[1], L"unit")) return unit();
    if (argc >= 4 && !wcscmp(argv[1], L"host")) {
        std::wstring shot;
        bool stall = false;
        for (int i = 4; i < argc; i++) {
            if (!wcscmp(argv[i], L"stall")) stall = true;
            else shot = argv[i];
        }
        const int r = host(argv[2], argv[3], shot, stall);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(r)); // the host's threads stay until the process ends
        return r;
    }
    if (argc >= 4 && !wcscmp(argv[1], L"compare")) {
        unsigned a = 0, b = 0;
        double ma = 0, mb = 0;
        const bool ok = readOut(argv[2], &a, &ma) && readOut(argv[3], &b, &mb) && a == b && a != 0;
        printf("%s scene checksum %08x vs %08x; frame %.3f ms vs %.3f ms\n", ok ? "PASS" : "FAIL", a, b, ma, mb);
        return ok ? 0 : 1;
    }
    if (argc >= 4 && !wcscmp(argv[1], L"limit")) {
        unsigned a = 0;
        double ms = 0;
        const double fps = _wtof(argv[3]);
        const bool ok = readOut(argv[2], &a, &ms) && fps > 0 && std::fabs(ms - 1000.0 / fps) <= 0.04 * 1000.0 / fps;
        printf("%s limiter: %.3f ms per frame, target %.3f ms\n", ok ? "PASS" : "FAIL", ms, fps > 0 ? 1000.0 / fps : 0.0);
        return ok ? 0 : 1;
    }
    printf("usage: perf_test unit | host <dir> <out.txt> [shot.png] [stall] | compare <a> <b> | limit <a> <fps>\n");
    return 2;
}
