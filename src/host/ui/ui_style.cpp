// Tokens, fonts and the ImGui style of the Horizon menu.
#include "ui.h"

#include "ui_icons.h"

#include "imgui.h"

#include <windows.h>

#include <new>
#include <string>
#include <utility>
#include <vector>

namespace horizon {
namespace ui {

namespace {
std::vector<std::pair<std::string, unsigned long long>> g_icons;
unsigned long long g_wordmark = 0, g_mark = 0;
float g_wordmarkAspect = 1.0f;
} // namespace

void setIcon(const std::string& key, unsigned long long texture) {
    for (auto& kv : g_icons)
        if (kv.first == key) {
            kv.second = texture;
            return;
        }
    g_icons.emplace_back(key, texture);
}
unsigned long long iconFor(const std::string& key) {
    for (const auto& kv : g_icons)
        if (kv.first == key) return kv.second;
    return 0;
}
void setLogo(unsigned long long wordmark, float aspect, unsigned long long mark) {
    g_wordmark = wordmark;
    g_wordmarkAspect = aspect;
    g_mark = mark;
}
unsigned long long logoWordmark(float* aspect) {
    if (aspect) *aspect = g_wordmarkAspect;
    return g_wordmark;
}
unsigned long long logoMark() { return g_mark; }

namespace {

Fonts g_fonts;
float g_scale = 1.0f;
std::string g_iconFile;  // fallback for the standalone UI preview (no host.dll, so no resource)
std::string g_iconBytes; // owns the file's bytes: the atlas does not copy them

// Latin text plus the general punctuation block (dashes, quotes, ellipsis).
const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2013, 0x203A, 0};

std::string fontsDir() {
    char win[MAX_PATH] = {};
    if (GetWindowsDirectoryA(win, MAX_PATH) == 0) return "C:\\Windows\\Fonts\\";
    return std::string(win) + "\\Fonts\\";
}

bool fileExists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void mergeIcons(float px);

// Adds a TTF at the given pixel size; nullptr when the file is missing.
ImFont* addFile(const char* file, float px) {
    const std::string path = fontsDir() + file;
    if (!fileExists(path)) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg, kRanges);
    if (f) mergeIcons(px);
    return f;
}

// The Lucide subset (RCDATA 120 in host.dll, else the file set by setIconFontFile); empty when neither exists.
bool iconFontData(const void** data, int* size) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&iconFontData), &self);
    HRSRC r = self ? FindResourceW(self, MAKEINTRESOURCEW(120), MAKEINTRESOURCEW(10)) : nullptr; // RT_RCDATA
    HGLOBAL g = r ? LoadResource(self, r) : nullptr;
    const void* p = g ? LockResource(g) : nullptr;
    if (p && SizeofResource(self, r) > 0) {
        *data = p;
        *size = static_cast<int>(SizeofResource(self, r));
        return true;
    }
    if (g_iconBytes.empty() && !g_iconFile.empty()) {
        HANDLE h = CreateFileA(g_iconFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const DWORD n = GetFileSize(h, nullptr);
            if (n != INVALID_FILE_SIZE && n > 0 && n < (4u << 20)) {
                g_iconBytes.resize(n);
                DWORD got = 0;
                if (!ReadFile(h, &g_iconBytes[0], n, &got, nullptr) || got != n) g_iconBytes.clear();
            }
            CloseHandle(h);
        }
    }
    if (g_iconBytes.empty()) return false;
    *data = g_iconBytes.data();
    *size = static_cast<int>(g_iconBytes.size());
    return true;
}

// Merges the icons into the font added last, at its pixel size (no-op without the icon font).
void mergeIcons(float px) {
    const void* data = nullptr;
    int size = 0;
    if (!iconFontData(&data, &size)) return;
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.FontDataOwnedByAtlas = false;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = px;
    cfg.GlyphOffset.y = static_cast<float>(static_cast<int>(px * 0.28f + 0.5f)); // lowers the icons onto the text's centre line (tuned in the UI preview)
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<void*>(data), size, px, &cfg,
                                               reinterpret_cast<const ImWchar*>(kIconRanges));
}

ImVec4 col(const Rgba& c, float alpha = -1.0f) {
    return ImVec4(c.r, c.g, c.b, alpha >= 0.0f ? alpha : c.a);
}

} // namespace

const Tokens& tokens() {
    static const Tokens t;
    return t;
}

const Fonts& fonts() {
    return g_fonts;
}

float scale() {
    return g_scale;
}

void setIconFontFile(const std::string& path) {
    g_iconFile = path;
}

void loadFonts(float scale) {
    g_scale = scale < 0.5f ? 0.5f : scale;
    ImGuiIO& io = ImGui::GetIO();
    const float s = g_scale;

    ImFont* body = addFile("segoeui.ttf", 16.0f * s);
    if (!body) {
        ImFontConfig cfg;
        cfg.SizePixels = 16.0f * s;
        body = io.Fonts->AddFontDefault(&cfg);
        mergeIcons(cfg.SizePixels);
    }
    ImFont* bold = addFile("seguisb.ttf", 16.0f * s);
    ImFont* title = addFile("bahnschrift.ttf", 28.0f * s);
    ImFont* small = addFile("segoeui.ttf", 13.0f * s);
    ImFont* mono = addFile("consola.ttf", 14.0f * s);

    g_fonts.body = body;
    g_fonts.bodyBold = bold ? bold : body;
    g_fonts.title = title ? title : (bold ? bold : body);
    g_fonts.small = small ? small : body;
    g_fonts.mono = mono ? mono : body;
    io.FontDefault = body;
}

void reloadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontAtlas* atlas = io.Fonts;
    // keep the old ImFont objects: plugins may hold them (HorizonStyle::font_*), and io.FontDefault points at one
    ImVector<ImFont*> keep = atlas->Fonts;
    atlas->ClearInputData();
    atlas->ClearTexData();
    atlas->Fonts.clear(); // the objects stay alive in keep
    loadFonts(scale);     // new fonts, g_fonts points at them
    for (int i = 0; i < atlas->Fonts.Size; ++i) {
        ImFont* fresh = atlas->Fonts[i];
        if (i >= keep.Size) break; // more fonts than before (a font file appeared): the extra ones are new objects
        ImFont* old = keep[i];
        old->~ImFont();
        new (old) ImFont(); // an empty font, filled by Build below
        for (ImFontConfig& c : atlas->ConfigData)
            if (c.DstFont == fresh) c.DstFont = old;
        atlas->Fonts[i] = old;
        ImFont** slots[] = {&g_fonts.body, &g_fonts.bodyBold, &g_fonts.title, &g_fonts.small, &g_fonts.mono, &io.FontDefault};
        for (ImFont** s : slots)
            if (*s == fresh) *s = old;
        IM_DELETE(fresh);
    }
    // fewer fonts than before: the leftovers stay allocated (never freed while plugins may point at them)
    atlas->Build();
}

void applyStyle(float s) {
    g_scale = s < 0.5f ? 0.5f : s;
    s = g_scale;
    const Tokens& t = tokens();
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();

    // spacing scale 4 / 8 / 12 / 16 / 24 / 32
    st.WindowPadding = ImVec2(16.0f * s, 16.0f * s);
    st.FramePadding = ImVec2(12.0f * s, 8.0f * s);
    st.CellPadding = ImVec2(12.0f * s, 8.0f * s);
    st.ItemSpacing = ImVec2(12.0f * s, 8.0f * s);
    st.ItemInnerSpacing = ImVec2(8.0f * s, 4.0f * s);
    st.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    st.IndentSpacing = 24.0f * s;
    st.ScrollbarSize = 12.0f * s;
    st.GrabMinSize = 14.0f * s;

    st.WindowBorderSize = 1.0f;
    st.ChildBorderSize = 0.0f;
    st.PopupBorderSize = 1.0f;
    st.FrameBorderSize = 0.0f;
    st.TabBorderSize = 0.0f;

    st.WindowRounding = t.radiusLg * s;
    st.ChildRounding = t.radius * s;
    st.FrameRounding = t.radiusSm * s;
    st.PopupRounding = t.radiusSm * s;
    st.ScrollbarRounding = 12.0f * s;
    st.GrabRounding = t.radiusSm * s;
    st.TabRounding = t.radiusSm * s;

    st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    st.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    st.SeparatorTextBorderSize = 1.0f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = col(t.text);
    c[ImGuiCol_TextDisabled] = col(t.muted);
    c[ImGuiCol_WindowBg] = col(t.bg);
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = col(t.bg2, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.11f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.15f);
    c[ImGuiCol_TitleBg] = col(t.bg2);
    c[ImGuiCol_TitleBgActive] = col(t.bg2);
    c[ImGuiCol_TitleBgCollapsed] = col(t.bg2, 0.8f);
    c[ImGuiCol_MenuBarBg] = col(t.bg2);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.24f);
    c[ImGuiCol_ScrollbarGrabActive] = col(t.accent2, 0.85f);
    c[ImGuiCol_CheckMark] = col(t.accent2);
    c[ImGuiCol_SliderGrab] = col(t.accent);
    c[ImGuiCol_SliderGrabActive] = col(t.accent2);
    // Buttons one step lighter than the card surface, so they show on cards (accent buttons push their own colours)
    const Rgba button{0.165f, 0.184f, 0.220f, 1.0f};        // #2a2f38
    const Rgba buttonHover{0.216f, 0.235f, 0.278f, 1.0f};   // #373c47
    const Rgba buttonActive{0.259f, 0.282f, 0.329f, 1.0f};  // #424854
    c[ImGuiCol_Button] = col(button);
    c[ImGuiCol_ButtonHovered] = col(buttonHover);
    c[ImGuiCol_ButtonActive] = col(buttonActive);
    c[ImGuiCol_Header] = col(t.accent, 0.16f);
    c[ImGuiCol_HeaderHovered] = col(t.accent, 0.28f);
    c[ImGuiCol_HeaderActive] = col(t.accent, 0.40f);
    c[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    c[ImGuiCol_SeparatorHovered] = col(t.accent2, 0.6f);
    c[ImGuiCol_SeparatorActive] = col(t.accent2);
    c[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    c[ImGuiCol_ResizeGripHovered] = col(t.accent2, 0.6f);
    c[ImGuiCol_ResizeGripActive] = col(t.accent2);
    c[ImGuiCol_TabHovered] = col(t.accent, 0.4f);
    c[ImGuiCol_Tab] = col(button);
    c[ImGuiCol_TabSelected] = col(t.accent, 0.3f);
    c[ImGuiCol_TabSelectedOverline] = col(t.accent);
    c[ImGuiCol_TabDimmed] = col(t.bg2);
    c[ImGuiCol_TabDimmedSelected] = col(t.surface);
    c[ImGuiCol_TabDimmedSelectedOverline] = col(t.accent, 0.5f);
    c[ImGuiCol_PlotLines] = col(t.accent2);
    c[ImGuiCol_PlotLinesHovered] = col(t.accent3);
    c[ImGuiCol_PlotHistogram] = col(t.accent);
    c[ImGuiCol_PlotHistogramHovered] = col(t.accent3);
    c[ImGuiCol_TableHeaderBg] = col(t.bg2);
    c[ImGuiCol_TableBorderStrong] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    c[ImGuiCol_TableBorderLight] = ImVec4(1.0f, 1.0f, 1.0f, 0.05f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
    c[ImGuiCol_TextLink] = col(t.accent2);
    c[ImGuiCol_TextSelectedBg] = col(t.accent, 0.35f);
    c[ImGuiCol_DragDropTarget] = col(t.accent3);
    c[ImGuiCol_NavCursor] = col(t.accent2, 0.8f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.5f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

} // namespace ui
} // namespace horizon
