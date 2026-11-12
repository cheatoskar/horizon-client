// Renders the client mods (WR Hunter, Input Display, TMX Browser) and the toasts offscreen with D3D9, inside
// the real Horizon menu shell (src/host/ui) and as HUD windows over a stand-in game picture, with the client
// test host (fake world, fake TM Horizon bridge with TMX fixtures). Saves screens\client-<scene>.png
// (screens\ is not committed). No game needed.
//
// Also the modules in their own window (top bar, left click on the icon) with the top bar shown, at 640x480 and
// 1920x1080: screens\client-popout-<tag>-<module>-<size>.png (<tag> = second argument, default "now").
//
//   client_preview.exe <build\plugins folder> [tag]
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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "client_host.h"
#include "../../tmx-browser/tests/fake_tmx.h"

using namespace horizon::ui;

namespace {

const int kWidth = 1600, kHeight = 900;
IDirect3DDevice9* g_dev = nullptr;
D3DPRESENT_PARAMETERS g_pp = {};
int g_w = kWidth, g_h = kHeight; // the back buffer (the game window) of the current scene
std::string g_tag = "now";
int g_failed = 0;

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

// Real D3D9 textures for the plugins' thumbnails (the test host only hands out numbers).
uint64_t HORIZON_CALL createTexture(HorizonPluginHandle, uint32_t w, uint32_t h, const void* rgba) {
    if (!w || !h || !rgba) return 0;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) return 0;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
        const BYTE* src = static_cast<const BYTE*>(rgba);
        for (uint32_t y = 0; y < h; y++) {
            BYTE* dst = static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(y) * static_cast<size_t>(lr.Pitch);
            for (uint32_t x = 0; x < w; x++) {
                const BYTE* p = src + (static_cast<size_t>(y) * w + x) * 4;
                dst[x * 4 + 0] = p[2];
                dst[x * 4 + 1] = p[1];
                dst[x * 4 + 2] = p[0];
                dst[x * 4 + 3] = p[3];
            }
        }
        tex->UnlockRect(0);
    }
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tex));
}
void HORIZON_CALL releaseTexture(HorizonPluginHandle, uint64_t t) {
    if (t) reinterpret_cast<IDirect3DTexture9*>(static_cast<uintptr_t>(t))->Release();
}

// A stand-in for the game: sky, stadium stands, a road to the horizon.
void gameBackground(bool night) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    const float hz = d.y * 0.46f;
    if (night) dl->AddRectFilledMultiColor(ImVec2(0, 0), d, IM_COL32(18, 28, 52, 255), IM_COL32(40, 22, 40, 255), IM_COL32(14, 16, 24, 255), IM_COL32(10, 12, 18, 255));
    else {
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(d.x, hz), IM_COL32(86, 140, 200, 255), IM_COL32(86, 140, 200, 255), IM_COL32(170, 200, 228, 255),
                                    IM_COL32(170, 200, 228, 255));
        dl->AddRectFilled(ImVec2(0, hz - 40), ImVec2(d.x, hz), IM_COL32(120, 128, 140, 255));
        for (int i = 0; i < 40; i++) dl->AddRectFilled(ImVec2(i * 42.0f, hz - 38), ImVec2(i * 42.0f + 30, hz - 6), IM_COL32(150 + (i * 37) % 60, 80, 70, 255));
        dl->AddRectFilledMultiColor(ImVec2(0, hz), d, IM_COL32(70, 120, 60, 255), IM_COL32(70, 120, 60, 255), IM_COL32(40, 80, 36, 255), IM_COL32(40, 80, 36, 255));
        const ImVec2 road[4] = {ImVec2(d.x * 0.47f, hz), ImVec2(d.x * 0.53f, hz), ImVec2(d.x * 0.95f, d.y), ImVec2(d.x * 0.05f, d.y)};
        dl->AddConvexPolyFilled(road, 4, IM_COL32(64, 66, 72, 255));
        for (int i = 0; i < 8; i++) {
            const float t0 = 0.05f + i * 0.12f, t1 = t0 + 0.05f;
            const float y0 = hz + (d.y - hz) * t0 * t0, y1 = hz + (d.y - hz) * t1 * t1;
            const float w0 = 2 + 10 * t0, w1 = 2 + 10 * t1;
            const ImVec2 m[4] = {ImVec2(d.x * 0.5f - w0, y0), ImVec2(d.x * 0.5f + w0, y0), ImVec2(d.x * 0.5f + w1, y1), ImVec2(d.x * 0.5f - w1, y1)};
            dl->AddConvexPolyFilled(m, 4, IM_COL32(230, 230, 230, 255));
        }
    }
}

// One shot: `frames` frames, the last one saved as screens\client-<name>.png.
void shot(const char* name, int frames, const std::function<void(int)>& draw, bool night = true) {
    ImGuiIO& io = ImGui::GetIO();
    for (int f = 0; f < frames; ++f) {
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        io.DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));
        io.DeltaTime = 1.0f / 30.0f;
        io.AddMousePosEvent(-100, -100);
        ImGui::NewFrame();
        gameBackground(night);
        draw(f);
        ImGui::EndFrame();
        chost::endFrame();
        ImGui::Render();
        g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        if (SUCCEEDED(g_dev->BeginScene())) {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_dev->EndScene();
        }
        if (f == frames - 1) {
            const std::wstring path = L"screens\\client-" + std::wstring(name, name + std::strlen(name)) + L".png";
            if (savePng(g_dev, path.c_str())) std::printf("saved %ls\n", path.c_str());
            else {
                std::printf("FAILED %ls\n", path.c_str());
                ++g_failed;
            }
        }
        g_dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
}

// A frame without drawing (game time passes for the plugin).
void tick(const std::function<void()>& fn) {
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));
    ImGui::NewFrame();
    fn();
    ImGui::EndFrame();
    chost::endFrame();
}

// A HUD panel the way the host draws it with the menu closed.
void hudWindow(const char* id, ImVec2 pos, const std::function<void()>& body) {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w * 0.9f);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    body();
    ImGui::End();
}

const chost::Panel* panel(const char* key) {
    for (const chost::Panel& p : chost::panels)
        if (p.key == key) return &p;
    return nullptr;
}
void drawPanel(const char* key) {
    if (const chost::Panel* p = panel(key)) p->draw(p->user);
}

struct Plugin {
    HMODULE mod = nullptr;
    HorizonPlugin_InitFn init = nullptr;
    HorizonPlugin_ShutdownFn shutdown = nullptr;
    HorizonPlugin_OnEventFn onEvent = nullptr;
    HorizonPlugin_DrawOverlayFn overlay = nullptr;
    HorizonPlugin_DrawMenuFn menu = nullptr;
    int32_t(__cdecl* hook)(const char*, char*, int32_t) = nullptr;
    void cmd(const char* c) const { hook(c, nullptr, 0); }
    void send(HorizonEvent e) const {
        if (onEvent) onEvent(&e);
    }
};

Plugin load(const std::string& dir, const char* id) {
    Plugin p;
    const std::string path = dir + "\\" + id + "\\" + id + ".dll";
    p.mod = LoadLibraryA(path.c_str());
    if (!p.mod) {
        std::printf("FAILED LoadLibrary %s (%lu)\n", path.c_str(), GetLastError());
        ++g_failed;
        return p;
    }
    p.init = reinterpret_cast<HorizonPlugin_InitFn>(GetProcAddress(p.mod, HORIZON_EXPORT_INIT));
    p.shutdown = reinterpret_cast<HorizonPlugin_ShutdownFn>(GetProcAddress(p.mod, HORIZON_EXPORT_SHUTDOWN));
    p.onEvent = reinterpret_cast<HorizonPlugin_OnEventFn>(GetProcAddress(p.mod, HORIZON_EXPORT_ON_EVENT));
    p.overlay = reinterpret_cast<HorizonPlugin_DrawOverlayFn>(GetProcAddress(p.mod, HORIZON_EXPORT_DRAW_OVERLAY));
    p.menu = reinterpret_cast<HorizonPlugin_DrawMenuFn>(GetProcAddress(p.mod, HORIZON_EXPORT_DRAW_MENU));
    p.hook = reinterpret_cast<int32_t(__cdecl*)(const char*, char*, int32_t)>(GetProcAddress(p.mod, "HorizonClient_TestHook"));
    return p;
}

std::string dataRoot() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string root = exe;
    root.resize(root.find_last_of('\\') + 1);
    return root + "preview-data\\";
}

void startHost(uint32_t game, const char* pluginId) {
    chost::setup(game, dataRoot(), pluginId);
    chost::style.font_body = fonts().body;
    chost::style.font_title = fonts().title;
    chost::style.font_mono = fonts().mono;
}

HorizonHostApi hostApi() {
    HorizonHostApi a = chost::api();
    a.create_texture_rgba = &createTexture;
    a.release_texture = &releaseTexture;
    return a;
}

Model baseModel(const char* gameName) {
    Model m;
    m.hostVersion = "0.2.0";
    m.gameName = gameName;
    m.exeVersion = "2.11.26";
    m.menuKey = "Insert";
    m.gameState = "Main menu";
    m.bridgeText = "Connected to TM Horizon";
    m.bridgePaired = true;
    return m;
}

PluginView view(const char* id, const char* name, const char* desc) {
    PluginView pv;
    pv.id = id;
    pv.name = name;
    pv.version = "0.1.0";
    pv.author = "cheatoskar";
    pv.description = desc;
    pv.status = PluginStatus::On;
    pv.enabled = true;
    pv.liveToggle = true;
    return pv;
}

PanelView tab(const char* key, const char* title, bool detached = false) {
    PanelView v;
    v.key = key;
    v.title = title;
    v.label = title;
    v.detached = detached;
    return v;
}

// The host's ui_metrics (ABI 1.4) as the menu shell knows them right now, handed to the plugin.
void syncMetrics() {
    const MenuMetrics& mm = menuMetrics();
    chost::metrics.scale = scale();
    chost::metrics.display_w = static_cast<float>(g_w);
    chost::metrics.display_h = static_cast<float>(g_h);
    chost::metrics.menu_w = mm.menuW;
    chost::metrics.menu_h = mm.menuH;
    chost::metrics.page_w = mm.pageW;
    chost::metrics.page_h = mm.pageH;
}

// A new back buffer size (the game window) and the UI scale the host would pick for it.
void setDisplay(int w, int h, float uiScale) {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    g_pp.BackBufferWidth = static_cast<UINT>(w);
    g_pp.BackBufferHeight = static_cast<UINT>(h);
    if (FAILED(g_dev->Reset(&g_pp))) {
        std::printf("FAILED Reset to %dx%d\n", w, h);
        ++g_failed;
    }
    g_w = w;
    g_h = h;
    reloadFonts(uiScale);
    applyStyle(uiScale);
    ImGui_ImplDX9_CreateDeviceObjects();
    chost::style.scale = uiScale;
    chost::style.font_body = fonts().body;
    chost::style.font_title = fonts().title;
    chost::style.font_mono = fonts().mono;
}

// The module in its own window (left click on its bar icon) with the top bar shown, at 640x480 (UI scale 0.8,
// as the host fits it) and 1920x1080 (1.0). A fresh window per size (id suffix), so it opens at its default size.
void menuShot(const char* name, const Model& model, const Actions& actions, const char* page, int frames, const std::function<void(int)>& before);

void popoutShots(const char* name, PluginView pv, const Actions& a, const char* gameName) {
    struct Size {
        int w, h;
        float s;
        const char* tag;
    } sizes[] = {{640, 480, 0.8f, "640x480"}, {1920, 1080, 1.0f, "1920x1080"}};
    const std::string baseId = pv.id;
    for (const Size& z : sizes) {
        setDisplay(z.w, z.h, z.s);
        Model m = baseModel(gameName);
        pv.id = baseId + "@" + z.tag;
        m.plugins.clear();
        m.plugins.push_back(pv);
        for (const char* other : {"input-display", "tmx-browser", "tmx-upload", "performance", "scripts"}) {
            if (baseId.find(other) == 0) continue;
            PluginView o = view(other, other, "");
            m.plugins.push_back(o);
        }
        togglePluginWindow(pv.id);
        const std::string shotName = "popout-" + g_tag + "-" + name + "-" + z.tag;
        shot(shotName.c_str(), 14, [&](int) {
            bool openMenu = false;
            drawBar(m, a, true, false, &openMenu);
            drawPluginWindows(m, a);
        }, false);
        togglePluginWindow(pv.id);
    }
    // 640x480: the window next to the open menu (its header must stay reachable), and the window made narrow
    // by the player (330 px: the compact layout; the window keeps its minimum, content scrolls)
    setDisplay(640, 480, 0.8f);
    for (int scene = 0; scene < 2; ++scene) {
        Model m = baseModel(gameName);
        PluginView q = pv;
        q.id = baseId + (scene == 0 ? "@menu" : "@narrow");
        if (scene == 1) {
            q.winX = 8;
            q.winY = 60;
            q.winW = 330;
            q.winH = 400;
        }
        m.plugins.push_back(q);
        togglePluginWindow(q.id);
        const std::string shotName = "popout-" + g_tag + "-" + name + (scene == 0 ? "-640x480-menu" : "-640x480-narrow");
        menuShot(shotName.c_str(), m, a, scene == 0 ? "general" : "", 14, [&](int) {});
        togglePluginWindow(q.id);
    }
    setDisplay(kWidth, kHeight, 1.0f);
}

Actions actionsFor(const Plugin& p) {
    Actions a;
    a.drawPluginPage = [&p](const std::string&) {
        syncMetrics();
        if (p.menu) p.menu();
    };
    a.drawPanelTab = [](const std::string&, const std::string& key) {
        syncMetrics();
        drawPanel(key.c_str());
    };
    a.setPluginEnabled = [](const std::string&, bool) {};
    a.closeMenu = [] {};
    a.setDnd = [](bool) {};
    a.setHoldToasts = [](bool) {};
    a.setBridgeEnabled = [](bool) {};
    a.forgetBridge = [] {};
    return a;
}

void menuShot(const char* name, const Model& model, const Actions& actions, const char* page, int frames, const std::function<void(int)>& before) {
    frames += 30; // the menu fades in
    shot(name, frames, [&](int f) {
        if (before) before(f);
        if (!*page) { // no menu: the free plugin windows only
            drawPluginWindows(model, actions);
            return;
        }
        if (std::strncmp(page, "plugin:", 7) == 0) showPluginPage(page + 7);
        else showPage(page);
        bool open = true;
        drawMenu(model, actions, &open);
        drawPluginWindows(model, actions); // free plugin windows sit above the menu, as in the host
    });
}

// ---------------------------------------------------------------- WR Hunter

void wrHunter(const std::string& dir) {
    Plugin p = load(dir, "wr-hunter");
    if (!p.mod || !p.hook) return;
    startHost(HORIZON_GAME_TMNF, "wr-hunter");
    // a fresh data folder each run
    const std::string maps = dataRoot() + "wr-hunter\\maps\\";
    DeleteFileA((maps + "UidOfTheTestMap000000000aB.json").c_str());
    chost::buildWorld(true, "UidOfTheTestMap000000000aB", L"$o$fffSpeedway Echo");
    chost::settings["Target"] = "1";
    const HorizonHostApi api = hostApi();
    if (p.init(&api) != 0) {
        ++g_failed;
        return;
    }
    HorizonMapInfo mi = chost::mapInfo("Speedway Echo", "", 24000);
    HorizonEvent e = chost::event(HORIZON_EVENT_MAP_LOADED);
    e.map = &mi;
    p.send(e);
    for (int i = 0; i < 3; i++) tick([&] { p.overlay(); });
    auto drive = [&](const std::vector<int>& cps, size_t upTo, bool finish) {
        chost::gameState = HORIZON_STATE_SOLO_RACE;
        chost::setRun(0, 0, {});
        p.send(chost::event(HORIZON_EVENT_RACE_START));
        chost::setRun(1, 10, {});
        tick([&] { p.overlay(); });
        std::vector<int> passed;
        for (size_t i = 0; i < upTo; i++) {
            passed.push_back(cps[i]);
            const bool last = finish && i + 1 == cps.size();
            chost::setRun(last ? 2 : 1, cps[i] + (last ? 0 : 40), passed);
            tick([&] { p.overlay(); });
            if (last) {
                HorizonEvent f = chost::event(HORIZON_EVENT_FINISH);
                f.time_ms = cps[i];
                f.is_local_player = 1;
                p.send(f);
            }
        }
    };
    drive({5300, 10100, 15400, 20500, 25200}, 3, false);
    drive({5300, 10100, 15400, 20500, 25200}, 5, true);
    drive({5000, 10300, 15100, 20000, 24900}, 5, true);
    drive({5200, 10200, 15300, 20300, 25050}, 5, true);
    drive({4950, 10150, 15000, 20100, 24990}, 3, false); // running now: three splits
    chost::setRun(1, 17320, {4950, 10150, 15000});
    chost::toasts.clear();

    for (int layout = 0; layout < 2; layout++) {
        char c[16];
        snprintf(c, sizeof(c), "layout %d", layout);
        p.cmd(c);
        shot(layout ? "wr-hunter-detailed" : "wr-hunter-compact", 6, [&](int) {
            p.overlay();
            hudWindow("##splits", ImVec2(kWidth * 0.70f, kHeight * 0.22f), [] { drawPanel("splits"); });
        }, false);
    }
    p.cmd("layout 0");

    Model m = baseModel("TrackMania Nations Forever");
    m.gameState = "Solo race";
    PluginView pv = view("wr-hunter", "WR Hunter", "Splits per checkpoint against your PB and the TMX record.");
    pv.hasMenu = true;
    pv.panels = {tab("splits", "Splits", true), tab("history", "History")};
    m.plugins.push_back(pv);
    Actions a = actionsFor(p);
    menuShot("wr-hunter-settings", m, a, "plugin:wr-hunter", 8, nullptr);
    m.plugins[0].hasMenu = false;
    m.plugins[0].id = "wr-hunter.history"; // a fresh tab bar that opens on History
    m.plugins[0].panels = {tab("history", "History"), tab("splits", "Splits", true)};
    menuShot("wr-hunter-history", m, a, "plugin:wr-hunter.history", 8, nullptr);
    PluginView pw = view("wr-hunter", "WR Hunter", "Splits against your PB and the TMX record.");
    pw.hasMenu = true;
    pw.panels = {tab("splits", "Splits", true), tab("history", "History")};
    popoutShots("wr-hunter-settings", pw, a, "TrackMania Nations Forever");
    pw.id = "wr-hunter.history";
    pw.hasMenu = false;
    pw.panels = {tab("history", "History"), tab("splits", "Splits", true)};
    popoutShots("wr-hunter-history", pw, a, "TrackMania Nations Forever");
    p.shutdown();
}

// ---------------------------------------------------------------- Input Display

void inputDisplay(const std::string& dir) {
    Plugin p = load(dir, "input-display");
    if (!p.mod || !p.hook) return;
    startHost(HORIZON_GAME_TMUF, "input-display");
    chost::buildWorld(true, "UidOfTheTestMap000000000aB", L"Test");
    chost::settings["Gear"] = "1";
    chost::settings["Rpm"] = "1";
    const HorizonHostApi api = hostApi();
    if (p.init(&api) != 0) {
        ++g_failed;
        return;
    }
    chost::gameState = HORIZON_STATE_SOLO_RACE;
    chost::setRun(1, 12000, {});
    const char* names[4] = {"Bars", "Keys", "Wheel", "Minimal"};
    const ImVec2 at[4] = {ImVec2(60, 520), ImVec2(470, 560), ImVec2(820, 520), ImVec2(1220, 640)};
    shot("input-display", 200, [&](int f) {
        const float t = f / 30.0f;
        const float steer = std::sin(t * 1.7f) * (f > 150 ? 0.62f : 1.0f);
        const bool braking = f > 185;
        chost::setCar(steer, braking ? 0.0f : 1.0f, braking ? 1.0f : 0.0f, 180 + static_cast<int>(40 * std::sin(t * 0.6f)), 5, 7900.0f + 600.0f * std::sin(t));
        p.overlay();
        for (int s = 0; s < 4; s++) {
            char c[16];
            snprintf(c, sizeof(c), "style %d", s);
            p.cmd(c);
            hudWindow(names[s], at[s], [] { drawPanel("inputs"); });
            ImGui::GetForegroundDrawList()->AddText(ImVec2(at[s].x, at[s].y - 22), IM_COL32(255, 255, 255, 200), names[s]);
        }
    }, false);
    p.cmd("style 0");
    {
        PluginView pv = view("input-display", "Input Display", "Steering, gas, brake and speed on screen.");
        pv.hasMenu = true;
        pv.panels = {tab("inputs", "Inputs", true)};
        Actions a = actionsFor(p);
        popoutShots("input-display", pv, a, "TrackMania United Forever");
    }
    p.shutdown();
    // ESWC: no speed field in the old engine
    startHost(HORIZON_GAME_ESWC, "input-display");
    chost::buildWorld(false, "UidOfTheTestMap000000000aB", L"Test");
    if (p.init(&api) != 0) {
        ++g_failed;
        return;
    }
    chost::gameState = HORIZON_STATE_SOLO_RACE;
    chost::setRun(1, 12000, {});
    shot("input-display-eswc", 60, [&](int f) {
        chost::setCar(-0.4f + 0.01f * f, 1.0f, 0.0f, 0, 0, 0.0f);
        p.overlay();
        hudWindow("eswc", ImVec2(60, 560), [] { drawPanel("inputs"); });
    }, false);
    p.shutdown();
}

// ---------------------------------------------------------------- TMX Browser

void tmxBrowser(const std::string& dir) {
    Plugin p = load(dir, "tmx-browser");
    if (!p.mod || !p.hook) return;
    // TMX Browser talks to TMX itself: a fake exchange instead of the network
    if (auto net = reinterpret_cast<faketmx::TestNetFn>(GetProcAddress(p.mod, "HorizonClient_TestNet"))) net(&faketmx::fetch, &faketmx::link);
    startHost(HORIZON_GAME_TMNF, "tmx-browser");
    DeleteFileA((dataRoot() + "tmx-browser\\favourites.json").c_str());
    chost::bridgeStateValue = HORIZON_BRIDGE_CONNECTED;
    chost::gameState = HORIZON_STATE_MAIN_MENU;
    const HorizonHostApi api = hostApi();
    if (p.init(&api) != 0) {
        ++g_failed;
        return;
    }
    Model m = baseModel("TrackMania Nations Forever");
    PluginView pv = view("tmx-browser", "TMX Browser", "Search TMX and play maps in the game.");
    pv.panels = {tab("browse", "Browse"), tab("favourites", "Favourites")};
    m.plugins.push_back(pv);
    Actions a = actionsFor(p);
    p.cmd("search");
    p.cmd("pump");
    menuShot("tmx-browser-grid", m, a, "plugin:tmx-browser", 24, nullptr);
    popoutShots("tmx-browser-grid", pv, a, "TrackMania Nations Forever");
    p.cmd("select 2");
    menuShot("tmx-browser-detail", m, a, "plugin:tmx-browser", 6, nullptr);
    popoutShots("tmx-browser-detail", pv, a, "TrackMania Nations Forever");
    p.cmd("back");
    p.cmd("fav 1");
    p.cmd("fav 4");
    p.cmd("fav 7");
    p.cmd("tab favourites");
    m.plugins[0].id = "tmx-browser.favourites"; // a fresh tab bar that opens on Favourites
    m.plugins[0].panels = {tab("favourites", "Favourites"), tab("browse", "Browse")};
    menuShot("tmx-browser-favourites", m, a, "plugin:tmx-browser.favourites", 10, nullptr);
    m.plugins[0].id = "tmx-browser";
    m.plugins[0].panels = {tab("browse", "Browse"), tab("favourites", "Favourites")};
    p.cmd("tab browse");
    p.cmd("select 1");
    chost::online = true;
    m.online = true;
    m.gameState = "Online race";
    menuShot("tmx-browser-online", m, a, "plugin:tmx-browser", 6, nullptr);
    chost::online = false;
    p.shutdown();
}

// ---------------------------------------------------------------- toasts and the General page

void toasts() {
    startHost(HORIZON_GAME_TMNF, "none");
    Model m = baseModel("TrackMania Nations Forever");
    m.holdToasts = true;
    m.recentToasts = {{"WR Hunter", "New personal best", "Speedway Echo  0:24.90", HORIZON_TOAST_OK, false},
                      {"TM Horizon", "Replay uploaded to TMX", "Speedway Echo", HORIZON_TOAST_OK, true},
                      {"TM Horizon", "Mod update available", "tmx-browser 0.1.1", HORIZON_TOAST_INFO, false}};
    Actions a;
    a.setDnd = [](bool) {};
    a.setHoldToasts = [](bool) {};
    a.setBridgeEnabled = [](bool) {};
    a.forgetBridge = [] {};
    a.closeMenu = [] {};
    std::vector<ToastView> tv;
    ToastView t1;
    t1.owner = "WR Hunter";
    t1.title = "New personal best";
    t1.text = "Speedway Echo  0:24.90 (-0.30)";
    t1.kind = HORIZON_TOAST_OK;
    ToastView t2;
    t2.owner = "TM Horizon";
    t2.title = "Replay uploaded to TMX";
    t2.text = "Speedway Echo";
    t2.kind = HORIZON_TOAST_OK;
    ToastView t3;
    t3.owner = "TMX Browser";
    t3.title = "Playing is off while online";
    t3.text = "Leave the server to start a map.";
    t3.kind = HORIZON_TOAST_WARN;
    tv = {t1, t2, t3};
    shot("toasts", 4, [&](int) { drawToasts(tv); }, false);
    menuShot("general-notifications", m, a, "general", 6, nullptr);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: client_preview <build\\plugins>\n");
        return 2;
    }
    const std::string dir = argv[1];
    if (argc > 2) g_tag = argv[2];
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"CkPreview", nullptr};
    RegisterClassExW(&wc);
    RECT rc = {0, 0, kWidth, kHeight};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"client mods preview", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                              nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 2;
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return 3;
    D3DPRESENT_PARAMETERS& pp = g_pp;
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev);
    if (FAILED(hr)) hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev);
    if (FAILED(hr)) {
        std::printf("no HAL device (0x%08lx)\n", static_cast<unsigned long>(hr));
        return 4;
    }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    setIconFontFile("assets/lucide-subset.ttf");
    loadFonts(1.0f);
    applyStyle(1.0f);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_dev);
    CreateDirectoryW(L"screens", nullptr);

    wrHunter(dir);
    inputDisplay(dir);
    tmxBrowser(dir);
    toasts();

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::printf("client_preview: %s\n", g_failed ? "FAILED" : "all saved");
    return g_failed;
}
