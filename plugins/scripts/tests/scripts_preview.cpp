// Renders Horizon Scripts inside the real Horizon menu shell (src/host/ui), offscreen with D3D9, and saves
// screens\scripts-<scene>.png (not committed): the Scripts tab with the example scripts, a script with
// errors, and the two example overlays over a stand-in game picture. No game needed.
//
//   scripts_preview.exe <scripts.dll> <examples folder>
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
#include "horizon_imgui.h"
#include "stub_host.h"
#include "test_host.h"

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

// A stand-in for the game picture: sky, grass, a road in perspective.
void gameBackground() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    const float horizon = d.y * 0.46f;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(d.x, horizon), IM_COL32(64, 118, 196, 255), IM_COL32(64, 118, 196, 255),
                                IM_COL32(176, 206, 232, 255), IM_COL32(176, 206, 232, 255));
    dl->AddRectFilledMultiColor(ImVec2(0, horizon), d, IM_COL32(70, 112, 52, 255), IM_COL32(70, 112, 52, 255), IM_COL32(40, 70, 30, 255),
                                IM_COL32(40, 70, 30, 255));
    const ImVec2 road[4] = {ImVec2(d.x * 0.47f, horizon), ImVec2(d.x * 0.53f, horizon), ImVec2(d.x * 0.92f, d.y), ImVec2(d.x * 0.08f, d.y)};
    dl->AddConvexPolyFilled(road, 4, IM_COL32(58, 60, 66, 255));
    for (int i = 0; i < 8; i++) {
        const float t0 = static_cast<float>(i) / 8.0f + 0.03f, t1 = t0 + 0.05f;
        auto at = [&](float t) { return horizon + (d.y - horizon) * t * t; };
        dl->AddRectFilled(ImVec2(d.x * 0.5f - 2 - 10 * t0, at(t0)), ImVec2(d.x * 0.5f + 2 + 10 * t1, at(t1)), IM_COL32(230, 230, 230, 255));
    }
    dl->AddRectFilled(ImVec2(d.x * 0.5f - 60, d.y * 0.70f), ImVec2(d.x * 0.5f + 60, d.y * 0.70f + 46), IM_COL32(200, 40, 30, 255), 8.0f);
}

void writeText(const std::string& path, const char* text) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") == 0 && f) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
}

void copyTree(const std::string& from, const std::string& to) {
    CreateDirectoryA(to.c_str(), nullptr);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((from + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        const std::string a = from + "\\" + fd.cFileName, b = to + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copyTree(a, b);
        else CopyFileA(a.c_str(), b.c_str(), FALSE);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

struct Scene {
    const char* name;
    const char* selected;
    bool menu;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: scripts_preview <scripts.dll> <examples folder>\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    char full[MAX_PATH];
    GetFullPathNameA(argv[2], MAX_PATH, full, nullptr);
    const std::string examples = full;

    // the stand-in game folder: both examples and two scripts with problems
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string root = exe;
    root.resize(root.find_last_of('\\') + 1);
    const std::string game = root + "preview-game\\";
    CreateDirectoryA(game.c_str(), nullptr);
    CreateDirectoryA((game + "horizon").c_str(), nullptr);
    copyTree(examples, game + "horizon\\scripts");
    const std::string broken = game + "horizon\\scripts\\split-demo\\";
    CreateDirectoryA(broken.c_str(), nullptr);
    writeText(broken + "info.toml", "name = \"Split Demo\"\nauthor = \"me\"\nversion = \"0.1.0\"\ndescription = \"A half-written script.\"\n"
                                    "online = \"solo-only\"\npermissions = [\"ui\", \"read-game\"]\n");
    writeText(broken + "main.as", "int best = -1;\n\nvoid OnFinish(int time) {\n    best = time;\n}\n\nvoid Render() {\n"
                                  "    UI::Begin(\"Split\");\n    UI::Text(\"Best \" + Time::FormatRace(bets));\n    UI::End();\n}\n");
    const std::string slow = game + "horizon\\scripts\\slow-demo\\";
    CreateDirectoryA(slow.c_str(), nullptr);
    writeText(slow + "info.toml", "name = \"Slow Demo\"\nversion = \"0.1.0\"\nonline = \"online-ok\"\npermissions = [\"ui\"]\n");
    writeText(slow + "main.as", "void Render() {\n    int n = 0;\n    while (n >= 0) {\n        n++;\n    }\n}\n");

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"HsPreview", nullptr};
    RegisterClassExW(&wc);
    RECT rc = {0, 0, kWidth, kHeight};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"scripts preview", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                              nullptr, wc.hInstance, nullptr);
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
    setIconFontFile("assets\\lucide-subset.ttf");
    loadFonts(1.0f);
    applyStyle(1.0f);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(dev);

    HMODULE mod = LoadLibraryA(argv[1]);
    if (!mod) {
        std::printf("LoadLibrary failed (%lu)\n", GetLastError());
        return 5;
    }
    auto init = reinterpret_cast<HorizonPlugin_InitFn>(GetProcAddress(mod, HORIZON_EXPORT_INIT));
    auto shutdown = reinterpret_cast<HorizonPlugin_ShutdownFn>(GetProcAddress(mod, HORIZON_EXPORT_SHUTDOWN));
    auto overlay = reinterpret_cast<HorizonPlugin_DrawOverlayFn>(GetProcAddress(mod, HORIZON_EXPORT_DRAW_OVERLAY));
    auto onEvent = reinterpret_cast<HorizonPlugin_OnEventFn>(GetProcAddress(mod, HORIZON_EXPORT_ON_EVENT));
    if (!init || !shutdown || !overlay || !onEvent) return 6;

    Model model;
    model.hostVersion = "0.2.0";
    model.gameName = "TrackMania Nations Forever";
    model.exeVersion = "fake engine graph";
    model.menuKey = "Insert";
    model.gameState = "Solo race";
    PluginView pv;
    pv.id = "scripts";
    pv.name = "Horizon Scripts";
    pv.version = "0.1.0";
    pv.author = "cheatoskar";
    pv.description = "Runs AngelScript scripts from horizon\\scripts on every supported game.";
    pv.status = PluginStatus::On;
    pv.enabled = true;
    pv.hasMenu = false;
    pv.liveToggle = true;
    PanelView scriptsTab;
    scriptsTab.key = "scripts";
    scriptsTab.title = "Scripts";
    scriptsTab.icon = HZ_ICON_SCROLL_TEXT;
    scriptsTab.label = HZ_ICON_SCROLL_TEXT_U8 " Scripts";
    PanelView optionsTab;
    optionsTab.key = "options";
    optionsTab.title = "Options";
    optionsTab.icon = HZ_ICON_SLIDERS_HORIZONTAL;
    optionsTab.label = HZ_ICON_SLIDERS_HORIZONTAL_U8 " Options";
    pv.panels = {scriptsTab, optionsTab};
    model.plugins.push_back(pv);
    PluginView ne;
    ne.id = "node-explorer";
    ne.name = "Node Explorer";
    ne.version = "0.1.0";
    ne.status = PluginStatus::On;
    ne.enabled = true;
    model.plugins.push_back(ne);
    Actions actions;
    actions.setPluginEnabled = [](const std::string&, bool) {};
    actions.closeMenu = [] {};
    actions.drawPanelTab = [](const std::string&, const std::string& key) {
        for (const testhost::Panel& p : testhost::panels)
            if (p.key == key) p.draw(p.user);
    };

    CreateDirectoryW(L"screens", nullptr);
    const Scene scenes[] = {
        {"tab", "map-info", true},
        {"errors", "split-demo", true},
        {"overlays", "clock", false},
    };
    int failed = 0;
    for (const Scene& sc : scenes) {
        testhost::setup(HORIZON_GAME_TMNF, game);
        stubhost::graph = fake::build(true);
        stubhost::style.font_body = fonts().body;
        stubhost::style.font_title = fonts().title;
        stubhost::style.font_mono = fonts().mono;
        stubhost::settings.clear();
        stubhost::settings["Selected"] = sc.selected;
        const HorizonHostApi host = testhost::api();
        if (init(&host) != 0) return 7;
        HorizonMapInfo mi = {};
        mi.size = sizeof(mi);
        mi.name = "A07-Race";
        mi.author = "Nadeo";
        mi.uid = "";
        mi.path = "";
        mi.environment = "Stadium";
        mi.author_time = 26830;
        mi.gold_time = 29000;
        mi.silver_time = 33000;
        mi.bronze_time = 41000;
        mi.author_score = -1;
        HorizonEvent e = {};
        e.size = sizeof(e);
        e.type = HORIZON_EVENT_MAP_LOADED;
        e.map = &mi;
        onEvent(&e);
        e.map = nullptr;
        e.type = HORIZON_EVENT_RACE_START;
        onEvent(&e);
        showPluginPage("scripts");
        bool menuOpen = true;
        const int frames = 30;
        for (int f = 0; f < frames; ++f) {
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            io.DeltaTime = 1.0f / 30.0f;
            io.AddMousePosEvent(5, 890);
            ImGui::NewFrame();
            gameBackground();
            overlay();
            if (sc.menu) drawMenu(model, actions, &menuOpen);
            ImGui::EndFrame();
            ImGui::Render();
            testhost::runQueued();
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
            if (SUCCEEDED(dev->BeginScene())) {
                ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
                dev->EndScene();
            }
            if (f == frames - 1) {
                const std::wstring path = L"screens\\scripts-" + std::wstring(sc.name, sc.name + std::strlen(sc.name)) + L".png";
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
    return failed;
}
