// Renders the Horizon menu shell offscreen (hidden window, D3D9 HAL) and saves screens\ui-<scene>.png.
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
#include <string>
#include <vector>

using namespace horizon::ui;

namespace {

const int kWidth = 1600, kHeight = 900;

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

struct Preview {
    Model model;
    Actions actions;
    int captureHandle = 0;
    float sliderValue = 0.6f;
    bool checkA = true, checkB = false;
    int combo = 1;
};
Preview P;

void fillModel() {
    P.model = Model();
    Model& m = P.model;
    m.hostVersion = "0.1.0";
    m.gameName = "TrackMania Nations ESWC";
    m.exeVersion = "0.1.7.9 (unofficial 1.80)";
    m.profileName = "tmnf-solo";
    m.menuKey = "Insert";
    m.logPath = "C:\\Users\\player\\Documents\\TrackMania\\horizon\\logs\\2026-11-11_21-04-03.log";
    m.gameState = "Main menu";
    m.online = false;
    m.notes = {"Changes apply after a restart: TM Vibrant Shaders"};
    m.menuHotkey = 1;
    m.barEnabled = true;
    m.onlineFix = 1;
    m.menuKeySuggestions = {"Insert", "F2", "#", "+", "Ctrl+Tab"};
    m.otherMods = {
        {"TM Vibrant Shaders", "1.5.0", "d3d9.dll", "d3d9 proxy", "F5-F8, F12 (menu F8)", "Keeps its own Direct3D hook."},
        {"CenterSpeed", "", "CenterSpeed.dll", "TMLoader mod", "", ""},
        {"dinput8.chain.dll", "", "dinput8.chain.dll", "dinput8 chain", "", "ESWC Autosave loader 1.2"},
    };
    auto add = [&](const char* id, const char* name, const char* ver, const char* author, const char* desc,
                   PluginStatus st, bool enabled, bool menu, bool live, const char* text) {
        PluginView p;
        p.id = id;
        p.name = name;
        p.version = ver;
        p.author = author;
        p.description = desc;
        p.status = st;
        p.enabled = enabled;
        p.hasMenu = menu;
        p.liveToggle = live;
        p.statusText = text;
        m.plugins.push_back(p);
    };
    add("eswc-autosave", "ESWC Autosave", "1.2.0", "osaro", "Saves every finished run as a replay, named with map and time, so nothing is lost.",
        PluginStatus::On, true, true, true, "");
    add("legacy-randomizer", "Legacy Randomizer", "0.9.1", "osaro", "Picks a random map from your list.", PluginStatus::On, true,
        false, true, "");
    add("tm-vibrant-shaders", "TM Vibrant Shaders", "1.4.2", "TM Horizon",
        "Better light, bloom and colour grading for the old engine, with presets for day and night maps.",
        PluginStatus::NeedsRestart, true, true, false, "Switched, takes effect after a restart");
    add("clips", "Clips", "0.3.0", "someone", "Records the last 30 seconds on a hotkey.", PluginStatus::Off, false, true, true, "");
    add("broken-mod", "Broken Mod", "0.0.1", "unknown", "Does something it cannot do.", PluginStatus::Failed, true, false,
        false, "Init returned 3");
    add("ghost-viewer", "Ghost Viewer", "2.0.0", "ghosts.io", "Shows the best ghosts of a map while you drive.",
        PluginStatus::Blocked, true, true, true, "solo only: off while online");

    auto hk = [&](int h, const char* owner, const char* ownerName, const char* name, const char* key, const char* conflict) {
        HotkeyView v;
        v.handle = h;
        v.ownerId = owner;
        v.ownerName = ownerName;
        v.name = name;
        v.key = key;
        v.conflict = conflict;
        m.hotkeys.push_back(v);
    };
    hk(1, "host", "Horizon", "Open menu", "Insert", "");
    hk(2, "eswc-autosave", "ESWC Autosave", "Save replay now", "F9", "");
    hk(3, "tm-vibrant-shaders", "TM Vibrant Shaders", "Next preset", "F6", "also Legacy Randomizer (F6)");
    hk(4, "legacy-randomizer", "Legacy Randomizer", "Random map", "F6", "also TM Vibrant Shaders (F5-F8, F12)");
    hk(5, "clips", "Clips", "Save clip", "", "");
    hk(6, "tm-vibrant-shaders", "TM Vibrant Shaders", "Toggle shaders", "F5", "");

    static std::vector<LogLine> log;
    log.clear();
    const char* msgs[] = {"host: started, profile tmnf-solo", "plugin eswc-autosave: init ok", "plugin clips: switched off",
                          "hooks: Present installed", "map loaded: A01-Race", "SAVED C:\\replays\\A01_0-23.456.Replay.Gbx",
                          "net: login ok", "shader preset 'Night' applied"};
    for (int i = 0; i < 40; ++i) {
        LogLine l;
        l.level = i % 7 == 3 ? 2 : (i % 11 == 5 ? 3 : (i % 4 == 0 ? 0 : 1));
        char stamp[64];
        std::snprintf(stamp, sizeof(stamp), "21:04:%02d.%03d  ", 10 + i / 3, (i * 137) % 1000);
        l.text = std::string(stamp) + "[" + (i % 2 ? "host" : "eswc-autosave") + "] " + msgs[i % 8];
        if (l.level == 2) l.text += " (slow: 41 ms)";
        if (l.level == 3) l.text += " FAILED: error 0x80004005";
        log.push_back(l);
    }
    m.log = &log;

    P.actions.setPluginEnabled = [](const std::string& id, bool on) {
        for (PluginView& p : P.model.plugins) {
            if (p.id == id) p.enabled = on;
        }
    };
    P.actions.drawPluginPage = [](const std::string&) {
        ImGui::TextUnformatted("Save folder");
        ImGui::SetNextItemWidth(360.0f);
        ImGui::Combo("##dir", &P.combo, "Documents\\TrackMania\\Replays\0Desktop\0Custom...\0");
        ImGui::Spacing();
        ImGui::Checkbox("Save only personal bests", &P.checkA);
        ImGui::Checkbox("Show a message after saving", &P.checkB);
        ImGui::Spacing();
        ImGui::TextUnformatted("Keep at most");
        ImGui::SetNextItemWidth(360.0f);
        ImGui::SliderFloat("##keep", &P.sliderValue, 0.0f, 1.0f, "%.2f");
        ImGui::Spacing();
        ImGui::Button("Open replay folder");
    };
    P.actions.beginKeyCapture = [](int h) { P.captureHandle = h; };
    P.actions.pollKeyCapture = [](int, std::string&) { return false; };
    P.actions.rebindHotkey = [](int, const std::string&) { return true; };
    P.actions.openLogFolder = [] {};
    P.actions.openLauncher = [] {};
    P.actions.closeMenu = [] {};
}

// ---- PNG output --------------------------------------------------------------------------------------

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
                if (SUCCEEDED(factory->CreateStream(&stream)) &&
                    SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
                    SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                    SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
                    SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
                    SUCCEEDED(frame->SetSize(desc.Width, desc.Height))) {
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
                    frame->SetPixelFormat(&fmt);
                    if (SUCCEEDED(frame->WritePixels(desc.Height, desc.Width * 3, static_cast<UINT>(rgb.size()), rgb.data())) &&
                        SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit())) {
                        ok = true;
                    }
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

// ---- scenes ------------------------------------------------------------------------------------------

struct Scene {
    const char* name;
    bool menu;      // menu open
    bool bar;       // bar active (pointer at the top)
    ImVec2 mouse;
    ImVec2 click;   // (-1,-1) none
    void (*setup)();
};

void gameBackground() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), d, IM_COL32(18, 28, 52, 255), IM_COL32(40, 22, 40, 255),
                                IM_COL32(14, 16, 24, 255), IM_COL32(10, 12, 18, 255));
    // a "track"
    dl->AddQuadFilled(ImVec2(d.x * 0.42f, d.y * 0.45f), ImVec2(d.x * 0.58f, d.y * 0.45f), ImVec2(d.x * 0.9f, d.y),
                      ImVec2(d.x * 0.1f, d.y), IM_COL32(48, 52, 62, 255));
    dl->AddQuadFilled(ImVec2(d.x * 0.495f, d.y * 0.45f), ImVec2(d.x * 0.505f, d.y * 0.45f), ImVec2(d.x * 0.53f, d.y),
                      ImVec2(d.x * 0.47f, d.y), IM_COL32(255, 200, 80, 255));
    for (int i = 0; i < 18; ++i) {
        const float x = d.x * (0.05f + 0.055f * static_cast<float>(i));
        dl->AddCircleFilled(ImVec2(x, d.y * 0.38f + (i % 3) * 14.0f), 5.0f, IM_COL32(255, 220, 150, 230));
    }
    dl->AddText(ImVec2(d.x - 220.0f, d.y - 70.0f), IM_COL32(255, 255, 255, 255), "287 km/h");
}

void setupPlugin() { fillModel(); showPluginPage("eswc-autosave"); }
void setupGeneral() { fillModel(); showPage("general"); }
void setupHotkeys() { fillModel(); showPage("hotkeys"); }
void setupLog() { fillModel(); showPage("log"); }
void setupAbout() { fillModel(); showPage("about"); }
void setupOther() { fillModel(); showPage("other:d3d9.dll"); }
void setupEmpty() {
    fillModel();
    P.model.plugins.clear();
    P.model.safeMode = true;
    showPage("mods");
}
void setupNone() { fillModel(); }

} // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
                      L"HorizonUiPreview", nullptr};
    RegisterClassExW(&wc);
    RECT rc = {0, 0, kWidth, kHeight};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ui preview", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left,
                              rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    const float scale = 1.0f;
    setIconFontFile("assets/lucide-subset.ttf");
    loadFonts(scale);
    applyStyle(scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(dev);

    CreateDirectoryW(L"screens", nullptr);

    const Scene scenes[] = {
        {"plugin", true, false, {330, 330}, {-1, -1}, setupPlugin},
        {"general", true, false, {10, 10}, {-1, -1}, setupGeneral},
        {"hotkeys", true, false, {900, 395}, {900, 395}, setupHotkeys},
        {"log", true, false, {10, 10}, {-1, -1}, setupLog},
        {"about", true, false, {10, 10}, {-1, -1}, setupAbout},
        {"other", true, false, {10, 10}, {-1, -1}, setupOther},
        {"empty", true, false, {10, 10}, {-1, -1}, setupEmpty},
        {"bar", false, true, {800, 6}, {-1, -1}, setupNone},
        {"bar-tooltip", false, true, {679, 26}, {-1, -1}, setupNone},
    };
    int failed = 0;
    for (const Scene& sc : scenes) {
        sc.setup();
        P.captureHandle = 0;
        bool menuOpen = false;
        bool openMenu = false;
        if (sc.menu) menuOpen = true;
        bool menuWasOpen = false;
        (void)menuWasOpen;
        const int frames = 40;
        for (int f = 0; f < frames; ++f) {
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            io.DeltaTime = 1.0f / 30.0f;
            io.AddMousePosEvent(sc.mouse.x, sc.mouse.y);
            if (sc.click.x >= 0.0f) {
                if (f == 25) io.AddMouseButtonEvent(0, true);
                if (f == 27) io.AddMouseButtonEvent(0, false);
            }
            ImGui::NewFrame();
            gameBackground();
            if (menuOpen) drawMenu(P.model, P.actions, &menuOpen);
            drawBar(P.model, P.actions, sc.bar, menuOpen, &openMenu);
            ImGui::EndFrame();
            ImGui::Render();
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
            if (SUCCEEDED(dev->BeginScene())) {
                ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
                dev->EndScene();
            }
            if (f == frames - 1) {
                const std::wstring path = L"screens\\ui-" + std::wstring(sc.name, sc.name + std::strlen(sc.name)) + L".png";
                if (savePng(dev, path.c_str())) {
                    std::printf("saved %ls\n", path.c_str());
                } else {
                    std::printf("FAILED %ls\n", path.c_str());
                    ++failed;
                }
            }
            dev->Present(nullptr, nullptr, nullptr, nullptr);
        }
        // let the bar slide away between scenes
        for (int f = 0; f < 60; ++f) {
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            io.DeltaTime = 1.0f / 30.0f;
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            ImGui::NewFrame();
            drawBar(P.model, P.actions, false, false, &openMenu);
            ImGui::EndFrame();
        }
    }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return failed;
}
