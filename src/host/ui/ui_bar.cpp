// The slim Horizon bar at the top edge of the game window.
#include "ui.h"
#include "ui_common.h"

#include "imgui.h"
#include "horizon_imgui.h" // HZ_ICON_*

#include <cmath>

namespace horizon {
namespace ui {

using namespace detail;

namespace {

struct BarState {
    float slide = 0.0f;       // 0 hidden .. 1 shown
    double hideAt = 0.0;      // keeps the bar open until this time
    bool hovered = false;     // pointer was over the bar last frame
};
BarState g;

const float kSlideSeconds = 0.15f;
const double kLingerSeconds = 0.6;

// Pill button, true when clicked.
bool pillButton(ImDrawList* dl, const char* id, ImVec2 p, float h, ImFont* font, const std::string& label,
                bool primary) {
    const float s = scale();
    const Tokens& t = tokens();
    const float w = textSize(font, label).x + 28.0f * s;
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImU32 fill = W(hov ? 0.14f : 0.07f);
    ImU32 text = U(t.text);
    if (primary) {
        fill = U(hov ? t.accent2 : t.accent);
        text = U(t.accentInk);
    }
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), fill, h * 0.5f);
    drawTextCentred(dl, font, ImVec2(p.x + w * 0.5f, p.y + h * 0.5f), text, label);
    return clicked;
}

float buttonWidth(ImFont* font, const char* label) {
    return textSize(font, label).x + 28.0f * scale();
}

} // namespace

bool barHovered() {
    return g.hovered && g.slide > 0.0f;
}

bool drawBar(const Model& model, const Actions& actions, bool mouseAtTop, bool menuOpen, bool* openMenu) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImGuiIO& io = ImGui::GetIO();
    const double now = ImGui::GetTime();

    if (menuOpen) { // the big menu has its own header
        g.slide = 0.0f;
        g.hovered = false;
        g.hideAt = 0.0;
        return false;
    }

    if (mouseAtTop || g.hovered) g.hideAt = now + kLingerSeconds;
    const bool wantShown = now < g.hideAt;
    const float step = io.DeltaTime / kSlideSeconds;
    g.slide = wantShown ? std::min(1.0f, g.slide + step) : std::max(0.0f, g.slide - step);
    if (g.slide <= 0.0f) {
        g.hovered = false;
        return false;
    }
    const float eased = 1.0f - (1.0f - g.slide) * (1.0f - g.slide) * (1.0f - g.slide);

    // geometry
    const float h = 52.0f * s;
    const float pad = 14.0f * s;
    const float gap = 8.0f * s;
    const float iconD = 32.0f * s;
    const float btnH = 32.0f * s;
    ImFont* bf = F(f.bodyBold);
    const bool hasOnline = model.online;
    const char* menuLabel = HZ_ICON_MENU_U8 "  Menu";
    const char* logLabel = HZ_ICON_SCROLL_TEXT_U8 "  Log";
    const char* launcherLabel = "TM Horizon  " HZ_ICON_EXTERNAL_LINK_U8;
    const float chipH = 26.0f * s;
    float fixedW = pad + 28.0f * s + 12.0f * s;                       // logo
    float tail = 12.0f * s + 1.0f * s + 12.0f * s;                    // separator
    tail += buttonWidth(bf, menuLabel) + gap + buttonWidth(bf, logLabel) + gap + buttonWidth(bf, launcherLabel);
    if (hasOnline) tail += gap + chipWidth(f.small, "Online", true);
    tail += pad;
    size_t maxIcons = model.plugins.size();
    const float room = io.DisplaySize.x - 32.0f * s - fixedW - tail;
    while (maxIcons > 0 && static_cast<float>(maxIcons) * (iconD + gap) - gap > room) --maxIcons;
    const float iconsW = maxIcons > 0 ? static_cast<float>(maxIcons) * (iconD + gap) - gap : 0.0f;
    const float w = fixedW + iconsW + tail;

    const ImVec2 pos(std::floor((io.DisplaySize.x - w) * 0.5f), std::floor(-h * (1.0f - eased)));
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoBackground;
    ImGui::Begin("##horizon_bar", nullptr, flags);
    ImGui::PopStyleVar(2);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = t.radius * s;
    const ImVec2 br(pos.x + w, pos.y + h);
    dl->AddRectFilled(pos, br, U(t.bg2, 0.96f), r, ImDrawFlags_RoundCornersBottom);
    dl->AddRect(pos, br, W(0.07f), r, ImDrawFlags_RoundCornersBottom, 1.0f);
    dl->AddLine(ImVec2(pos.x + r, br.y - 0.5f), ImVec2(br.x - r, br.y - 0.5f), U(t.accent), 2.0f * s);

    const float midY = pos.y + h * 0.5f;
    float x = pos.x + pad;
    // logo mark
    {
        const float d = 28.0f * s;
        if (const unsigned long long mark = logoMark()) {
            dl->AddImageRounded(static_cast<ImTextureID>(mark), ImVec2(x, midY - d * 0.5f), ImVec2(x + d, midY + d * 0.5f), ImVec2(0, 0),
                                ImVec2(1, 1), IM_COL32_WHITE, 7.0f * s);
        } else {
            dl->AddRectFilled(ImVec2(x, midY - d * 0.5f), ImVec2(x + d, midY + d * 0.5f), U(t.accent), 8.0f * s);
            drawTextCentred(dl, f.bodyBold, ImVec2(x + d * 0.5f, midY - 0.5f), U(t.accentInk), "H");
        }
        x += d + 12.0f * s;
    }
    // plugin icons
    for (size_t i = 0; i < maxIcons; ++i) {
        const PluginView& p = model.plugins[i];
        ImGui::PushID(p.id.c_str());
        const ImVec2 tl(x, midY - iconD * 0.5f);
        ImGui::SetCursorScreenPos(tl);
        const bool clicked = ImGui::InvisibleButton("##icon", ImVec2(iconD, iconD));
        const bool hov = ImGui::IsItemHovered();
        const ImVec2 c(tl.x + iconD * 0.5f, tl.y + iconD * 0.5f);
        if (hov) {
            dl->AddCircle(c, iconD * 0.5f + 3.0f * s, U(t.accent, 0.9f), 32, 1.5f * s);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        drawIcon(dl, c, iconD * 0.5f, p.id, p.name, f.bodyBold);
        if (p.status == PluginStatus::Off || p.status == PluginStatus::WrongGame || p.status == PluginStatus::Failed) {
            dl->AddCircleFilled(c, iconD * 0.5f, U(t.bg2, 0.45f), 32); // dimmed when not running
        }
        Rgba dot = t.muted;
        switch (p.status) {
        case PluginStatus::On: dot = t.ok; break;
        case PluginStatus::Failed: dot = t.bad; break;
        case PluginStatus::Blocked:
        case PluginStatus::NeedsRestart: dot = t.warn; break;
        default: break;
        }
        const ImVec2 dc(tl.x + iconD - 4.0f * s, tl.y + iconD - 4.0f * s);
        dl->AddCircleFilled(dc, 6.0f * s, U(t.bg2), 16);
        dl->AddCircleFilled(dc, 4.5f * s, U(dot), 16);

        if (clicked) togglePluginWindow(p.id); // free window with the plugin's page; on/off is a switch inside it
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            showPluginPage(p.id);
            if (openMenu) *openMenu = true;
        }
        if (hov) {
            std::string tip = p.name + ": " + statusLabel(p.status, false);
            if (!p.statusText.empty() && p.status != PluginStatus::NeedsRestart) tip += "\n" + p.statusText;
            tip += "\nLinksklick: Fenster \xC2\xB7 Rechtsklick: Einstellungen";
            ImGui::SetTooltip("%s", tip.c_str());
        }
        ImGui::PopID();
        x += iconD + gap;
    }
    if (maxIcons > 0) x -= gap;
    // separator
    x += 12.0f * s;
    dl->AddLine(ImVec2(x, midY - 12.0f * s), ImVec2(x, midY + 12.0f * s), W(0.12f), 1.0f);
    x += 1.0f * s + 12.0f * s;

    const float by = midY - btnH * 0.5f;
    if (pillButton(dl, "##menu", ImVec2(x, by), btnH, bf, menuLabel, true) && openMenu) *openMenu = true;
    x += buttonWidth(bf, menuLabel) + gap;
    if (pillButton(dl, "##log", ImVec2(x, by), btnH, bf, logLabel, false)) {
        showPage("log");
        if (openMenu) *openMenu = true;
    }
    x += buttonWidth(bf, logLabel) + gap;
    if (pillButton(dl, "##launcher", ImVec2(x, by), btnH, bf, launcherLabel, false) && actions.openLauncher) {
        actions.openLauncher();
    }
    x += buttonWidth(bf, launcherLabel) + gap;
    if (hasOnline) drawChip(dl, ImVec2(x, midY - chipH * 0.5f), chipH, f.small, "Online", t.ok, true);

    ImGui::End();

    g.hovered = ImGui::IsMouseHoveringRect(pos, br, false);
    return true;
}

} // namespace ui
} // namespace horizon
