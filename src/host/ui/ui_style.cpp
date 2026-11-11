// Tokens, fonts and the ImGui style of the Horizon menu.
#include "ui.h"

#include "imgui.h"

#include <windows.h>

#include <string>

namespace horizon {
namespace ui {

namespace {

Fonts g_fonts;
float g_scale = 1.0f;

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

// Adds a TTF at the given pixel size; nullptr when the file is missing.
ImFont* addFile(const char* file, float px) {
    const std::string path = fontsDir() + file;
    if (!fileExists(path)) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg, kRanges);
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

void loadFonts(float scale) {
    g_scale = scale < 0.5f ? 0.5f : scale;
    ImGuiIO& io = ImGui::GetIO();
    const float s = g_scale;

    ImFont* body = addFile("segoeui.ttf", 16.0f * s);
    if (!body) {
        ImFontConfig cfg;
        cfg.SizePixels = 16.0f * s;
        body = io.Fonts->AddFontDefault(&cfg);
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

    const Rgba hover{0.153f, 0.169f, 0.204f, 1.0f};   // surface, one step lighter
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
    c[ImGuiCol_Button] = col(t.surface);
    c[ImGuiCol_ButtonHovered] = col(hover);
    c[ImGuiCol_ButtonActive] = col(t.accent, 0.35f);
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
    c[ImGuiCol_Tab] = col(t.surface);
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
