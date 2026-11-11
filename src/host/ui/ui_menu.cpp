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
};
MenuState g;

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

const PluginView* findPlugin(const Model& m, const std::string& id) {
    for (const PluginView& p : m.plugins) {
        if (p.id == id) return &p;
    }
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
    if (const unsigned long long logo = logoWordmark(&aspect)) {
        // the TM Horizon wordmark (planet + chrome TM + HORIZON), as tall as the title line
        const float lh = titleH * 1.15f;
        dl->AddImage(static_cast<ImTextureID>(logo), ImVec2(p.x, p.y - titleH * 0.08f), ImVec2(p.x + lh * aspect, p.y - titleH * 0.08f + lh));
    } else {
        const std::string tm = "TM ";
        drawText(dl, f.title, p, U(t.text), tm);
        drawText(dl, f.title, ImVec2(p.x + textSize(f.title, tm).x, p.y), U(t.accent), "HORIZON");
    }
    std::string sub = model.gameName;
    if (!model.exeVersion.empty()) sub += (sub.empty() ? "" : "  \xC2\xB7  ") + model.exeVersion;
    drawText(dl, f.small, ImVec2(p.x + 1.0f * s, p.y + titleH + 4.0f * s), U(t.muted), sub);

    const float closeD = 36.0f * s;
    const float rightX = p.x + width;
    const float midY = p.y + height * 0.5f;
    if (closeButton(ImVec2(rightX - closeD, midY - closeD * 0.5f), closeD)) *open = false;

    struct Chip {
        std::string text;
        Rgba colour;
        bool dot;
    };
    std::vector<Chip> chips;
    chips.push_back({model.online ? "Online" : "Offline", model.online ? t.ok : t.muted, true});
    if (model.safeMode) chips.push_back({"Safe mode", t.warn, false});
    if (model.runGate) chips.push_back({"Run check", t.warn, false});
    const float chipH = 28.0f * s;
    float x = rightX - closeD - 16.0f * s;
    for (size_t i = chips.size(); i-- > 0;) {
        x -= chipWidth(f.small, chips[i].text, chips[i].dot);
        drawChip(dl, ImVec2(x, midY - chipH * 0.5f), chipH, f.small, chips[i].text, chips[i].colour, chips[i].dot);
        x -= 8.0f * s;
    }
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
    if (swHov && canToggle(p.status) && !p.liveToggle) {
        ImGui::SetTooltip("Applies after a restart.");
    } else if (hov && !swHov && !p.statusText.empty()) {
        ImGui::SetTooltip("%s", p.statusText.c_str());
    }
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

void drawSidebar(const Model& model, const Actions& actions, float height) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec(t.bg2));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, t.radius * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s, 6.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    const bool shown = ImGui::BeginChild("##sidebar", ImVec2(260.0f * s, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar(3);
    if (shown) {
        sectionLabel("MODS");
        if (model.plugins.empty()) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x - 20.0f * s;
            const std::string msg = model.safeMode ? "Safe mode: no mods loaded. Start without Shift to load them."
                                                   : "No mods installed.";
            const std::vector<std::string> lines = wrapLines(f.small, msg, w, 4);
            const float lh = F(f.small)->FontSize + 3.0f * s;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (size_t i = 0; i < lines.size(); ++i) {
                drawText(dl, f.small, ImVec2(p.x + 10.0f * s, p.y + 2.0f * s + static_cast<float>(i) * lh), U(t.text2), lines[i]);
            }
            ImGui::Dummy(ImVec2(1.0f, static_cast<float>(lines.size()) * lh + 10.0f * s));
        }
        for (const PluginView& p : model.plugins) pluginRow(p, actions);

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

bool beginCard(const char* id, float padX, float padY, bool autoHeight) {
    const float s = scale();
    const Tokens& t = tokens();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec(t.surface, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, t.radius * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX * s, padY * s));
    const bool shown = ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
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
void pageHeader(const std::string& id, const std::string& name, bool greyIcon, const char* chipText, const Rgba* chipColour,
                const std::string& meta) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
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
    drawText(dl, f.title, ImVec2(tx, top), U(t.text), name);
    if (chipText) {
        const float chipH = 26.0f * s;
        drawChip(dl, ImVec2(tx + textSize(f.title, name).x + 16.0f * s, top + (titleH - chipH) * 0.5f + 2.0f * s), chipH,
                 f.small, chipText, *chipColour, false);
    }
    drawText(dl, f.small, ImVec2(tx + 1.0f * s, top + titleH + 2.0f * s), U(t.muted), meta);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, iconR * 2.0f + 14.0f * s));
}

void drawPluginPage(const PluginView& plugin, const Actions& actions) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    std::string meta = plugin.version;
    if (!plugin.author.empty()) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + plugin.author;
    if (!plugin.statusText.empty()) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + plugin.statusText;
    const Rgba sc = statusColour(plugin.status);
    pageHeader(plugin.id, plugin.name, false, statusLabel(plugin.status, false), &sc, meta);
    if (!plugin.description.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.text2));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(plugin.description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 6.0f * s));
    }
    (void)f;
    if (beginCard("##plugin_page", 20.0f, 16.0f, false)) {
        ImGui::PushID(plugin.id.c_str());
        if (plugin.panels.empty()) {
            if (actions.drawPluginPage) actions.drawPluginPage(plugin.id);
        } else if (ImGui::BeginTabBar("##panels")) {
            const bool pick = g.tabPending && g.tabPlugin == plugin.id;
            // Settings (the plugin's DrawMenu) first, then one tab per panel (ABI minor 2)
            if (plugin.hasMenu && ImGui::BeginTabItem("Settings", nullptr, pick && g.tabKey.empty() ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (actions.drawPluginPage) actions.drawPluginPage(plugin.id);
                ImGui::EndTabItem();
            }
            for (const PanelView& pv : plugin.panels) {
                const std::string label = pv.label + (pv.detached ? " (window)" : "") + "###tab_" + pv.key;
                if (ImGui::BeginTabItem(label.c_str(), nullptr, pick && g.tabKey == pv.key ? ImGuiTabItemFlags_SetSelected : 0)) {
                    ImGui::Spacing();
                    if (actions.drawPanelTab) actions.drawPanelTab(plugin.id, pv.key);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
            if (pick) g.tabPending = false;
        }
        ImGui::PopID();
    }
    endCard();
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
    ImGui::TextUnformatted("Not a Horizon plugin: shown for orientation, Horizon does not control it.");
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
            g.note = "That key is not available.";
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
                     "Shows the Horizon bar when the pointer touches the top edge.");
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

        // ESWC online login fix (only on ESWC)
        if (model.onlineFix >= 0) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            drawText(dl, f.bodyBold, p, U(t.text), "Online login fix");
            drawText(dl, f.small, ImVec2(p.x, p.y + F(f.bodyBold)->FontSize + 2.0f * s), U(t.muted),
                     "Lets the ESWC login reach the server on Windows 10/11. Applies on the next start.");
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
        switchRow("##dnd", "Do not disturb", "Notifications only go to the list below.", model.dnd, actions.setDnd);
        switchRow("##holdtoasts", "Quiet while driving", "Notifications wait until the run ends.", model.holdToasts, actions.setHoldToasts);
        switchRow("##bridge", "TM Horizon connection",
                  model.bridgeEnabled ? (model.bridgeText.empty() ? std::string("On") : model.bridgeText)
                                      : std::string("Off: no TMX search, no notifications from TM Horizon."),
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
                     "Opens and closes this menu.");
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
                            g.note = "That key is not available.";
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
        infoRow("Safe mode", model.safeMode ? "On: mods are not loaded" : "Off");
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

    dim("Click a key to change it.", true);
    ImGui::Dummy(ImVec2(0.0f, 2.0f * s));
    if (model.hotkeys.empty()) {
        dim("No hotkeys registered.", false);
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
            ImGui::TextUnformatted(g.logFilter == 1 && model.log && !model.log->empty() ? "No problems logged."
                                                                                         : "No log lines yet.");
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
        infoRow("Horizon Host", model.hostVersion);
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
    drawText(dl, f.small, ImVec2(p.x, ty), U(t.muted), key + " closes");
    const std::string right = "Horizon Host " + model.hostVersion;
    drawText(dl, f.small, ImVec2(p.x + width - textSize(f.small, right).x, ty), U(t.muted), right);
    ImGui::Dummy(ImVec2(width, height));
}

// Makes sure sel points at something that exists.
void resolveSelection(const Model& model) {
    if (g.sel.empty() || g.sel == "mods") {
        g.sel = model.plugins.empty() ? "general" : "plugin:" + model.plugins[0].id;
    }
    if (startsWith(g.sel, "plugin:") && !findPlugin(model, g.sel.substr(7))) g.sel = "general";
    if (startsWith(g.sel, "other:") && !findOther(model, g.sel.substr(6))) g.sel = "general";
}

void drawPage(const Model& model, const Actions& actions, float height) {
    const float s = scale();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool scrolls = g.sel == "general" || g.sel == "about" || startsWith(g.sel, "other:");
    ImGui::BeginChild("##page", ImVec2(0.0f, height), 0, scrolls ? 0 : ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    (void)s;
    if (startsWith(g.sel, "plugin:")) {
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

    // dim layer
    const ImVec2 disp = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(disp);
    const ImGuiWindowFlags dimFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##horizon_dim", nullptr, dimFlags);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), disp, U(Rgba{0.0f, 0.0f, 0.0f, 1.0f}, 0.55f));
    ImGui::End();

    // panel
    const float pw = std::min(1200.0f * s, disp.x * 0.82f);
    const float ph = std::min(780.0f * s, disp.y * 0.84f);
    const ImVec2 pos(std::floor((disp.x - pw) * 0.5f), std::floor((disp.y - ph) * 0.5f + (1.0f - eased) * 12.0f * s));
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, toVec(t.bg));
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

    drawHeader(model, open);
    ImGui::Dummy(ImVec2(0.0f, 12.0f * s));

    const float footerH = 36.0f * s;
    const float contentH = ImGui::GetContentRegionAvail().y - footerH;
    const float gap = 24.0f * s;
    drawSidebar(model, actions, contentH);
    ImGui::SameLine(0.0f, gap);
    drawPage(model, actions, contentH);
    drawFooter(model, footerH);

    const ImVec2 wpos = ImGui::GetWindowPos();
    const ImVec2 wsize = ImGui::GetWindowSize();
    ImGui::End();
    ImGui::PopStyleVar(); // alpha

    // close: Esc (not while a key is being captured) or a click on the dim layer
    const bool captureBusy = captureAtStart || g.captureHandle > 0 || g.captureEnded;
    if (!captureBusy && !popupOpen && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) *open = false;
    if (!popupOpen && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseHoveringRect(wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y), false)) {
        *open = false;
    }
}

} // namespace ui
} // namespace horizon
