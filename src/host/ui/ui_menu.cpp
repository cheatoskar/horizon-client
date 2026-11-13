// The big centred menu: header, sidebar (mods, Horizon pages, other mods) and the selected page.
#include "ui.h"
#include "ui_common.h"

#include "imgui.h"
#include "horizon_imgui.h" // HZ_ICON_*

#include <cmath>
#include <cstring>

namespace horizon {
namespace ui {

using namespace detail;

namespace {

struct MenuState {
    std::string sel;          // "general", "hotkeys", "log", "about", "plugin:<id>", "other:<file>", "mods" (resolve), "" (first open)
    int logFilter = 0;        // 0 all, 1 problems
    int captureHandle = 0;    // hotkey waiting for a key
    bool captureEnded = false;
    std::string note;         // short message under the key controls
    double noteUntil = 0.0;
    double openedAt = 0.0;
    int lastFrame = -10;
    std::string tabPlugin;    // selectPanelTab: plugin whose page selects tabKey once
    std::string tabKey;       // "" = Settings
    bool tabPending = false;
    int scaleDrag = 0;        // content scale slider value while it is dragged (0 = not dragged)
    bool narrowPage = false;  // narrow menu (small window): the page is shown instead of the list
    bool narrow = false;      // the last frame used the narrow layout
    bool rowClicked = false;  // a sidebar row was clicked in this frame
    float menuAlpha = 1.0f;   // menu background opacity (General) relative to the default 94 %: sidebar and dim layer
};
MenuState g;
MenuMetrics g_metrics;

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

const PluginView* findPlugin(const Model& m, const std::string& id) {
    for (const PluginView& p : m.plugins) {
        if (p.id == id) return &p;
    }
    return nullptr;
}
const GroupView* findGroup(const Model& m, const std::string& id) {
    for (const GroupView& gv : m.groups)
        if (gv.id == id) return &gv;
    return nullptr;
}
// The group a module is shown in (nullptr: listed alone).
const GroupView* groupOfPlugin(const Model& m, const std::string& pluginId) {
    for (const GroupView& gv : m.groups)
        for (const GroupFeatureView& fv : gv.features)
            if (fv.pluginId == pluginId) return &gv;
    return nullptr;
}
const OtherModView* findOther(const Model& m, const std::string& file) {
    for (const OtherModView& o : m.otherMods) {
        if (o.file == file) return &o;
    }
    return nullptr;
}
bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

// The pointer is over a window that takes the mouse (a widget, a popped-out panel, a free plugin window, a
// plugin's own window). ImGui finds it at the start of the frame from last frame's windows.
bool pointerOverAnyWindow() {
    return ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                                  ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
}

// Pill-shaped segmented control. Returns the (new) selection.
int segmented(const char* id, const char* const* labels, int n, int sel, float h, ImFont* font) {
    const float s = scale();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float pad = 4.0f * s;
    std::vector<float> w(static_cast<size_t>(n));
    float total = pad * 2.0f;
    for (int i = 0; i < n; ++i) {
        w[static_cast<size_t>(i)] = textSize(font, labels[i]).x + 32.0f * s;
        total += w[static_cast<size_t>(i)];
    }
    dl->AddRectFilled(p, ImVec2(p.x + total, p.y + h), W(0.05f), h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + total, p.y + h), W(0.07f), h * 0.5f, 0, 1.0f);
    float x = p.x + pad;
    int result = sel;
    ImGui::PushID(id);
    for (int i = 0; i < n; ++i) {
        const ImVec2 q(x, p.y + pad);
        const ImVec2 sz(w[static_cast<size_t>(i)], h - 2.0f * pad);
        ImGui::SetCursorScreenPos(q);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", sz)) result = i;
        ImGui::PopID();
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const Tokens& t = tokens();
        if (i == sel) {
            dl->AddRectFilled(q, ImVec2(q.x + sz.x, q.y + sz.y), U(t.accent), sz.y * 0.5f);
        } else if (hov) {
            dl->AddRectFilled(q, ImVec2(q.x + sz.x, q.y + sz.y), W(0.07f), sz.y * 0.5f);
        }
        drawTextCentred(dl, font, ImVec2(q.x + sz.x * 0.5f, q.y + sz.y * 0.5f),
                        i == sel ? U(t.accentInk) : U(hov ? t.text : t.text2), labels[i]);
        x += sz.x;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(total, h));
    return result;
}

// Round close button with a cross; true when clicked.
bool closeButton(ImVec2 p, float d) {
    const float s = scale();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton("##close", ImVec2(d, d));
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImVec2 c(p.x + d * 0.5f, p.y + d * 0.5f);
    dl->AddCircleFilled(c, d * 0.5f, hov ? W(0.14f) : W(0.07f), 32);
    const float a = d * 0.2f;
    const ImU32 col = U(hov ? tokens().text : tokens().text2);
    dl->AddLine(ImVec2(c.x - a, c.y - a), ImVec2(c.x + a, c.y + a), col, 1.6f * s);
    dl->AddLine(ImVec2(c.x - a, c.y + a), ImVec2(c.x + a, c.y - a), col, 1.6f * s);
    return clicked;
}

// Switch with its own hit area and animation. Returns true when clicked (and usable).
bool inlineSwitch(const char* id, ImVec2 pos, ImVec2 size, bool on, bool usable, bool* hoveredOut) {
    const float s = scale();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(ImVec2(pos.x - 4.0f * s, pos.y - 4.0f * s));
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size.x + 8.0f * s, size.y + 8.0f * s));
    const bool hov = ImGui::IsItemHovered();
    float& anim = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), on ? 1.0f : 0.0f);
    const float target = on ? 1.0f : 0.0f;
    const float step = ImGui::GetIO().DeltaTime / 0.12f;
    if (anim < target) anim = std::min(target, anim + step);
    if (anim > target) anim = std::max(target, anim - step);
    drawSwitch(dl, pos, size, anim, usable, hov && usable);
    if (hov && usable) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (hoveredOut) *hoveredOut = hov;
    return clicked && usable;
}

// ---- header ----------------------------------------------------------------------------------------

void drawHeader(const Model& model, bool* open) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float titleH = F(f.title)->FontSize;
    const float smallH = F(f.small)->FontSize;
    const float height = titleH + smallH + 6.0f * s;

    float aspect = 1.0f;
    float logoEnd = p.x;
    if (const unsigned long long logo = logoWordmark(&aspect)) {
        // the TM Horizon wordmark (planet + chrome TM + HORIZON), as tall as the title line
        const float lh = titleH * 1.15f;
        dl->AddImage(static_cast<ImTextureID>(logo), ImVec2(p.x, p.y - titleH * 0.08f), ImVec2(p.x + lh * aspect, p.y - titleH * 0.08f + lh));
        logoEnd = p.x + lh * aspect;
    } else {
        const std::string tm = "TM ";
        drawText(dl, f.title, p, U(t.text), tm);
        drawText(dl, f.title, ImVec2(p.x + textSize(f.title, tm).x, p.y), U(t.accent), "HORIZON");
        logoEnd = p.x + textSize(f.title, tm + "HORIZON").x;
    }

    const float closeD = 30.0f * s;
    const float rightX = p.x + width;
    const float midY = p.y + height * 0.5f;
    if (closeButton(ImVec2(rightX - closeD, midY - closeD * 0.5f), closeD)) *open = false;

    struct Chip {
        std::string text;
        Rgba colour;
        bool dot;
        std::string tip;
    };
    std::vector<Chip> chips;
    if (model.safeMode) chips.push_back({"Safe mode", t.warn, false, "No mods this start."});
    if (model.runGate) chips.push_back({"Run check", t.warn, false, "Solo-only modules paused."});
    {
        // the launcher (TM Horizon) over the bridge: 3 = connected, 2 = waiting for Allow, 0 = switched off
        const int b = model.bridgeState;
        const char* text = b == 3 ? "Launcher connected" : b == 2 ? "Launcher: allow?" : b == 0 ? "Launcher off" : "Launcher not connected";
        const Rgba& c = b == 3 ? t.ok : b == 2 ? t.warn : t.muted;
        chips.push_back({text, c, true, model.bridgeText.empty() ? std::string("TM Horizon") : "TM Horizon: " + model.bridgeText});
    }
    chips.push_back({model.online ? "Game: online" : "Game: offline", model.online ? t.ok : t.muted, true,
                     "Solo-only modules pause online."});
    // Chips between the logo and the close button. When the full chips do not fit (small window), each
    // shrinks to a coloured dot and its text moves into the tooltip.
    const float chipH = 26.0f * s;
    const float gapX = 8.0f * s;
    const float room = rightX - closeD - 16.0f * s - (logoEnd + 16.0f * s);
    float full = 0.0f;
    for (const Chip& c : chips) full += chipWidth(f.small, c.text, c.dot) + gapX;
    const bool dots = full > room;
    float x = rightX - closeD - 16.0f * s;
    for (size_t i = chips.size(); i-- > 0;) {
        const float cw = dots ? chipH : chipWidth(f.small, chips[i].text, chips[i].dot);
        x -= cw;
        const ImVec2 cp(x, midY - chipH * 0.5f);
        if (dots) {
            const ImVec2 c(cp.x + chipH * 0.5f, cp.y + chipH * 0.5f);
            dl->AddCircleFilled(c, chipH * 0.5f, W(0.06f), 24);
            dl->AddCircleFilled(c, 4.0f * s, U(chips[i].colour), 16);
        } else {
            drawChip(dl, cp, chipH, f.small, chips[i].text, chips[i].colour, chips[i].dot);
        }
        if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(cp, ImVec2(cp.x + cw, cp.y + chipH))) {
            if (dots) ImGui::SetTooltip("%s%s%s", chips[i].text.c_str(), chips[i].tip.empty() ? "" : "\n", chips[i].tip.c_str());
            else if (!chips[i].tip.empty()) ImGui::SetTooltip("%s", chips[i].tip.c_str());
        }
        x -= gapX;
    }
    // the game name under the logo, cut before the chips
    std::string sub = model.gameName;
    if (!model.exeVersion.empty()) sub += (sub.empty() ? "" : "  \xC2\xB7  ") + model.exeVersion;
    const float subX = p.x + 1.0f * s;
    const float subW = x - 8.0f * s - subX;
    drawText(dl, f.small, ImVec2(subX, p.y + titleH + 4.0f * s), U(t.muted), ellipsize(f.small, sub, std::max(0.0f, subW)));
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, height));
}

// ---- sidebar ---------------------------------------------------------------------------------------

void sectionLabel(const char* text) {
    const float s = scale();
    const Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = F(f.small)->FontSize + 10.0f * s;
    drawText(dl, f.small, ImVec2(p.x + 10.0f * s, p.y + 6.0f * s), U(tokens().muted), text);
    ImGui::Dummy(ImVec2(1.0f, h));
}

// Row background (selected / hover). Returns true when clicked.
bool rowButton(const char* id, ImVec2 size, bool selected, bool* hoveredOut) {
    const float s = scale();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton(id, size);
    if (clicked) g.rowClicked = true;
    const bool hov = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + size.x, p.y + size.y));
    const Tokens& t = tokens();
    const ImVec2 br(p.x + size.x, p.y + size.y);
    if (selected) {
        dl->AddRectFilled(p, br, U(t.accent, 0.16f), t.radiusSm * s);
        dl->AddRectFilled(ImVec2(p.x, p.y + size.y * 0.25f), ImVec2(p.x + 3.0f * s, p.y + size.y * 0.75f), U(t.accent), 2.0f * s);
    } else if (hov) {
        dl->AddRectFilled(p, br, W(0.06f), t.radiusSm * s);
    }
    if (hoveredOut) *hoveredOut = hov;
    return clicked;
}

void horizonRow(const char* label, const char* page, const char* icon) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 32.0f * s);
    const bool selected = g.sel == page;
    bool hov = false;
    if (rowButton(label, size, selected, &hov)) g.sel = page;
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float iconW = textSize(f.body, icon).x;
    drawText(dl, f.body, ImVec2(p.x + 16.0f * s, p.y + (size.y - F(f.body)->FontSize) * 0.5f), U(selected ? t.accent : t.muted),
             icon);
    drawText(dl, selected ? f.bodyBold : f.body,
             ImVec2(p.x + 16.0f * s + iconW + 10.0f * s, p.y + (size.y - F(f.body)->FontSize) * 0.5f),
             U(selected ? t.text : t.text2), label);
}

void pluginRow(const PluginView& p, const Actions& actions) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 40.0f * s);
    const std::string key = "plugin:" + p.id;
    ImGui::PushID(p.id.c_str());
    const bool selected = g.sel == key;
    bool hov = false;
    if (rowButton("##row", size, selected, &hov)) g.sel = key;

    const float iconR = 13.0f * s;
    drawIcon(dl, ImVec2(pos.x + 14.0f * s + iconR, pos.y + size.y * 0.5f), iconR, p.id, p.name, f.small);
    const float textX = pos.x + 14.0f * s + iconR * 2.0f + 10.0f * s;
    const ImVec2 swSize(36.0f * s, 20.0f * s);
    const ImVec2 swPos(pos.x + size.x - 12.0f * s - swSize.x, pos.y + (size.y - swSize.y) * 0.5f);
    const float textW = swPos.x - 10.0f * s - textX;
    const float bodyH = F(f.body)->FontSize, smallH = F(f.small)->FontSize;
    const float top = pos.y + (size.y - bodyH - smallH - 1.0f * s) * 0.5f;
    drawText(dl, selected ? f.bodyBold : f.body, ImVec2(textX, top), U(t.text), ellipsize(f.body, p.name, textW));
    const Rgba sc = statusColour(p.status);
    dl->AddCircleFilled(ImVec2(textX + 3.0f * s, top + bodyH + 1.0f * s + smallH * 0.5f + 1.0f * s), 3.0f * s, U(sc), 12);
    drawText(dl, f.small, ImVec2(textX + 11.0f * s, top + bodyH + 1.0f * s), U(sc), statusLabel(p.status, true));

    bool swHov = false;
    if (inlineSwitch("##sw", swPos, swSize, p.enabled, canToggle(p.status), &swHov) && actions.setPluginEnabled) {
        actions.setPluginEnabled(p.id, !p.enabled);
    }
    if (swHov && canToggle(p.status) && p.restartToSwitch) {
        ImGui::SetTooltip("%s", p.restartText.empty() ? "Applies after a restart." : p.restartText.c_str());
    } else if (hov && !swHov && !p.statusText.empty()) {
        ImGui::SetTooltip("%s", p.statusText.c_str());
    }
    if (hov && !swHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y));
    ImGui::Dummy(ImVec2(1.0f, 2.0f * s));
    ImGui::PopID();
}

// A module group (Legacy Essentials): like a module row, one switch for all of its features.
void groupRow(const GroupView& gv, const Actions& actions) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 40.0f * s);
    const std::string key = "group:" + gv.id;
    ImGui::PushID(key.c_str());
    const bool selected = g.sel == key;
    bool hov = false;
    if (rowButton("##row", size, selected, &hov)) g.sel = key;
    const float iconR = 13.0f * s;
    drawIcon(dl, ImVec2(pos.x + 14.0f * s + iconR, pos.y + size.y * 0.5f), iconR, key, gv.name, f.small);
    const float textX = pos.x + 14.0f * s + iconR * 2.0f + 10.0f * s;
    const ImVec2 swSize(36.0f * s, 20.0f * s);
    const ImVec2 swPos(pos.x + size.x - 12.0f * s - swSize.x, pos.y + (size.y - swSize.y) * 0.5f);
    const float textW = swPos.x - 10.0f * s - textX;
    const float bodyH = F(f.body)->FontSize, smallH = F(f.small)->FontSize;
    const float top = pos.y + (size.y - bodyH - smallH - 1.0f * s) * 0.5f;
    drawText(dl, selected ? f.bodyBold : f.body, ImVec2(textX, top), U(t.text), ellipsize(f.body, gv.name, textW));
    int on = 0;
    for (const GroupFeatureView& fv : gv.features) on += fv.enabled ? 1 : 0;
    const Rgba sc = statusColour(gv.enabled ? PluginStatus::On : PluginStatus::Off);
    const std::string sub = gv.enabled ? std::to_string(on) + " of " + std::to_string(gv.features.size()) + " on" : std::string("Off");
    dl->AddCircleFilled(ImVec2(textX + 3.0f * s, top + bodyH + 1.0f * s + smallH * 0.5f + 1.0f * s), 3.0f * s, U(sc), 12);
    drawText(dl, f.small, ImVec2(textX + 11.0f * s, top + bodyH + 1.0f * s), U(sc), sub);
    bool swHov = false;
    if (inlineSwitch("##sw", swPos, swSize, gv.enabled, true, &swHov) && actions.setGroupEnabled) actions.setGroupEnabled(gv.id, !gv.enabled);
    if (hov && !swHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y));
    ImGui::Dummy(ImVec2(1.0f, 2.0f * s));
    ImGui::PopID();
}

void otherRow(const OtherModView& o) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 40.0f * s);
    const std::string key = "other:" + o.file;
    ImGui::PushID(o.file.c_str());
    const bool selected = g.sel == key;
    bool hov = false;
    if (rowButton("##row", size, selected, &hov)) g.sel = key;
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float iconR = 13.0f * s;
    const ImVec2 c(pos.x + 14.0f * s + iconR, pos.y + size.y * 0.5f);
    if (!drawPictureIcon(dl, c, iconR, "other:" + o.name)) {
        dl->AddCircleFilled(c, iconR, W(0.10f), 32);
        drawTextCentred(dl, f.small, ImVec2(c.x, c.y - 0.5f), U(t.text2), initials(o.name));
    }
    const float textX = pos.x + 14.0f * s + iconR * 2.0f + 10.0f * s;
    const float textW = pos.x + size.x - 12.0f * s - textX;
    const float bodyH = F(f.body)->FontSize, smallH = F(f.small)->FontSize;
    const float top = pos.y + (size.y - bodyH - smallH - 1.0f * s) * 0.5f;
    drawText(dl, selected ? f.bodyBold : f.body, ImVec2(textX, top), U(t.text2), ellipsize(f.body, o.name, textW));
    drawText(dl, f.small, ImVec2(textX, top + bodyH + 1.0f * s), U(t.muted), ellipsize(f.small, o.kind, textW));
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y));
    ImGui::Dummy(ImVec2(1.0f, 2.0f * s));
    ImGui::PopID();
}

void drawSidebar(const Model& model, const Actions& actions, float width, float height) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec(t.bg2, t.bg2.a * g.menuAlpha));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, t.radius * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s, 6.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    const bool shown = ImGui::BeginChild("##sidebar", ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar(3);
    if (shown) {
        sectionLabel("MODS");
        if (model.plugins.empty()) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x - 20.0f * s;
            const std::string msg = model.safeMode ? "Safe mode: no mods." : "No mods.";
            const std::vector<std::string> lines = wrapLines(f.small, msg, w, 4);
            const float lh = F(f.small)->FontSize + 3.0f * s;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (size_t i = 0; i < lines.size(); ++i) {
                drawText(dl, f.small, ImVec2(p.x + 10.0f * s, p.y + 2.0f * s + static_cast<float>(i) * lh), U(t.text2), lines[i]);
            }
            ImGui::Dummy(ImVec2(1.0f, static_cast<float>(lines.size()) * lh + 10.0f * s));
        }
        for (const GroupView& gv : model.groups) groupRow(gv, actions);
        for (const PluginView& p : model.plugins)
            if (!groupOfPlugin(model, p.id)) pluginRow(p, actions);

        sectionLabel("HORIZON");
        horizonRow("General", "general", HZ_ICON_SETTINGS_U8);
        horizonRow("Hotkeys", "hotkeys", HZ_ICON_KEYBOARD_U8);
        horizonRow("Log", "log", HZ_ICON_SCROLL_TEXT_U8);
        horizonRow("About", "about", HZ_ICON_INFO_U8);

        if (!model.otherMods.empty()) {
            sectionLabel("OTHER MODS");
            for (const OtherModView& o : model.otherMods) otherRow(o);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ---- pages -----------------------------------------------------------------------------------------

void dim(const std::string& text, bool small) {
    const Fonts& f = fonts();
    ImGui::PushFont(F(small ? f.small : f.body));
    ImGui::PushStyleColor(ImGuiCol_Text, toVec(tokens().muted));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void infoRow(const char* label, const std::string& value) {
    const float s = scale();
    const Tokens& t = tokens();
    const float labelW = 150.0f * s;
    const float x0 = ImGui::GetCursorPosX();
    ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.muted));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(x0 + labelW);
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(value.empty() ? "-" : value.c_str());
    ImGui::PopTextWrapPos();
}

bool beginCard(const char* id, float padX, float padY, bool autoHeight, float minHeight = 0.0f) {
    const float s = scale();
    const Tokens& t = tokens();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec(t.surface, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, t.radius * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX * s, padY * s));
    const float h = !autoHeight && ImGui::GetContentRegionAvail().y < minHeight ? minHeight : 0.0f;
    const bool shown = ImGui::BeginChild(id, ImVec2(0.0f, h),
                                         ImGuiChildFlags_AlwaysUseWindowPadding |
                                             (autoHeight ? ImGuiChildFlags_AutoResizeY : 0));
    ImGui::PopStyleVar(2);
    return shown;
}
void endCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Big page header: icon, title, chip, meta line. Returns nothing; advances the cursor.
// rightReserve: room kept free at the right end (the widget switches of a plugin page).
void pageHeader(const std::string& id, const std::string& name, bool greyIcon, const char* chipText, const Rgba* chipColour,
                const std::string& meta, float rightReserve = 0.0f) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float fullWidth = ImGui::GetContentRegionAvail().x;
    const float width = std::max(0.0f, fullWidth - rightReserve);
    const float iconR = 26.0f * s;
    const ImVec2 c(p.x + iconR, p.y + iconR);
    if (greyIcon) {
        if (!drawPictureIcon(dl, c, iconR, "other:" + name)) {
            dl->AddCircleFilled(c, iconR, W(0.10f), 40);
            drawTextCentred(dl, f.bodyBold, ImVec2(c.x, c.y - 0.5f), U(t.text2), initials(name));
        }
    } else {
        drawIcon(dl, c, iconR, id, name, f.bodyBold);
    }
    const float tx = p.x + iconR * 2.0f + 16.0f * s;
    const float titleH = F(f.title)->FontSize;
    const float smallH = F(f.small)->FontSize;
    const float top = p.y + (iconR * 2.0f - titleH - smallH - 2.0f * s) * 0.5f;
    const float textW = std::max(0.0f, p.x + width - tx);
    const std::string title = ellipsize(f.title, name, textW);
    drawText(dl, f.title, ImVec2(tx, top), U(t.text), title);
    if (chipText) {
        const float chipH = 26.0f * s;
        const float chipX = tx + textSize(f.title, title).x + 16.0f * s;
        if (chipX + chipWidth(f.small, chipText, false) <= p.x + width)
            drawChip(dl, ImVec2(chipX, top + (titleH - chipH) * 0.5f + 2.0f * s), chipH, f.small, chipText, *chipColour, false);
    }
    drawText(dl, f.small, ImVec2(tx + 1.0f * s, top + titleH + 2.0f * s), U(t.muted), ellipsize(f.small, meta, textW));
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(fullWidth, iconR * 2.0f + 14.0f * s));
}

// The widgets of a plugin page (HUD panels), at most three: a switch each at the right end of the header, so a
// widget can be shown or hidden without opening its tab. Width 0 when there are none or no room.
std::vector<const PanelView*> headerWidgets(const PluginView& plugin) {
    std::vector<const PanelView*> out;
    for (const PanelView& pv : plugin.panels)
        if (pv.hud && out.size() < 3) out.push_back(&pv);
    return out;
}
float headerWidgetsWidth(const std::vector<const PanelView*>& ws) {
    const float s = scale();
    float w = 0.0f;
    for (const PanelView* pv : ws) w += textSize(fonts().small, pv->title).x + 8.0f * s + 36.0f * s + 18.0f * s;
    return w;
}
void drawHeaderWidgets(const PluginView& plugin, const std::vector<const PanelView*>& ws, ImVec2 headerPos, float headerW,
                       const Actions& actions) {
    if (ws.empty()) return;
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 back = ImGui::GetCursorScreenPos();
    const float cy = headerPos.y + 26.0f * s; // the icon's centre line
    float x = headerPos.x + headerW - headerWidgetsWidth(ws) + 18.0f * s;
    ImGui::PushID("##hdrwidgets");
    for (const PanelView* pv : ws) {
        const float lw = textSize(f.small, pv->title).x;
        drawText(dl, f.small, ImVec2(x, cy - F(f.small)->FontSize * 0.5f), U(pv->usable ? t.text2 : t.muted), pv->title);
        x += lw + 8.0f * s;
        const ImVec2 swSize(36.0f * s, 20.0f * s);
        bool hov = false;
        ImGui::PushID(pv->key.c_str());
        if (inlineSwitch("##wsw", ImVec2(x, cy - swSize.y * 0.5f), swSize, pv->onScreen, pv->usable, &hov) && actions.setWidgetShown)
            actions.setWidgetShown(plugin.id, pv->key, !pv->onScreen);
        ImGui::PopID();
        if (hov) ImGui::SetTooltip(pv->usable ? (pv->onScreen ? "Hide the widget." : "Show the widget on screen.") : "The mod is off.");
        x += swSize.x + 18.0f * s;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(back);
}

void drawPluginBody(const PluginView& plugin, const Actions& actions);

void drawPluginPage(const PluginView& plugin, const Actions& actions) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    std::string meta = plugin.version;
    if (!plugin.author.empty()) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + plugin.author;
    if (!plugin.statusText.empty()) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + plugin.statusText;
    const Rgba sc = statusColour(plugin.status);
    // the widget switches, when the header has room for them next to a readable title
    std::vector<const PanelView*> widgets = headerWidgets(plugin);
    const ImVec2 headerPos = ImGui::GetCursorScreenPos();
    const float headerW = ImGui::GetContentRegionAvail().x;
    while (!widgets.empty() && headerWidgetsWidth(widgets) > headerW - 220.0f * s) widgets.pop_back();
    pageHeader(plugin.id, plugin.name, false, statusLabel(plugin.status, false), &sc, meta, headerWidgetsWidth(widgets));
    drawHeaderWidgets(plugin, widgets, headerPos, headerW, actions);
    if (!plugin.description.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.text2));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(plugin.description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
    }
    (void)f;
    if (beginCard("##plugin_page", 20.0f, 16.0f, false, 240.0f * s)) drawPluginBody(plugin, actions);
    endCard();
}

// A group's page: one switch per feature, then the members' own pages as tabs.
void drawGroupPage(const Model& model, const GroupView& gv, const Actions& actions) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    const Rgba sc = statusColour(gv.enabled ? PluginStatus::On : PluginStatus::Off);
    pageHeader("group:" + gv.id, gv.name, false, gv.enabled ? "On" : "Off", &sc, "");
    if (!gv.description.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.text2));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(gv.description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
    }
    // the features, one switch each
    if (beginCard("##group_features", 20.0f, 12.0f, true)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (const GroupFeatureView& fv : gv.features) {
            ImGui::PushID(fv.label.c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            const float rowH = 34.0f * s;
            drawText(dl, f.body, ImVec2(p.x, p.y + (rowH - F(f.body)->FontSize) * 0.5f), U(fv.usable ? t.text : t.muted), fv.label);
            if (!fv.statusText.empty()) {
                const float lx = p.x + textSize(f.body, fv.label).x + 12.0f * s;
                drawText(dl, f.small, ImVec2(lx, p.y + (rowH - F(f.small)->FontSize) * 0.5f), U(t.muted),
                         ellipsize(f.small, fv.statusText, std::max(0.0f, p.x + w - 60.0f * s - lx)));
            }
            const ImVec2 swSize(36.0f * s, 20.0f * s);
            const ImVec2 swPos(p.x + w - swSize.x, p.y + (rowH - swSize.y) * 0.5f);
            if (inlineSwitch("##fsw", swPos, swSize, fv.enabled, fv.usable, nullptr)) {
                if (!fv.pluginId.empty() && actions.setPluginEnabled) actions.setPluginEnabled(fv.pluginId, !fv.enabled);
                else if (fv.hostFeature == "login-fix" && actions.setOnlineFix) actions.setOnlineFix(!fv.enabled);
            }
            ImGui::SetCursorScreenPos(p);
            ImGui::Dummy(ImVec2(w, rowH));
            ImGui::PopID();
        }
    }
    endCard();
    ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
    // the members' pages
    std::vector<const PluginView*> pages;
    for (const GroupFeatureView& fv : gv.features)
        if (const PluginView* pv = fv.pluginId.empty() ? nullptr : findPlugin(model, fv.pluginId))
            if (pv->hasMenu || !pv->panels.empty()) pages.push_back(pv);
    if (pages.empty()) return;
    if (beginCard("##group_pages", 20.0f, 16.0f, false, 200.0f * s)) {
        const ImVec2 room = ImGui::GetContentRegionAvail();
        g_metrics.pageW = room.x;
        g_metrics.pageH = room.y;
        if (ImGui::BeginTabBar("##members")) {
            for (const PluginView* pv : pages) {
                std::string label = pv->name;
                for (const GroupFeatureView& fv : gv.features)
                    if (fv.pluginId == pv->id) label = fv.label;
                const bool pick = g.tabPending && g.tabPlugin == pv->id;
                if (ImGui::BeginTabItem((label + "###m_" + pv->id).c_str(), nullptr, pick ? ImGuiTabItemFlags_SetSelected : 0)) {
                    if (pick && g.tabKey.empty()) g.tabPending = false;
                    ImGui::Spacing();
                    drawPluginBody(*pv, actions);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
    }
    endCard();
}

// The plugin's own page inside its card: its DrawMenu, or Settings + one tab per panel.
void drawPluginBody(const PluginView& plugin, const Actions& actions) {
    {
        const ImVec2 room = ImGui::GetContentRegionAvail();
        g_metrics.pageW = room.x;
        g_metrics.pageH = room.y;
        ImGui::PushID(plugin.id.c_str());
        // ui_metrics' page_w/page_h: the room right where the plugin draws (PageRoom)
        auto page = [&] {
            const ImVec2 a = ImGui::GetContentRegionAvail();
            const PageRoom r(a.x, a.y);
            actions.drawPluginPage(plugin.id);
        };
        if (plugin.panels.empty()) {
            if (actions.drawPluginPage) page();
        } else if (ImGui::BeginTabBar("##panels")) {
            const bool pick = g.tabPending && g.tabPlugin == plugin.id;
            // Settings (the plugin's DrawMenu) first, then one tab per panel (ABI minor 2)
            if (plugin.hasMenu && ImGui::BeginTabItem("Settings", nullptr, pick && g.tabKey.empty() ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (actions.drawPluginPage) page();
                ImGui::EndTabItem();
            }
            for (const PanelView& pv : plugin.panels) {
                const std::string label = pv.label + (pv.detached ? " (window)" : "") + "###tab_" + pv.key;
                if (ImGui::BeginTabItem(label.c_str(), nullptr, pick && g.tabKey == pv.key ? ImGuiTabItemFlags_SetSelected : 0)) {
                    ImGui::Spacing();
                    if (actions.drawPanelTab) {
                        const ImVec2 a = ImGui::GetContentRegionAvail();
                        const PageRoom r(a.x, a.y);
                        actions.drawPanelTab(plugin.id, pv.key);
                    }
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
            if (pick) g.tabPending = false;
        }
        ImGui::PopID();
    }
}

void drawOtherPage(const OtherModView& o) {
    const float s = scale();
    const Tokens& t = tokens();
    std::string meta = o.kind;
    pageHeader(o.file, o.name, true, nullptr, nullptr, meta);
    if (beginCard("##other_page", 24.0f, 20.0f, true)) {
        infoRow("Version", o.version);
        infoRow("File", o.file);
        infoRow("Kind", o.kind);
        infoRow("Hotkeys", o.keys);
        if (!o.note.empty()) infoRow("Note", o.note);
    }
    endCard();
    ImGui::Dummy(ImVec2(0.0f, 10.0f * s));
    ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.muted));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("Not controlled by Horizon.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// Finishes a running key capture. Called once per frame.
void pollCapture(const Model& model, const Actions& actions) {
    if (g.captureHandle <= 0) return;
    bool known = g.captureHandle == model.menuHotkey;
    for (const HotkeyView& h : model.hotkeys) known = known || h.handle == g.captureHandle;
    std::string key;
    if (!known) {
        g.captureHandle = 0;
    } else if (actions.pollKeyCapture && actions.pollKeyCapture(g.captureHandle, key)) {
        if (!key.empty() && actions.rebindHotkey && !actions.rebindHotkey(g.captureHandle, key)) {
            g.note = "Key not available";
            g.noteUntil = ImGui::GetTime() + 3.0;
        }
        g.captureHandle = 0;
        g.captureEnded = true;
    }
}

void startCapture(int handle, const Actions& actions) {
    g.captureHandle = handle;
    if (actions.beginKeyCapture) actions.beginKeyCapture(handle);
}

void drawGeneral(const Model& model, const Actions& actions) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f * s, 10.0f * s));
    if (beginCard("##general", 24.0f, 20.0f, false)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // top bar switch
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            drawText(dl, f.bodyBold, p, U(t.text), "Top bar");
            drawText(dl, f.small, ImVec2(p.x, p.y + F(f.bodyBold)->FontSize + 2.0f * s), U(t.muted),
                     "Top screen edge");
            const ImVec2 swSize(44.0f * s, 24.0f * s);
            const ImVec2 swPos(p.x + w - swSize.x, p.y + 8.0f * s);
            if (inlineSwitch("##bar", swPos, swSize, model.barEnabled, true, nullptr) && actions.setBarEnabled) {
                actions.setBarEnabled(!model.barEnabled);
            }
            ImGui::SetCursorScreenPos(p);
            ImGui::Dummy(ImVec2(w, 44.0f * s));
        }
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));

        // Shared setup: one setup for every game of this Windows user (docs/shared-setup.md)
        if (model.setupAvailable) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            drawText(dl, f.bodyBold, p, U(t.text), "Shared setup");
            std::string sub = model.setupShared ? "All games" : "This game only";
            if (model.setupRestart) sub += "  \xC2\xB7  Restart to apply";
            drawText(dl, f.small, ImVec2(p.x, p.y + F(f.bodyBold)->FontSize + 2.0f * s), U(model.setupRestart ? t.warn : t.muted), sub);
            const ImVec2 swSize(44.0f * s, 24.0f * s);
            const ImVec2 swPos(p.x + w - swSize.x, p.y + 8.0f * s);
            if (inlineSwitch("##sharedsetup", swPos, swSize, model.setupShared, actions.setSharedSetup != nullptr, nullptr) && actions.setSharedSetup)
                actions.setSharedSetup(!model.setupShared);
            ImGui::SetCursorScreenPos(p);
            ImGui::Dummy(ImVec2(w, 44.0f * s));
            if (ImGui::Button("Copy from this game") && actions.copySetupFromGame) actions.copySetupFromGame();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("This game's setup becomes the shared one.");
            ImGui::SameLine();
            if (ImGui::Button("Reset##sharedsetup")) ImGui::OpenPopup("##resetsetup");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shared setup back to defaults.");
            if (ImGui::BeginPopup("##resetsetup")) {
                ImGui::TextUnformatted("Reset shared setup?");
                if (ImGui::Button("Reset##confirm") && actions.resetSharedSetup) {
                    actions.resetSharedSetup();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##resetsetup")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Export") && actions.exportSetup) actions.exportSetup();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Saves the setup as a file.");
            ImGui::SameLine();
            if (ImGui::Button("Import")) ImGui::OpenPopup("##importsetup");
            if (ImGui::BeginPopup("##importsetup")) {
                if (model.setupFiles.empty()) ImGui::TextDisabled("No files");
                for (size_t i = 0; i < model.setupFiles.size() && i < 12; i++) {
                    if (ImGui::Selectable(model.setupFiles[i].c_str()) && actions.importSetup) actions.importSetup(model.setupFiles[i]);
                }
                ImGui::Separator();
                if (ImGui::Selectable("Open folder") && actions.openSetupFolder) actions.openSetupFolder();
                ImGui::EndPopup();
            }
            ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        }

        // Display: menu size and text size
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawText(dl, f.bodyBold, p, U(t.text), "Menu size");
            drawText(dl, f.small, ImVec2(p.x, p.y + F(f.bodyBold)->FontSize + 2.0f * s), U(t.muted), "Share of screen");
            ImGui::Dummy(ImVec2(1.0f, 44.0f * s));
            const std::string autoLabel = "Auto (" + std::to_string(model.menuSizeAuto) + "%)";
            const char* labels[5] = {autoLabel.c_str(), "60%", "70%", "80%", "90%"};
            const int values[5] = {0, 60, 70, 80, 90};
            int sel = 0;
            for (int i = 0; i < 5; ++i)
                if (values[i] == model.menuSizeSetting) sel = i;
            ImGui::PushFont(F(f.body));
            const int picked = segmented("menusize", labels, 5, sel, 34.0f * s, F(f.body));
            ImGui::PopFont();
            if (picked != sel && actions.setMenuSize) actions.setMenuSize(values[picked]);
        }
        {
            ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawText(dl, f.bodyBold, p, U(t.text), "Text and icons");
            drawText(dl, f.small, ImVec2(p.x, p.y + F(f.bodyBold)->FontSize + 2.0f * s), U(t.muted),
                     "Ctrl + wheel");
            ImGui::Dummy(ImVec2(1.0f, 44.0f * s));
            // applied when the slider is let go: the fonts are rebuilt at the new size
            int pct = g.scaleDrag > 0 ? g.scaleDrag : model.contentPct;
            ImGui::SetNextItemWidth(320.0f * s);
            ImGui::SliderInt("##contentscale", &pct, 80, 125, "%d%%");
            if (ImGui::IsItemActive()) g.scaleDrag = pct;
            if (ImGui::IsItemDeactivated()) {
                if (g.scaleDrag > 0 && actions.setContentScale) actions.setContentScale((g.scaleDrag + 2) / 5 * 5);
                g.scaleDrag = 0;
            }
            ImGui::SameLine();
            const bool isAuto = model.contentSetting == 0;
            ImGui::BeginDisabled(isAuto);
            if (ImGui::Button(("Auto (" + std::to_string(model.contentAuto) + "%)").c_str()) && actions.setContentScale) actions.setContentScale(0);
            ImGui::EndDisabled();
        }
        {
            ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawText(dl, f.bodyBold, p, U(t.text), "Module icons");
            drawText(dl, f.small, ImVec2(p.x, p.y + F(f.bodyBold)->FontSize + 2.0f * s), U(t.muted), "In-game only");
            ImGui::Dummy(ImVec2(1.0f, 44.0f * s));
            const char* styles[2] = {"Distinct", "Saturn"};
            ImGui::PushFont(F(f.body));
            const int picked = segmented("iconstyle", styles, 2, model.iconStyle, 34.0f * s, F(f.body));
            ImGui::PopFont();
            if (picked != model.iconStyle && actions.setIconStyle) actions.setIconStyle(picked);
        }
        {
            ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawText(dl, f.bodyBold, p, U(t.text), "Menu background");
            drawText(dl, f.small, ImVec2(p.x, p.y + F(f.bodyBold)->FontSize + 2.0f * s), U(t.muted), "Opacity");
            ImGui::Dummy(ImVec2(1.0f, 44.0f * s));
            int op = std::max(50, std::min(100, model.menuOpacity));
            ImGui::SetNextItemWidth(std::min(320.0f * s, ImGui::GetContentRegionAvail().x));
            if (ImGui::SliderInt("##menuopacity", &op, 50, 100, "%d%%") && actions.setMenuOpacity) actions.setMenuOpacity(op);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lower shows more of the game.");
        }
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));

        // layout mode
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            drawText(dl, f.bodyBold, p, U(t.text), "Widget layout");
            std::string sub = model.layoutKey.empty() ? std::string("No key") : "Key: " + model.layoutKey;
            if (model.forever) sub += "  \xC2\xB7  game menus only";
            drawText(dl, f.small, ImVec2(p.x, p.y + F(f.bodyBold)->FontSize + 2.0f * s), U(t.muted), sub);
            const char* label = "Edit layout";
            const float bw = textSize(f.body, label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw, p.y + 4.0f * s));
            ImGui::BeginDisabled(!model.layoutAllowed);
            if (ImGui::Button(label) && actions.toggleLayout) actions.toggleLayout();
            ImGui::EndDisabled();
            if (!model.layoutAllowed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", model.layoutWhy.c_str());
            ImGui::SetCursorScreenPos(p);
            ImGui::Dummy(ImVec2(w, 44.0f * s));
            int op = model.widgetOpacity;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Widget background");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(240.0f * s);
            if (ImGui::SliderInt("##widgetbg", &op, 20, 100, "%d%%") && actions.setWidgetOpacity) actions.setWidgetOpacity(op);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lower shows more of the game.");
        }
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));

        // online login fix (ESWC and Original)
        bool loginFixGrouped = false; // shown in its group (Legacy Essentials) instead
        for (const GroupView& gv : model.groups)
            for (const GroupFeatureView& fv : gv.features) loginFixGrouped = loginFixGrouped || fv.hostFeature == "login-fix";
        if (model.onlineFix >= 0 && !loginFixGrouped) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            drawText(dl, f.bodyBold, p, U(t.text), "Login-Fix (Fehler 12157)");
            drawText(dl, f.small, ImVec2(p.x, p.y + F(f.bodyBold)->FontSize + 2.0f * s), U(t.muted),
                     "Windows 10/11, next start");
            const ImVec2 swSize(44.0f * s, 24.0f * s);
            const ImVec2 swPos(p.x + w - swSize.x, p.y + 8.0f * s);
            if (inlineSwitch("##onlinefix", swPos, swSize, model.onlineFix == 1, true, nullptr) && actions.setOnlineFix) {
                actions.setOnlineFix(model.onlineFix != 1);
            }
            ImGui::SetCursorScreenPos(p);
            ImGui::Dummy(ImVec2(w, 44.0f * s));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        }

        // notifications and the TM Horizon connection (ABI minor 3)
        auto switchRow = [&](const char* id, const char* title, const std::string& sub, bool on, const std::function<void(bool)>& set) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            drawText(dl, f.bodyBold, p, U(t.text), title);
            drawText(dl, f.small, ImVec2(p.x, p.y + F(f.bodyBold)->FontSize + 2.0f * s), U(t.muted), sub);
            const ImVec2 swSize(44.0f * s, 24.0f * s);
            const ImVec2 swPos(p.x + w - swSize.x, p.y + 8.0f * s);
            if (inlineSwitch(id, swPos, swSize, on, set != nullptr, nullptr) && set) set(!on);
            ImGui::SetCursorScreenPos(p);
            ImGui::Dummy(ImVec2(w, 44.0f * s));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        };
        switchRow("##dnd", "Do not disturb", "List only", model.dnd, actions.setDnd);
        switchRow("##holdtoasts", "Quiet while driving", "After the run", model.holdToasts, actions.setHoldToasts);
        switchRow("##bridge", "TM Horizon connection",
                  model.bridgeEnabled ? (model.bridgeText.empty() ? std::string("On") : model.bridgeText)
                                      : std::string("Off"),
                  model.bridgeEnabled, actions.setBridgeEnabled);
        if (model.bridgePaired && actions.forgetBridge) {
            if (ImGui::Button("Forget pairing")) actions.forgetBridge();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("TM Horizon asks for Allow again.");
            ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        }
        if (!model.recentToasts.empty()) {
            dim("Recent notifications", true);
            for (const Model::Recent& r : model.recentToasts) {
                const Rgba& c = r.kind == 1 ? t.ok : r.kind == 2 ? t.warn : r.kind == 3 ? t.bad : t.accent;
                ImGui::PushStyleColor(ImGuiCol_Text, toVec(c));
                ImGui::TextUnformatted(r.kind == 3 ? HZ_ICON_CIRCLE_ALERT_U8 : r.kind == 2 ? HZ_ICON_TRIANGLE_ALERT_U8 : r.kind == 1 ? HZ_ICON_CHECK_U8 : HZ_ICON_INFO_U8);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                std::string line = r.title;
                if (!r.text.empty()) line += "  " + r.text;
                ImGui::PushStyleColor(ImGuiCol_Text, toVec(r.missed ? t.text2 : t.muted));
                ImGui::TextUnformatted(line.c_str());
                ImGui::PopStyleColor();
                if (!r.owner.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", r.owner.c_str());
                }
            }
            ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        }

        // menu key
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawText(dl, f.bodyBold, p, U(t.text), "Menu key");
            drawText(dl, f.small, ImVec2(p.x, p.y + F(f.bodyBold)->FontSize + 2.0f * s), U(t.muted),
                     "Open / close");
            ImGui::Dummy(ImVec2(1.0f, 44.0f * s));
            const bool capturing = model.menuHotkey > 0 && g.captureHandle == model.menuHotkey;
            ImGui::AlignTextToFramePadding();
            ImGui::PushFont(F(f.bodyBold));
            ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.accent2));
            ImGui::TextUnformatted(model.menuKey.empty() ? "None" : model.menuKey.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::SameLine(0.0f, 20.0f * s);
            if (capturing) {
                ImGui::PushStyleColor(ImGuiCol_Button, toVec(t.accent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec(t.accent2));
                ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.accentInk));
            }
            if (ImGui::Button(capturing ? "Press a key (Esc cancels)" : "Change") && !capturing && model.menuHotkey > 0) {
                startCapture(model.menuHotkey, actions);
            }
            if (capturing) ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            if (!model.menuKeySuggestions.empty()) {
                ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
                dim("Quick picks", true);
                const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                bool first = true;
                for (const std::string& sug : model.menuKeySuggestions) {
                    const float bw = textSize(f.body, sug).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                    if (!first) {
                        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + bw <= right) ImGui::SameLine();
                    }
                    first = false;
                    const bool current = sug == model.menuKey;
                    if (current) {
                        ImGui::PushStyleColor(ImGuiCol_Button, toVec(t.accent, 0.22f));
                        ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.accent2));
                    }
                    ImGui::PushID(sug.c_str());
                    const bool picked = ImGui::Button(sug.c_str(), ImVec2(std::max(bw, 52.0f * s), 0.0f));
                    ImGui::PopID();
                    if (picked && model.menuHotkey > 0 && actions.rebindHotkey) {
                        if (!actions.rebindHotkey(model.menuHotkey, sug)) {
                            g.note = "Key not available";
                            g.noteUntil = ImGui::GetTime() + 3.0;
                        }
                    }
                    if (current) ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }
            }
            if (!g.note.empty() && ImGui::GetTime() < g.noteUntil) {
                ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.warn));
                ImGui::TextUnformatted(g.note.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        infoRow("Safe mode", model.safeMode ? "On (no mods)" : "Off");
        infoRow("Profile", model.profileName.empty() ? "none" : model.profileName);
        infoRow("Game state", model.gameState);
    }
    endCard();
    ImGui::PopStyleVar();
}

void drawHotkeys(const Model& model, const Actions& actions) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();

    dim("Click a key to change", true);
    ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
    if (model.hotkeys.empty()) {
        dim("No hotkeys", false);
        return;
    }
    const float noteH = 28.0f * s;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX |
                                  ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##hotkeys", 4, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y - noteH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 230.0f * s);
        ImGui::TableSetupColumn("Conflict", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.muted));
        ImGui::PushFont(F(f.small));
        ImGui::TableHeadersRow();
        ImGui::PopFont();
        ImGui::PopStyleColor();
        for (const HotkeyView& h : model.hotkeys) {
            ImGui::TableNextRow();
            ImGui::PushID(h.handle);
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.text2));
            ImGui::TextUnformatted(h.ownerName.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(h.name.c_str());
            ImGui::TableSetColumnIndex(2);
            const bool capturing = g.captureHandle == h.handle;
            if (capturing) {
                ImGui::PushStyleColor(ImGuiCol_Button, toVec(t.accent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec(t.accent2));
                ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.accentInk));
            }
            const std::string label = capturing ? "Press a key (Esc cancels)" : (h.key.empty() ? "None" : h.key);
            if (!capturing && h.key.empty()) ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.muted));
            if (ImGui::Button((label + "##key").c_str(), ImVec2(-FLT_MIN, 0.0f)) && !capturing) startCapture(h.handle, actions);
            if (!capturing && h.key.empty()) ImGui::PopStyleColor();
            if (capturing) ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::TableSetColumnIndex(3);
            if (!h.conflict.empty()) {
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.warn));
                ImGui::TextWrapped("%s", h.conflict.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!g.note.empty() && ImGui::GetTime() < g.noteUntil) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.warn));
        ImGui::TextUnformatted(g.note.c_str());
        ImGui::PopStyleColor();
    }
}

void drawLog(const Model& model, const Actions& actions) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();

    const char* filters[2] = {"All", "Problems"};
    ImGui::PushFont(F(f.body));
    g.logFilter = segmented("logfilter", filters, 2, g.logFilter, 34.0f * s, F(f.body));
    ImGui::PopFont();
    ImGui::SameLine();
    const float btnW = textSize(f.body, "Open folder").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float btnX = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(btnX + std::max(0.0f, ImGui::GetContentRegionAvail().x - btnW));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (34.0f * s - ImGui::GetFrameHeight()) * 0.5f);
    if (ImGui::Button("Open folder") && actions.openLogFolder) actions.openLogFolder();
    ImGui::Dummy(ImVec2(0.0f, 2.0f * s));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec(Rgba{0.035f, 0.043f, 0.059f, 0.9f}));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, t.radius * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * s, 12.0f * s));
    const bool shown = ImGui::BeginChild("##log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
                                         ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleVar(2);
    if (shown) {
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        ImGui::PushFont(F(f.mono));
        std::vector<int> idx;
        if (model.log) {
            for (size_t i = 0; i < model.log->size(); ++i) {
                if (g.logFilter == 0 || (*model.log)[i].level >= 2) idx.push_back(static_cast<int>(i));
            }
        }
        if (idx.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.muted));
            ImGui::TextUnformatted(g.logFilter == 1 && model.log && !model.log->empty() ? "No problems" : "Empty");
            ImGui::PopStyleColor();
        } else {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(idx.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const LogLine& line = (*model.log)[static_cast<size_t>(idx[static_cast<size_t>(row)])];
                    Rgba c = t.text2;
                    if (line.level <= 0) c = t.muted;
                    if (line.level == 2) c = t.warn;
                    if (line.level >= 3) c = t.bad;
                    ImGui::PushStyleColor(ImGuiCol_Text, toVec(c));
                    ImGui::TextUnformatted(line.text.c_str());
                    ImGui::PopStyleColor();
                }
            }
            if (atBottom) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void drawAbout(const Model& model) {
    const float s = scale();
    const Tokens& t = tokens();
    if (beginCard("##about", 24.0f, 20.0f, true)) {
        infoRow("Horizon Client", model.hostVersion);
        infoRow("Game", model.gameName);
        infoRow("Game version", model.exeVersion);
        infoRow("Profile", model.profileName.empty() ? "none" : model.profileName);
        infoRow("Menu key", model.menuKey);
        infoRow("Log file", model.logPath);
        if (!model.notes.empty()) {
            ImGui::Dummy(ImVec2(0.0f, 8.0f * s));
            dim("Notes", true);
            for (const std::string& n : model.notes) {
                ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.warn));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::Bullet();
                ImGui::TextUnformatted(n.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
        }
        ImGui::Dummy(ImVec2(0.0f, 12.0f * s));
        dim("Dear ImGui (MIT, Omar Cornut)", true);
    }
    endCard();
}

void drawFooter(const Model& model, float height) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + width, p.y), W(0.07f), 1.0f);
    const float ty = p.y + (height - F(f.small)->FontSize) * 0.5f + 2.0f * s;
    const std::string key = model.menuKey.empty() ? std::string("Esc") : model.menuKey + " or Esc";
    const std::string right = "Horizon Client " + model.hostVersion;
    const float rightW = textSize(f.small, right).x;
    const float leftW = width - rightW - 16.0f * s;
    std::string left = key + " closes  \xC2\xB7  Ctrl + wheel: text size " + std::to_string(model.contentPct) + "%";
    if (textSize(f.small, left).x > leftW) left = key + " closes";
    if (textSize(f.small, left).x <= leftW) drawText(dl, f.small, ImVec2(p.x, ty), U(t.muted), left);
    drawText(dl, f.small, ImVec2(p.x + width - rightW, ty), U(t.muted), right);
    ImGui::Dummy(ImVec2(width, height));
}

// Makes sure sel points at something that exists.
void resolveSelection(const Model& model) {
    if (g.sel.empty() || g.sel == "mods") {
        if (!model.groups.empty()) g.sel = "group:" + model.groups[0].id;
        else g.sel = model.plugins.empty() ? "general" : "plugin:" + model.plugins[0].id;
    }
    // a module shown inside a group: the group's page, on that module's tab
    if (startsWith(g.sel, "plugin:")) {
        if (const GroupView* gv = groupOfPlugin(model, g.sel.substr(7))) {
            if (!(g.tabPending && g.tabPlugin == g.sel.substr(7))) {
                g.tabPlugin = g.sel.substr(7);
                g.tabKey.clear();
                g.tabPending = true;
            }
            g.sel = "group:" + gv->id;
        }
    }
    if (startsWith(g.sel, "group:") && !findGroup(model, g.sel.substr(6))) g.sel = "general";
    if (startsWith(g.sel, "plugin:") && !findPlugin(model, g.sel.substr(7))) g.sel = "general";
    if (startsWith(g.sel, "other:") && !findOther(model, g.sel.substr(6))) g.sel = "general";
}

void drawPage(const Model& model, const Actions& actions, float height) {
    const float s = scale();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool scrolls = g.sel == "general" || g.sel == "about" || startsWith(g.sel, "other:") || height < 360.0f * s;
    ImGui::BeginChild("##page", ImVec2(0.0f, height), 0, scrolls ? 0 : ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    (void)s;
    if (startsWith(g.sel, "group:")) {
        if (const GroupView* gv = findGroup(model, g.sel.substr(6))) drawGroupPage(model, *gv, actions);
    } else if (startsWith(g.sel, "plugin:")) {
        if (const PluginView* p = findPlugin(model, g.sel.substr(7))) drawPluginPage(*p, actions);
    } else if (startsWith(g.sel, "other:")) {
        if (const OtherModView* o = findOther(model, g.sel.substr(6))) drawOtherPage(*o);
    } else if (g.sel == "hotkeys") {
        drawHotkeys(model, actions);
    } else if (g.sel == "log") {
        drawLog(model, actions);
    } else if (g.sel == "about") {
        drawAbout(model);
    } else {
        drawGeneral(model, actions);
    }
    ImGui::EndChild();
}

} // namespace

void showPluginPage(const std::string& id) {
    g.sel = "plugin:" + id;
    g.narrowPage = true;
}

bool pluginPageShown(const std::string& id) {
    return g.sel == "plugin:" + id;
}

void selectPanelTab(const std::string& pluginId, const std::string& key) {
    g.tabPlugin = pluginId;
    g.tabKey = key;
    g.tabPending = true;
}

void showPage(const char* page) {
    if (!page) return;
    g.sel = page;
    g.narrowPage = std::strcmp(page, "mods") != 0;
}

void drawMenu(const Model& model, const Actions& actions, bool* open) {
    if (!open) return;
    const float s = scale();
    const Tokens& t = tokens();
    ImGuiIO& io = ImGui::GetIO();
    const int frame = ImGui::GetFrameCount();
    if (frame > g.lastFrame + 1) { // just opened
        g.openedAt = ImGui::GetTime();
        g.captureHandle = 0;
    }
    g.lastFrame = frame;
    const bool captureAtStart = g.captureHandle > 0;
    g.captureEnded = false;
    const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    resolveSelection(model);
    pollCapture(model, actions);

    const float appear = clamp01(static_cast<float>((ImGui::GetTime() - g.openedAt) / 0.14));
    const float eased = 1.0f - (1.0f - appear) * (1.0f - appear);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, eased);

    // dim layer: only the game behind. It goes into the background draw list, which is under every window
    // (a window for it ended up above the panel and darkened the menu itself). The style alpha does not
    // reach a draw list, so the fade-in is applied by hand.
    // A lower menu opacity (General) also lightens the sidebar and the dim layer, else the game would still be hidden.
    const int menuOpacity = std::max(50, std::min(100, model.menuOpacity));
    g.menuAlpha = std::min(1.0f, static_cast<float>(menuOpacity) / 94.0f);
    const ImVec2 disp = io.DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), disp, U(Rgba{0.0f, 0.0f, 0.0f, 1.0f}, 0.55f * g.menuAlpha * eased));

    // panel
    // the player's menu size: a share of the screen (General -> Display)
    const float share = menuShare(model.menuSizePct, disp.x, disp.y);
    const float pw = std::floor(disp.x * share);
    const float ph = std::floor(disp.y * share);
    g_metrics.menuW = pw;
    g_metrics.menuH = ph;
    const ImVec2 pos(std::floor((disp.x - pw) * 0.5f), std::floor((disp.y - ph) * 0.5f + (1.0f - eased) * 12.0f * s));
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImVec4 panelBg = toVec(t.bg);
    panelBg.w = static_cast<float>(menuOpacity) / 100.0f; // default 94 %: very slightly translucent; text and controls stay opaque
    ImGui::PushStyleColor(ImGuiCol_WindowBg, panelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radiusLg * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f * s, 22.0f * s));
    const ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoScrollWithMouse |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus; // popped-out panel windows stay above it
    ImGui::Begin("##horizon_panel", nullptr, panelFlags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    // where the header row ends once the fade-in is done (computed: while the alpha is 0 ImGui skips items)
    g_metrics.headerBottom = ImGui::GetCursorScreenPos().y - (1.0f - eased) * 12.0f * s + F(fonts().title)->FontSize +
                             F(fonts().small)->FontSize + 6.0f * s;
    g_metrics.headerFrame = frame;
    drawHeader(model, open);
    ImGui::Dummy(ImVec2(0.0f, 12.0f * s));

    const float footerH = 36.0f * s;
    const float contentH = ImGui::GetContentRegionAvail().y - footerH;
    const float contentW = ImGui::GetContentRegionAvail().x;
    const float gap = 24.0f * s;
    const float sideW = 260.0f * s;
    // Narrow (small window): list or page, one at a time; a click on a row opens its page, Back returns.
    const bool narrow = contentW < sideW + gap + kMinPageWidth * s;
    if (narrow != g.narrow) g.narrowPage = false; // start on the list after a layout change
    g.narrow = narrow;
    if (!narrow) {
        drawSidebar(model, actions, sideW, contentH);
        ImGui::SameLine(0.0f, gap);
        drawPage(model, actions, contentH);
    } else if (!g.narrowPage) {
        g.rowClicked = false;
        drawSidebar(model, actions, contentW, contentH);
        if (g.rowClicked) g.narrowPage = true;
    } else {
        const float backH = 30.0f * s;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * s, 4.0f * s));
        if (ImGui::Button(HZ_ICON_CHEVRON_LEFT_U8 " Back", ImVec2(0.0f, backH)) ) g.narrowPage = false;
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
        drawPage(model, actions, std::max(1.0f, contentH - backH - ImGui::GetStyle().ItemSpacing.y - 4.0f * s));
    }
    drawFooter(model, footerH);

    const ImVec2 wpos = ImGui::GetWindowPos();
    const ImVec2 wsize = ImGui::GetWindowSize();
    ImGui::End();
    ImGui::PopStyleVar(); // alpha

    // close: Esc (not while a key is being captured) or a click on the dim layer
    const bool captureBusy = captureAtStart || g.captureHandle > 0 || g.captureEnded;
    if (!captureBusy && !popupOpen && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) *open = false;
    // Only a click on the dim layer itself closes: a widget, a popped-out panel, a free plugin window or a
    // plugin's overlay window above it keeps the menu open (they are windows; the dim layer is not).
    if (!popupOpen && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseHoveringRect(wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y), false) && !pointerOverAnyWindow()) {
        *open = false;
    }
}

const MenuMetrics& menuMetrics() { return g_metrics; }

PageRoom::PageRoom(float w, float h) : w0_(g_metrics.pageW), h0_(g_metrics.pageH) {
    g_metrics.pageW = std::max(0.0f, w);
    g_metrics.pageH = std::max(0.0f, h);
}
PageRoom::~PageRoom() {
    g_metrics.pageW = w0_;
    g_metrics.pageH = h0_;
}

float windowTop(float displayH) {
    const float s = scale();
    float top = 56.0f * s; // the top bar (52 px at scale 1) stays clickable
    // and the menu's header row, while the menu is open (drawn this or the last frame)
    if (g_metrics.headerBottom > 0.0f && ImGui::GetFrameCount() - g_metrics.headerFrame <= 1)
        top = std::max(top, g_metrics.headerBottom + 4.0f * s);
    return std::floor(std::min(top, displayH * 0.25f));
}

WinRect fitWindow(float x, float y, float w, float h, float minW, float minH, float displayW, float displayH, int index) {
    const float s = scale();
    const float top = windowTop(displayH);
    const float roomW = std::max(1.0f, displayW), roomH = std::max(1.0f, displayH - top);
    WinRect r;
    r.minW = std::min(minW, roomW);
    r.minH = std::min(minH, roomH);
    r.w = std::floor(std::min(std::max(w, r.minW), roomW));
    r.h = std::floor(std::min(std::max(h, r.minH), roomH));
    const float shift = 24.0f * s * static_cast<float>(index);
    r.x = x >= 0.0f ? x : std::floor((displayW - r.w) * 0.5f + shift);
    r.y = y >= 0.0f ? y : std::floor(top + (roomH - r.h) * 0.35f + shift);
    r.x = std::min(std::max(r.x, 0.0f), std::max(0.0f, displayW - r.w));
    r.y = std::min(std::max(r.y, top), std::max(top, displayH - r.h));
    return r;
}

float menuShare(int menuSizePct, float displayW, float displayH) {
    // small windows (TMUF at 640x480, ESWC at 800x600) get nearly the whole window
    if (displayW < 1000.0f || displayH < 700.0f) return 0.96f;
    return static_cast<float>(std::max(50, std::min(95, menuSizePct))) / 100.0f;
}

float fitScale(float userScale, float displayW, float displayH, int menuSizePct) {
    if (displayW <= 0.0f || displayH <= 0.0f) return userScale;
    const float share = menuShare(menuSizePct, displayW, displayH);
    // the menu at scale 1 wants about kFitWidth x kFitHeight (header, list, a page beside it, footer)
    float fit = std::min(displayW * share / kFitWidth, displayH * share / kFitHeight);
    fit = std::floor(fit * 20.0f) / 20.0f; // steps of 0.05: a window drag does not rebuild the fonts every pixel
    float s = std::min(userScale, fit);
    s = std::max(s, std::min(kMinUiScale, userScale)); // readable minimum; below it the menu scrolls
    return s;
}

// ---- free plugin windows ---------------------------------------------------------------------------

namespace {
struct PluginWin {
    bool open = false;
    bool place = true;       // apply the rect when drawn next
    bool known = false;      // rect loaded from the model once
    float x = -1, y = -1, w = 0, h = 0;
    float saveX = -1, saveY = -1, saveW = 0, saveH = 0;
    ImVec2 screen = ImVec2(0, 0);
};
std::vector<std::pair<std::string, PluginWin>> g_pwins;

PluginWin& pwin(const std::string& id) {
    for (auto& e : g_pwins)
        if (e.first == id) return e.second;
    g_pwins.emplace_back(id, PluginWin());
    return g_pwins.back().second;
}

// The plugin's page without the menu frame: switch, then Settings and one tab per panel.
void drawWindowBody(const PluginView& p, const Actions& actions) {
    const float s = scale();
    if (!p.statusText.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toVec(statusColour(p.status)));
        ImGui::TextUnformatted(p.statusText.c_str());
        ImGui::PopStyleColor();
    }
    if (canToggle(p.status) && actions.setPluginEnabled) {
        bool on = p.enabled;
        if (ImGui::Checkbox("On", &on)) actions.setPluginEnabled(p.id, on);
        if (ImGui::IsItemHovered() && p.restartToSwitch)
            ImGui::SetTooltip("%s", p.restartText.empty() ? "Applies after a restart." : p.restartText.c_str());
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
    // ui_metrics reports this window's room, not the menu page's (plugins switch to a compact layout)
    auto page = [&] {
        const ImVec2 a = ImGui::GetContentRegionAvail();
        const PageRoom r(a.x, a.y);
        actions.drawPluginPage(p.id);
    };
    if (p.panels.empty()) {
        if (actions.drawPluginPage) page();
    } else if (ImGui::BeginTabBar("##panels")) {
        if (p.hasMenu && ImGui::BeginTabItem("Settings")) {
            if (actions.drawPluginPage) page();
            ImGui::EndTabItem();
        }
        for (const PanelView& pv : p.panels) {
            const std::string label = pv.label + "###wtab_" + pv.key;
            if (ImGui::BeginTabItem(label.c_str())) {
                ImGui::Spacing();
                if (actions.drawPanelTab) {
                    const ImVec2 a = ImGui::GetContentRegionAvail();
                    const PageRoom r(a.x, a.y);
                    actions.drawPanelTab(p.id, pv.key);
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}
} // namespace

void togglePluginWindow(const std::string& pluginId) {
    PluginWin& w = pwin(pluginId);
    w.open = !w.open;
    if (w.open) w.place = true;
}

bool pluginWindowOpen(const std::string& pluginId) {
    for (auto& e : g_pwins)
        if (e.first == pluginId) return e.second.open;
    return false;
}

bool anyPluginWindowOpen() {
    for (auto& e : g_pwins)
        if (e.second.open) return true;
    return false;
}

bool drawPluginWindows(const Model& model, const Actions& actions) {
    const float s = scale();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    bool any = false;
    for (auto& e : g_pwins) {
        PluginWin& w = e.second;
        if (!w.open) continue;
        const PluginView* pv = nullptr;
        for (const PluginView& p : model.plugins)
            if (p.id == e.first) pv = &p;
        if (!pv) {
            w.open = false;
            continue;
        }
        if (!w.known) {
            w.known = true;
            if (pv->winX >= 0.0f && pv->winW > 0.0f) {
                w.x = pv->winX;
                w.y = pv->winY;
                w.w = pv->winW;
                w.h = pv->winH;
            }
        }
        if (screen.x != w.screen.x || screen.y != w.screen.y) {
            w.screen = screen;
            w.place = true; // display size changed (Reset): clamp again
        }
        // plugin.json "ui" (logical px) times the UI scale; the saved size wins; clamped below the bar and header
        const WinRect fit = fitWindow(w.x, w.y, w.w > 0.0f ? w.w : pv->prefW * s, w.h > 0.0f ? w.h : pv->prefH * s, pv->minW * s,
                                      pv->minH * s, screen.x, screen.y, static_cast<int>(&e - &g_pwins[0]));
        if (w.place) {
            w.place = false;
            w.x = fit.x;
            w.y = fit.y;
            w.w = fit.w;
            w.h = fit.h;
            ImGui::SetNextWindowPos(ImVec2(fit.x, fit.y), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(fit.w, fit.h), ImGuiCond_Always);
        }
        ImVec4 bg = toVec(tokens().bg);
        bg.w = static_cast<float>(std::max(50, std::min(100, model.menuOpacity))) / 100.0f; // General: menu background
        ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, tokens().radius * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * s, 14.0f * s));
        const std::string title = pv->name + "###hzwin." + e.first;
        bool open = true;
        ImGui::SetNextWindowSizeConstraints(ImVec2(fit.minW, fit.minH), ImVec2(FLT_MAX, FLT_MAX));
        // smaller than its content: scrolls both ways instead of clipping
        const ImGuiWindowFlags wf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_HorizontalScrollbar;
        if (ImGui::Begin(title.c_str(), &open, wf)) {
            ImGui::PushID(e.first.c_str());
            drawWindowBody(*pv, actions);
            ImGui::PopID();
            const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                (std::fabs(pos.x - w.saveX) > 0.5f || std::fabs(pos.y - w.saveY) > 0.5f || std::fabs(size.x - w.saveW) > 0.5f ||
                 std::fabs(size.y - w.saveH) > 0.5f)) {
                w.x = w.saveX = pos.x;
                w.y = w.saveY = pos.y;
                w.w = w.saveW = size.x;
                w.h = w.saveH = size.y;
                if (actions.saveWindow) actions.saveWindow(e.first, pos.x, pos.y, size.x, size.y);
            }
            any = true;
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        if (!open) w.open = false;
    }
    return any;
}

} // namespace ui
} // namespace horizon
