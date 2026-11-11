// Renders the Node Explorer page inside the real Horizon menu shell (src/host/ui) over the fake engine
// graph, offscreen with D3D9, and saves screens\node-explorer-<scene>.png. No game needed.
//
//   explorer_preview.exe <plugin dll>
#include <windows.h>

#include <d3d9.h>

#include <initguid.h>
#include <wincodec.h>

#ifdef small
#undef small // rpcndr.h: would break Fonts::small in ui.h
#endif
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "host/ui/ui.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fake_graph.h"
#include "stub_host.h"

using namespace horizon::ui;

namespace {

const int kWidth = 1600, kHeight = 900;

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

bool savePng(IDirect3DDevice9* dev, const wchar_t* path) {
    IDirect3DSurface9* back = nullptr;
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return false;
    D3DSURFACE_DESC desc;
    back->GetDesc(&desc);
    bool ok = false;
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(back, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            std::vector<BYTE> rgb(static_cast<size_t>(desc.Width) * desc.Height * 3);
            for (UINT y = 0; y < desc.Height; ++y) {
                const BYTE* src = static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(y) * static_cast<size_t>(lr.Pitch);
                BYTE* dst = &rgb[static_cast<size_t>(y) * desc.Width * 3];
                for (UINT x = 0; x < desc.Width; ++x) {
                    dst[x * 3 + 0] = src[x * 4 + 0];
                    dst[x * 3 + 1] = src[x * 4 + 1];
                    dst[x * 3 + 2] = src[x * 4 + 2];
                }
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
                    if (SUCCEEDED(frame->WritePixels(desc.Height, desc.Width * 3, static_cast<UINT>(rgb.size()), rgb.data())) &&
                        SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit()))
                        ok = true;
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

void gameBackground() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), d, IM_COL32(18, 28, 52, 255), IM_COL32(40, 22, 40, 255), IM_COL32(14, 16, 24, 255),
                                IM_COL32(10, 12, 18, 255));
}

struct Scene {
    const char* name;
    const char* tab;
    const char* path;
    const char* cls;
    ImVec2 mouse;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: explorer_preview <node-explorer.dll>\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"NxPreview", nullptr};
    RegisterClassExW(&wc);
    RECT rc = {0, 0, kWidth, kHeight};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"node explorer preview", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 2;
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return 3;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        std::printf("no HAL device (0x%08lx)\n", static_cast<unsigned long>(hr));
        return 4;
    }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    loadFonts(1.0f);
    applyStyle(1.0f);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(dev);

    stubhost::setup();
    stubhost::graph = fake::build(true);
    stubhost::style.font_body = fonts().body;
    stubhost::style.font_title = fonts().title;
    stubhost::style.font_mono = fonts().mono;

    HMODULE mod = LoadLibraryA(argv[1]);
    if (!mod) {
        std::printf("LoadLibrary failed (%lu)\n", GetLastError());
        return 5;
    }
    auto init = reinterpret_cast<HorizonPlugin_InitFn>(GetProcAddress(mod, HORIZON_EXPORT_INIT));
    auto shutdown = reinterpret_cast<HorizonPlugin_ShutdownFn>(GetProcAddress(mod, HORIZON_EXPORT_SHUTDOWN));
    auto drawPage = reinterpret_cast<HorizonPlugin_DrawMenuFn>(GetProcAddress(mod, HORIZON_EXPORT_DRAW_MENU));
    if (!init || !shutdown || !drawPage) return 6;
    const HorizonHostApi host = stubhost::api();

    Model model;
    model.hostVersion = "0.1.0";
    model.gameName = "TrackMania Nations Forever";
    model.exeVersion = "fake engine graph";
    model.menuKey = "Insert";
    model.gameState = "Solo race";
    PluginView pv;
    pv.id = "node-explorer";
    pv.name = "Node Explorer";
    pv.version = "0.1.0";
    pv.author = "cheatoskar";
    pv.description = "Browse the engine's classes and live objects. Read-only.";
    pv.status = PluginStatus::On;
    pv.enabled = true;
    pv.hasMenu = true;
    pv.liveToggle = true;
    model.plugins.push_back(pv);
    PluginView auto_ = pv;
    auto_.id = "eswc-autosave";
    auto_.name = "ESWC Autosave";
    auto_.status = PluginStatus::WrongGame;
    auto_.enabled = false;
    model.plugins.push_back(auto_);
    Actions actions;
    actions.drawPluginPage = [&](const std::string&) { drawPage(); };
    actions.setPluginEnabled = [](const std::string&, bool) {};
    actions.closeMenu = [] {};

    CreateDirectoryW(L"screens", nullptr);
    const Scene scenes[] = {
        {"objects", "0", "App.CurrentPlayground.GameTerminals[0].ControlledPlayer", "CTrackManiaPlayer", ImVec2(10, 10)},
        {"app", "0", "App", "CTrackManiaPlayer", ImVec2(10, 10)},
        {"classes", "1", "App", "CTrackManiaPlayer", ImVec2(10, 10)},
        {"settings", "2", "App", "CTrackManiaPlayer", ImVec2(10, 10)},
    };
    int failed = 0;
    for (const Scene& sc : scenes) {
        stubhost::settings["Tab"] = sc.tab;
        stubhost::settings["Path"] = sc.path;
        stubhost::settings["Class"] = sc.cls;
        stubhost::settings["Pins"] = "Race\nChallenge.Blocks[2]\nApp.CurrentPlayground.GameTerminals[0].ControlledPlayer.Mobil";
        if (init(&host) != 0) return 7;
        showPluginPage("node-explorer");
        bool menuOpen = true;
        const int frames = 30;
        for (int f = 0; f < frames; ++f) {
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            io.DeltaTime = 1.0f / 30.0f;
            io.AddMousePosEvent(sc.mouse.x, sc.mouse.y);
            ImGui::NewFrame();
            gameBackground();
            drawMenu(model, actions, &menuOpen);
            ImGui::EndFrame();
            ImGui::Render();
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
            if (SUCCEEDED(dev->BeginScene())) {
                ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
                dev->EndScene();
            }
            if (f == frames - 1) {
                const std::wstring path = L"screens\\node-explorer-" + std::wstring(sc.name, sc.name + std::strlen(sc.name)) + L".png";
                if (savePng(dev, path.c_str())) {
                    std::printf("saved %ls\n", path.c_str());
                } else {
                    std::printf("FAILED %ls\n", path.c_str());
                    ++failed;
                }
            }
            dev->Present(nullptr, nullptr, nullptr, nullptr);
        }
        shutdown();
    }
    FreeLibrary(mod);
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::printf("guard page %s\n", fake::guardIntact() ? "intact" : "TOUCHED");
    return failed;
}
