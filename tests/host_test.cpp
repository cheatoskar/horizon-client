// Integration test without a game: loads the real loader/host DLLs in this process.
//
//   host_test loader <dir>        dir\dinput8.dll (Legacy Loader) + dir\dinput8.chain.dll (test chain) +
//                                 dir\horizon\host.dll: DirectInput8Create must work, go through the chain,
//                                 and the host must stay OFF (this exe is not a known game) and say so.
//   host_test host <dir> [safe] [expect-plugin|expect-no-plugin]
//                                 HorizonHostInit in test mode, then a D3D9 device on a hidden window, frames
//                                 with the menu open (HORIZON_TEST_MENU=1), a PNG of the frame into screens\.
// Exit code 0 = all checks passed. The host log of the run is checked for the expected lines.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <unknwn.h>
#include <d3d9.h>
#include <dinput.h>
#include <wincodec.h>

#include <cstdio>
#include <string>
#include <vector>

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

static int testHost(const std::wstring& dir, bool safe, int expectPlugin, const std::wstring& shot) {
    struct Info {
        unsigned api, size, flags;
        const wchar_t* path;
    };
    SetEnvironmentVariableW(L"HORIZON_TEST_MENU", L"1");
    HMODULE host = LoadLibraryW((dir + L"\\horizon\\host.dll").c_str());
    check(host != nullptr, "host.dll loads");
    if (!host) return 1;
    using InitFn = int(__cdecl*)(const Info*);
    auto init = reinterpret_cast<InitFn>(GetProcAddress(host, "HorizonHostInit"));
    Info info = {1, sizeof(Info), (safe ? 1u : 0u) | 2u, L""};
    check(init && init(&info) == 1, "HorizonHostInit (test mode) starts the host");
    std::string log = hostLog(dir);
    check(has(log, "Horizon Host"), "log file written in horizon\\logs");
    if (safe) check(has(log, "safe mode"), "safe mode logged");
    if (expectPlugin == 1) check(has(log, "[sample] started"), "the sample plugin started");
    if (expectPlugin == 0) check(!has(log, "[sample] started"), "the sample plugin did not start");

    // a D3D9 device on a hidden window, created the way the game does it
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HorizonHostTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Horizon host test", WS_OVERLAPPEDWINDOW, 0, 0, 1600, 900, nullptr, nullptr, wc.hInstance, nullptr);
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
    pp.BackBufferWidth = 1600;
    pp.BackBufferHeight = 900;
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
    }
    log = hostLog(dir);
    check(has(log, "CreateDevice hooked in the factory table"), "factory CreateDevice slot hooked");
    check(has(log, "Present and Reset in its method table"), "device Present/Reset hooked");
    check(has(log, "overlay: ready"), "ImGui overlay ready on the device");
    check(has(log, "menu: open"), "menu drawn");
    check(has(log, "d3d: Reset -> 0x00000000"), "Reset seen by the host");
    check(!has(log, "exception while drawing"), "no exception while drawing");
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
    std::wstring shot;
    for (int i = 3; i < argc; i++) {
        const std::wstring a = argv[i];
        if (a == L"safe") safe = true;
        else if (a == L"expect-plugin") expect = 1;
        else if (a == L"expect-no-plugin") expect = 0;
        else shot = a;
    }
    const int r = testHost(dir, safe, expect, shot);
    printf("%s\n", r == 0 ? "host test: all passed" : "host test: FAILED");
    // exit without unloading: the host's threads keep running until the process ends, as in the game
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(r));
    return r;
}
