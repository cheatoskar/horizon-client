// Integration test without a game: loads the real loader/host DLLs in this process.
//
//   host_test loader <dir>        dir\dinput8.dll (Legacy Loader) + dir\dinput8.chain.dll (test chain) +
//                                 dir\horizon\host.dll: DirectInput8Create must work, go through the chain,
//                                 and the host must stay OFF (this exe is not a known game) and say so.
//   host_test host <dir> [safe] [expect-plugin|expect-no-plugin] [expect-abi12]
//                                 HorizonHostInit in test mode, then a D3D9 device on a hidden window, frames
//                                 with the menu open (HORIZON_TEST_MENU=1), a PNG of the frame into screens\.
//                                 widgets / widgets-anchored: the sample's Clock widget through a play session
//                                 (testWidgets below).
// Exit code 0 = all checks passed. The host log of the run is checked for the expected lines.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <unknwn.h>
#include <d3d9.h>
#include <dinput.h>
#include <wincodec.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "horizon_plugin.h" // HORIZON_EVENT_* for the widgets run

static int g_failed = 0;
static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failed++;
}

static std::string readAll(const std::wstring& path) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(f);
    return out;
}

// The newest log in dir\horizon\logs.
static std::string hostLog(const std::wstring& dir) {
    const std::wstring logs = dir + L"\\horizon\\logs\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((logs + L"*.log").c_str(), &fd);
    std::wstring newest;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (newest.empty() || wcscmp(fd.cFileName, newest.c_str()) > 0) newest = fd.cFileName;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return newest.empty() ? std::string() : readAll(logs + newest);
}

static bool has(const std::string& log, const char* text) { return log.find(text) != std::string::npos; }

static int testLoader(const std::wstring& dir) {
    HMODULE loader = LoadLibraryW((dir + L"\\dinput8.dll").c_str());
    check(loader != nullptr, "Legacy Loader loads");
    if (!loader) return 1;
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    auto create = reinterpret_cast<Fn>(GetProcAddress(loader, "DirectInput8Create"));
    check(create != nullptr, "DirectInput8Create exported");
    IDirectInput8W* di = nullptr;
    const HRESULT hr = create ? create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&di), nullptr) : E_FAIL;
    check(SUCCEEDED(hr) && di, "DirectInput8Create returns a DirectInput object");
    if (di) di->Release();
    check(GetFileAttributesW((dir + L"\\chain_called.txt").c_str()) != INVALID_FILE_ATTRIBUTES, "the call went through dinput8.chain.dll");
    check(GetModuleHandleW((dir + L"\\horizon\\host.dll").c_str()) != nullptr, "horizon\\host.dll was loaded");
    const std::string log = hostLog(dir);
    check(has(log, "host stays OFF"), "unknown exe: the host stays off and logs why");
    check(!has(log, "d3d:"), "unknown exe: nothing hooked");
    return g_failed ? 1 : 0;
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
            std::vector<BYTE> rgb(static_cast<size_t>(desc.Width) * desc.Height * 3);
            for (UINT y = 0; y < desc.Height; y++) {
                const BYTE* row = static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
                for (UINT x = 0; x < desc.Width; x++) memcpy(&rgb[(static_cast<size_t>(y) * desc.Width + x) * 3], row + x * 4, 3);
            }
            sys->UnlockRect();
            IWICImagingFactory* factory = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
                IWICStream* stream = nullptr;
                IWICBitmapEncoder* enc = nullptr;
                IWICBitmapFrameEncode* frame = nullptr;
                if (SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
                    SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                    SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
                    SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(desc.Width, desc.Height))) {
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
                    frame->SetPixelFormat(&fmt);
                    ok = SUCCEEDED(frame->WritePixels(desc.Height, desc.Width * 3, static_cast<UINT>(rgb.size()), rgb.data())) &&
                         SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
                }
                if (frame) frame->Release();
                if (enc) enc->Release();
                if (stream) stream->Release();
                factory->Release();
            }
        }
    }
    if (sys) sys->Release();
    back->Release();
    return ok;
}

static int g_w = 1600, g_h = 900; // size=WxH: back buffer (UI screens at other resolutions)
static bool g_runtimeLoad = false; // runtime-load: switch modules on after the device exists
static bool g_crash = false;       // crash: crashmenu/crashoverlay throw while drawing, then the host's own frame throws

static size_t countOf(const std::string& log, const char* text) {
    size_t n = 0;
    for (size_t at = log.find(text); at != std::string::npos; at = log.find(text, at + 1)) n++;
    return n;
}

// widgets / widgets-anchored: the sample's Clock widget (a HUD panel kept on screen) through what a play session
// does to it: menu closed, map load, race start, checkpoint, finish, race end, the next map, the game menu, the
// Horizon menu, Resets at the same and at a smaller size and back. It must stay on screen the whole time, at the
// place saved in host.json (run_tests.cmd seeds it), and come back there after the smaller window.
//   widgets:          a pixel entry from 0.3.0 at 1300,40
//   widgets-anchored: the shared setup (HORIZON_SETUP_DIR) with an anchored entry: right edge, 10 % of the
//                     width from it, 40/900 of the height from the top
static bool g_widgets = false, g_anchored = false;

static void testWidgets(HMODULE host, IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS& pp, HWND hwnd, const std::wstring& dir) {
    using PanelFn = int(__cdecl*)(const char*, const char*, float*);
    using EventFn = int(__cdecl*)(int, const char*);
    using FaultFn = int(__cdecl*)(int);
    auto panel = reinterpret_cast<PanelFn>(GetProcAddress(host, "HorizonHost_TestPanel"));
    auto event = reinterpret_cast<EventFn>(GetProcAddress(host, "HorizonHost_TestEvent"));
    auto overlay = reinterpret_cast<FaultFn>(GetProcAddress(host, "HorizonHost_TestOverlayFault"));
    check(panel && event && overlay, "widgets: test exports (TestPanel, TestEvent, TestOverlayFault)");
    if (!panel || !event || !overlay) return;
    auto frames = [&](int n) {
        for (int i = 0; i < n; i++) {
            dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(24, 30, 52), 1.0f, 0);
            dev->BeginScene();
            dev->EndScene();
            dev->Present(nullptr, nullptr, nullptr, nullptr);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        }
    };
    auto resize = [&](int w, int h) {
        SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        pp.BackBufferWidth = static_cast<UINT>(w);
        pp.BackBufferHeight = static_cast<UINT>(h);
        const HRESULT r = dev->Reset(&pp);
        char what[96];
        snprintf(what, sizeof(what), "widgets: Reset to %dx%d", w, h);
        check(SUCCEEDED(r), what);
        frames(6);
    };
    float r[6] = {};
    // where it belongs at this size
    auto expected = [&](float screenW, float screenH, float w, float& ex, float& ey) {
        if (g_anchored) {
            ex = screenW - 0.1f * screenW - w;
            ey = std::floor(40.0f / 900.0f * screenH + 0.5f);
        } else {
            ex = 1300.0f;
            ey = 40.0f;
        }
        ex = std::floor(ex + 0.5f);
    };
    auto onScreenAtPlace = [&](const char* when) {
        const int shown = panel("sample", "clock", r);
        float ex, ey;
        expected(static_cast<float>(g_w), static_cast<float>(g_h), r[2], ex, ey);
        char what[160];
        snprintf(what, sizeof(what), "widgets: %s: Clock on screen at its place (%.0f,%.0f, wanted %.0f,%.0f)", when, r[0], r[1], ex, ey);
        check(shown == 1 && std::fabs(r[0] - ex) <= 1.5f && std::fabs(r[1] - ey) <= 1.5f, what);
    };

    overlay(3); // the Horizon menu closed: widgets alone on screen
    frames(6);
    check((overlay(0) & 1) == 0, "widgets: menu closed");
    onScreenAtPlace("menu closed");
    const struct {
        int type;
        const char* map;
        const char* what;
    } steps[] = {
        {HORIZON_EVENT_MAP_LOADED, "A01-Race", "map loaded"},
        {HORIZON_EVENT_RACE_START, nullptr, "race start"},
        {HORIZON_EVENT_CHECKPOINT, nullptr, "checkpoint"},
        {HORIZON_EVENT_RESPAWN, nullptr, "respawn"},
        {HORIZON_EVENT_FINISH, nullptr, "finish"},
        {HORIZON_EVENT_RACE_END, nullptr, "race end"},
        {HORIZON_EVENT_MAP_LOADED, "A02-Race", "next map loaded"},
        {HORIZON_EVENT_RACE_START, nullptr, "next race start"},
        {HORIZON_EVENT_FINISH, nullptr, "next finish"},
        {HORIZON_EVENT_MENU_ENTERED, nullptr, "game menu"},
        {HORIZON_EVENT_MAP_LOADED, "A03-Race", "third map loaded"},
    };
    for (const auto& s : steps) {
        event(s.type, s.map);
        frames(4);
        onScreenAtPlace(s.what);
    }
    overlay(2);
    frames(4);
    onScreenAtPlace("Horizon menu open");
    overlay(3);
    frames(4);
    onScreenAtPlace("Horizon menu closed again");
    resize(g_w, g_h); // a Reset at the same size (the game does one per map load on some setups)
    onScreenAtPlace("Reset, same size");

    // a smaller window: kept on screen, at the right edge, without losing the saved place
    const int bigW = g_w, bigH = g_h;
    g_w = 1024;
    g_h = 768;
    resize(g_w, g_h);
    const int shown = panel("sample", "clock", r);
    check(shown == 1 && r[0] >= 0.0f && r[0] + r[2] <= 1024.5f && r[1] >= 0.0f && r[1] + r[3] <= 768.5f, "widgets: 1024x768: Clock fully on screen");
    check(r[0] > 512.0f, "widgets: 1024x768: still on the right side");
    if (g_anchored) onScreenAtPlace("1024x768, anchored to the right edge");
    else check(std::fabs(r[4] - 1300.0f) < 0.5f && std::fabs(r[5] - 40.0f) < 0.5f, "widgets: 1024x768: the saved place stays 1300,40");
    event(HORIZON_EVENT_MAP_LOADED, "A04-Race");
    frames(4);
    check(panel("sample", "clock", r) == 1, "widgets: 1024x768: map loaded, Clock still on screen");
    g_w = bigW;
    g_h = bigH;
    resize(g_w, g_h);
    onScreenAtPlace("back to the first size");
    std::string l = hostLog(dir);
    check(!has(l, "crashed"), "widgets: no plugin crashed");
    if (g_anchored) {
        check(has(l, "setup: shared ("), "shared setup: the host reads the shared setup");
        check(has(l, "bridge: off"), "shared setup: the TM Horizon switch stays per game (bridge.enabled in the shared file is ignored)");
    }
    overlay(2); // as the other runs: the menu open for what follows
    frames(4);
}

static int testHost(const std::wstring& dir, bool safe, int expectPlugin, bool expectAbi12, const std::wstring& shot) {
    struct Info {
        unsigned api, size, flags;
        const wchar_t* path;
    };
    SetEnvironmentVariableW(L"HORIZON_TEST_MENU", L"1");
    SetEnvironmentVariableW(L"HORIZON_TEST_SAVE_REPLAY", L"1"); // the sample calls save_replay once in Init
    HMODULE host = LoadLibraryW((dir + L"\\horizon\\host.dll").c_str());
    check(host != nullptr, "host.dll loads");
    if (!host) return 1;
    using InitFn = int(__cdecl*)(const Info*);
    auto init = reinterpret_cast<InitFn>(GetProcAddress(host, "HorizonHostInit"));
    Info info = {1, sizeof(Info), (safe ? 1u : 0u) | 2u, L""};
    check(init && init(&info) == 1, "HorizonHostInit (test mode) starts the host");
    std::string log = hostLog(dir);
    check(has(log, "Horizon Client (host"), "log file written in horizon\\logs");
    if (safe) check(has(log, "safe mode"), "safe mode logged");
    if (expectPlugin == 1) check(has(log, "[sample] started"), "the sample plugin started");
    if (expectPlugin == 0) check(!has(log, "[sample] started"), "the sample plugin did not start");

    // a D3D9 device on a hidden window, created the way the game does it
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HorizonHostTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Horizon host test", WS_POPUP, 0, 0, g_w, g_h, nullptr, nullptr, wc.hInstance, nullptr);
    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    using Create9 = IDirect3D9*(WINAPI*)(UINT);
    auto create = reinterpret_cast<Create9>(GetProcAddress(d3d9, "Direct3DCreate9"));
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    check(d3d != nullptr, "Direct3DCreate9");
    if (!d3d) return 1;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = static_cast<UINT>(g_w);
    pp.BackBufferHeight = static_cast<UINT>(g_h);
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    check(SUCCEEDED(hr) && dev, "CreateDevice (HAL, depth buffer)");
    if (!dev) return 1;
    for (int i = 0; i < 30; i++) {
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(20 + i, 30, 50), 1.0f, 0);
        dev->BeginScene();
        dev->EndScene();
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(15); // the menu fades in over ~0.2 s
    }
    auto frames = [&](int n) {
        for (int i = 0; i < n; i++) {
            dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(24, 30, 52), 1.0f, 0);
            dev->BeginScene();
            dev->EndScene();
            dev->Present(nullptr, nullptr, nullptr, nullptr);
        }
    };
    if (g_runtimeLoad) {
        // modules that were off at start: switched on while the "game" runs (host.json plugin.<id>.enabled 0)
        using SetFn = int(__cdecl*)(const char*, int);
        auto setEnabled = reinterpret_cast<SetFn>(GetProcAddress(host, "HorizonHost_TestSetPluginEnabled"));
        check(setEnabled != nullptr, "test export HorizonHost_TestSetPluginEnabled");
        if (setEnabled) {
            check(!has(hostLog(dir), "[sample] started"), "runtime load: the sample is off at start");
            check(setEnabled("sample", 1) == 1, "runtime load: switch the sample on");
            frames(3);
            std::string l = hostLog(dir);
            check(has(l, "[sample] started"), "runtime load: the sample ran Init while the game runs");
            check(has(l, "[sample] loaded while the game runs"), "runtime load: logged as loaded while running");
            check(has(l, "panel \"clock\" (Clock), in its own window"), "runtime load: its HUD panel appeared");
            for (int cycle = 0; cycle < 3; cycle++) { // off and on again: live toggle, no second Init
                setEnabled("sample", 0);
                frames(2);
                setEnabled("sample", 1);
                frames(2);
            }
            l = hostLog(dir);
            size_t inits = 0;
            for (size_t at = l.find("[sample] started"); at != std::string::npos; at = l.find("[sample] started", at + 1)) inits++;
            check(inits == 1, "runtime load: three off/on cycles, Init ran once");
            // a startup_only module (tests: abi12 with "startup_only": true) is not loaded, it asks for a restart
            setEnabled("abi12", 1);
            frames(2);
            l = hostLog(dir);
            check(has(l, "needs a restart: this module hooks the game at start"), "startup_only: switching on says needs a restart");
            check(!has(l, "[abi12] started"), "startup_only: not loaded while running");
        }
    }
    if (g_widgets) testWidgets(host, dev, pp, hwnd, dir);
    // Reset as the game does on a resolution change: the overlay must survive it
    const HRESULT rr = dev->Reset(&pp);
    check(SUCCEEDED(rr), "Reset through the hook");
    for (int i = 0; i < 10; i++) {
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(24, 30, 52), 1.0f, 0);
        dev->BeginScene();
        dev->EndScene();
        dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
    if (!shot.empty()) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        check(savePng(dev, shot.c_str()), "frame saved as PNG");
        // handshake for mods that are not plugins (TMVS): UI state, capture, version
        using U32Fn = uint32_t(__cdecl*)(void);
        using SetFn = void(__cdecl*)(int);
        auto uiState = reinterpret_cast<U32Fn>(GetProcAddress(host, "HorizonHost_GetUiState"));
        auto setCapture = reinterpret_cast<SetFn>(GetProcAddress(host, "HorizonHost_SetCaptureActive"));
        auto version = reinterpret_cast<U32Fn>(GetProcAddress(host, "HorizonHost_Version"));
        check(uiState && setCapture && version, "handshake exports present");
        if (uiState && setCapture && version) {
            check(version() == 0x000301u, "HorizonHost_Version = 0.3.1");
            check((uiState() & 1u) != 0, "UI state: menu open");
            if (expectPlugin == 1) check((uiState() & 4u) != 0, "UI state: the sample's Clock window is on screen");
            setCapture(1);
            dev->Present(nullptr, nullptr, nullptr, nullptr);
            check(uiState() == 0, "UI state 0 while capturing");
            setCapture(0);
            dev->Present(nullptr, nullptr, nullptr, nullptr);
            check((uiState() & 1u) != 0, "menu back after capture");
        }
    }
    if (g_crash) {
        // the plugins: crashmenu threw in DrawMenu (its page is open), crashoverlay in DrawOverlay
        using FaultFn = int(__cdecl*)(int);
        using U32Fn = uint32_t(__cdecl*)(void);
        auto fault = reinterpret_cast<FaultFn>(GetProcAddress(host, "HorizonHost_TestOverlayFault"));
        auto uiState = reinterpret_cast<U32Fn>(GetProcAddress(host, "HorizonHost_GetUiState"));
        check(fault && uiState, "crash: test export HorizonHost_TestOverlayFault");
        if (fault && uiState) {
            frames(5);
            std::string l = hostLog(dir);
            check(has(l, "[crashmenu] crashed in DrawMenu"), "crash: DrawMenu throw caught");
            check(has(l, "[crashoverlay] crashed in DrawOverlay"), "crash: DrawOverlay access violation caught");
            check(countOf(l, "stopped after an error") == 2, "crash: one toast per module");
            check(!has(l, "[sample] crashed"), "crash: the sample keeps running");
            check(!has(l, "error while drawing") && !has(l, "exception while drawing"), "crash: the host frame never failed");
            check((fault(0) & 9) == 1 && (uiState() & 1u) != 0, "crash: the menu stays open and usable");
            check((uiState() & 4u) != 0, "crash: the sample's Clock window is still drawn");
            // the host's own frame throws inside an open window: menu closed, input back, UI keeps going
            fault(1);
            frames(3);
            l = hostLog(dir);
            check(countOf(l, "overlay: error while drawing: menu closed, input back to the game") == 1, "host fault: logged once");
            check(has(l, "toast: Menu closed"), "host fault: toast");
            check((fault(0) & 15) == 0, "host fault: menu and layout closed, game input not held");
            fault(2);
            frames(3);
            check((fault(0) & 1) == 1 && (uiState() & 1u) != 0, "host fault: the menu opens again");
            check((uiState() & 4u) != 0, "host fault: widgets drawn again");
            // three host faults: the UI stays off, the menu no longer opens, nothing holds the input
            fault(1);
            frames(2);
            fault(1);
            frames(2);
            fault(2);
            frames(2);
            l = hostLog(dir);
            check(has(l, "Horizon UI off until the next start"), "host fault x3: UI off logged");
            check((fault(0) & 15) == 8, "host fault x3: UI off, menu closed, input not held");
            check(uiState() == 0, "host fault x3: UI state 0");
        }
    }
    log = hostLog(dir);
    check(has(log, "CreateDevice hooked in the factory table"), "factory CreateDevice slot hooked");
    check(has(log, "Present and Reset in its method table"), "device Present/Reset hooked");
    check(has(log, "overlay: ready"), "ImGui overlay ready on the device");
    check(has(log, "menu: open"), "menu drawn");
    check(has(log, "d3d: Reset -> 0x00000000"), "Reset seen by the host");
    check(!has(log, "exception while drawing"), "no exception while drawing");
    if (g_runtimeLoad) {
        check(!has(log, "crashed"), "runtime load: no plugin crashed (also after the Reset)");
        check(has(log, "d3d: Reset -> 0x00000000"), "runtime load: Reset after the late load");
    }
    if (expectPlugin == 1) {
        check(has(log, "test: save_replay -> Not supported"), "save_replay: NOT_SUPPORTED outside ESWC");
        check(has(log, "panel \"events\" (Events)"), "panel Events registered");
        check(has(log, "panel \"clock\" (Clock), in its own window"), "HUD panel Clock starts in its own window");
        if (!g_crash) check(!has(log, "crashed"), "no plugin crashed");
    }
    if (expectAbi12) {
        // tests/abi12: built against the 1.2 header, loaded by this 1.3 host
        check(has(log, "[abi12] started"), "ABI 1.2 plugin started");
        check(has(log, "1.2 layout ok"), "ABI 1.2 plugin: host table and host_flags as in 1.2");
        check(has(log, "abi12: pre_present called"), "ABI 1.2 plugin: its pre_present runs");
        check(!has(log, "abi12: FAIL"), "ABI 1.2 plugin: host reads only cb->size of its callbacks (no post_present)");
    }
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    return g_failed ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); // the process ends with TerminateProcess
    if (argc < 3) {
        printf("usage: host_test loader|host <dir> [safe] [expect-plugin|expect-no-plugin] [shot.png]\n");
        return 2;
    }
    const std::wstring mode = argv[1], dir = argv[2];
    if (mode == L"loader") return testLoader(dir);
    bool safe = false;
    int expect = -1;
    bool abi12 = false;
    std::wstring shot;
    for (int i = 3; i < argc; i++) {
        const std::wstring a = argv[i];
        if (a == L"safe") safe = true;
        else if (a == L"expect-plugin") expect = 1;
        else if (a == L"expect-no-plugin") expect = 0;
        else if (a == L"expect-abi12") abi12 = true;
        else if (a == L"runtime-load") g_runtimeLoad = true;
        else if (a == L"crash") g_crash = true;
        else if (a == L"widgets") g_widgets = true;
        else if (a == L"widgets-anchored") g_widgets = g_anchored = true;
        else if (a == L"layout") SetEnvironmentVariableW(L"HORIZON_TEST_LAYOUT", L"1"); // screens of layout mode
        else if (a.rfind(L"size=", 0) == 0) swscanf_s(a.c_str() + 5, L"%dx%d", &g_w, &g_h);
        else if (a.rfind(L"page=", 0) == 0) SetEnvironmentVariableW(L"HORIZON_TEST_PAGE", a.c_str() + 5);
        else shot = a;
    }
    const int r = testHost(dir, safe, expect, abi12, shot);
    printf("%s\n", r == 0 ? "host test: all passed" : "host test: FAILED");
    // exit without unloading: the host's threads keep running until the process ends, as in the game
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(r));
    return r;
}
