// The big centred menu: header, navigation, mod tiles, plugin page, hotkeys, log, about.
#include "ui.h"
#include "ui_common.h"

#include "imgui.h"

#include <cmath>
#include <cstring>

namespace horizon {
namespace ui {

using namespace detail;

namespace {

enum Page { PageMods = 0, PageHotkeys = 1, PageLog = 2, PageAbout = 3 };

struct MenuState {
    int page = PageMods;
    std::string plugin;      // open plugin page ("" = tile grid)
    int logFilter = 0;       // 0 all, 1 problems
    int captureHandle = 0;   // hotkey waiting for a key
    bool captureEnded = false;
    std::string note;        // short message under the hotkey table
    double noteUntil = 0.0;
    double openedAt = 0.0;
    int lastFrame = -10;
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

// Pill-shaped segmented control. Returns the (new) selection.
int segmented(const char* id, const char* const* labels, int n, int sel, float h, ImFont* font, bool* clickedAny = nullptr) {
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
        if (ImGui::InvisibleButton("##seg", sz)) {
            result = i;
            if (clickedAny) *clickedAny = true;
        }
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

    // wordmark
    const std::string tm = "TM ";
    drawText(dl, f.title, p, U(t.text), tm);
    drawText(dl, f.title, ImVec2(p.x + textSize(f.title, tm).x, p.y), U(t.accent), "HORIZON");
    std::string sub = model.gameName;
    if (!model.exeVersion.empty()) sub += (sub.empty() ? "" : "  \xC2\xB7  ") + model.exeVersion;
    drawText(dl, f.small, ImVec2(p.x + 1.0f * s, p.y + titleH + 4.0f * s), U(t.muted), sub);

    // close button and chips on the right
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

void drawNavRow(const Model& model) {
    const float s = scale();
    const Fonts& f = fonts();
    const char* labels[4] = {"Mods", "Hotkeys", "Log", "About"};
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float h = 40.0f * s;
    ImFont* nf = f.bodyBold;
    ImGui::PushFont(F(nf));
    bool clicked = false;
    const int pick = segmented("nav", labels, 4, g.page, h, F(nf), &clicked);
    ImGui::PopFont();
    if (clicked) {
        g.page = pick;
        g.plugin.clear();
    }
    if (!model.gameState.empty()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const std::string txt = "Now: " + model.gameState;
        const ImVec2 sz = textSize(f.small, txt);
        drawText(dl, f.small, ImVec2(p.x + width - sz.x, p.y + (h - sz.y) * 0.5f), U(tokens().muted), txt);
    }
}

void drawEmptyState(const Model& model, float availW, float availH) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = std::min(availH, 160.0f * s);
    dl->AddRect(p, ImVec2(p.x + availW, p.y + h), W(0.10f), t.radius * s, 0, 1.0f);
    const std::string msg = model.safeMode ? "Safe mode: no mods loaded. Start without Shift to load them."
                                           : "No mods installed.";
    drawTextCentred(dl, f.body, ImVec2(p.x + availW * 0.5f, p.y + h * 0.5f), U(t.text2), msg);
    ImGui::Dummy(ImVec2(availW, h));
}

void drawTile(const PluginView& p, ImVec2 pos, ImVec2 size, const Actions& actions) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = 16.0f * s;

    ImGui::PushID(p.id.c_str());
    ImGui::SetCursorScreenPos(pos);
    ImGui::SetNextItemAllowOverlap();
    const bool bodyClicked = ImGui::InvisibleButton("##tile", size);
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));

    // switch geometry (needed for hit testing before drawing)
    const ImVec2 swSize(44.0f * s, 24.0f * s);
    const float rowY = pos.y + size.y - pad - 26.0f * s;
    const ImVec2 swPos(pos.x + size.x - pad - swSize.x, rowY + 1.0f * s);
    const bool usable = canToggle(p.status);
    ImGui::SetCursorScreenPos(ImVec2(swPos.x - 4.0f * s, swPos.y - 4.0f * s));
    const bool swClicked = ImGui::InvisibleButton("##switch", ImVec2(swSize.x + 8.0f * s, swSize.y + 8.0f * s));
    const bool swHovered = ImGui::IsItemHovered();

    // background
    const float rounding = t.radius * s;
    const ImVec2 br(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, br, U(hovered ? lighten(t.surface, 0.035f) : t.surface), rounding);
    if (hovered) {
        dl->AddRect(pos, br, U(t.accent, 0.9f), rounding, 0, 1.5f * s);
    } else {
        dl->AddRect(pos, br, W(0.06f), rounding, 0, 1.0f);
    }

    // icon, name, meta
    const float iconR = 22.0f * s;
    drawIcon(dl, ImVec2(pos.x + pad + iconR, pos.y + pad + iconR), iconR, p.id, p.name, f.bodyBold);
    const float textX = pos.x + pad + iconR * 2.0f + 12.0f * s;
    const float textW = pos.x + size.x - pad - textX;
    const float nameH = F(f.bodyBold)->FontSize;
    drawText(dl, f.bodyBold, ImVec2(textX, pos.y + pad + 3.0f * s), U(t.text), ellipsize(f.bodyBold, p.name, textW));
    std::string meta = p.version;
    if (!p.author.empty()) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + p.author;
    drawText(dl, f.small, ImVec2(textX, pos.y + pad + 3.0f * s + nameH + 3.0f * s), U(t.muted),
             ellipsize(f.small, meta, textW));

    // description, two lines at most
    const float descY = pos.y + pad + iconR * 2.0f + 12.0f * s;
    const float lineH = F(f.small)->FontSize + 3.0f * s;
    const std::vector<std::string> lines = wrapLines(f.small, p.description, size.x - 2.0f * pad, 2);
    for (size_t i = 0; i < lines.size(); ++i) {
        drawText(dl, f.small, ImVec2(pos.x + pad, descY + static_cast<float>(i) * lineH), U(t.text2), lines[i]);
    }

    // bottom row: status pill, settings hint, switch
    const float chipH = 26.0f * s;
    drawChip(dl, ImVec2(pos.x + pad, rowY), chipH, f.small, statusLabel(p.status, true), statusColour(p.status), false);
    if (p.hasMenu) {
        const std::string label = "Settings";
        const ImVec2 sz = textSize(f.small, label);
        drawText(dl, f.small, ImVec2(swPos.x - 14.0f * s - sz.x, rowY + (chipH - sz.y) * 0.5f),
                 U(hovered ? t.accent2 : t.muted), label);
    }
    float& anim = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("##anim"), p.enabled ? 1.0f : 0.0f);
    const float target = p.enabled ? 1.0f : 0.0f;
    const float step = ImGui::GetIO().DeltaTime / 0.12f;
    if (anim < target) anim = std::min(target, anim + step);
    if (anim > target) anim = std::max(target, anim - step);
    drawSwitch(dl, swPos, swSize, anim, usable, swHovered && usable);

    if (swHovered && usable) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (hovered && !swHovered && p.hasMenu) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (swClicked && usable && actions.setPluginEnabled) {
        actions.setPluginEnabled(p.id, !p.enabled);
    } else if (bodyClicked && !swHovered && p.hasMenu) {
        g.plugin = p.id;
    }

    if (swHovered && usable && !p.liveToggle) {
        ImGui::SetTooltip("Applies after a restart.");
    } else if (hovered && !swHovered && !p.statusText.empty()) {
        ImGui::SetTooltip("%s", p.statusText.c_str());
    }
    ImGui::PopID();
}

void drawMods(const Model& model, const Actions& actions) {
    const float s = scale();
    const float scrollbar = ImGui::GetScrollMaxY() > 0.0f ? ImGui::GetStyle().ScrollbarSize + 4.0f * s : 0.0f;
    const float availW = ImGui::GetContentRegionAvail().x - scrollbar;
    if (model.plugins.empty()) {
        drawEmptyState(model, availW, ImGui::GetContentRegionAvail().y);
        return;
    }
    const float gap = 16.0f * s;
    const float tileW0 = 260.0f * s, tileH = 172.0f * s;
    int cols = static_cast<int>((availW + gap) / (tileW0 + gap));
    if (cols < 1) cols = 1;
    const float tileW = (availW - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    int i = 0;
    for (const PluginView& p : model.plugins) {
        const int cx = i % cols, cy = i / cols;
        drawTile(p, ImVec2(origin.x + static_cast<float>(cx) * (tileW + gap), origin.y + static_cast<float>(cy) * (tileH + gap)),
                 ImVec2(tileW, tileH), actions);
        ++i;
    }
    const int rows = (i + cols - 1) / cols;
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(availW, static_cast<float>(rows) * (tileH + gap) - gap));
}

void drawBackButton() {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 36.0f * s;
    const float w = textSize(f.bodyBold, "Mods").x + 52.0f * s;
    if (ImGui::InvisibleButton("##back", ImVec2(w, h))) g.plugin.clear();
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? W(0.12f) : W(0.06f), h * 0.5f);
    const float cx = p.x + 20.0f * s, cy = p.y + h * 0.5f, a = 5.0f * s;
    const ImU32 col = U(t.text);
    dl->AddLine(ImVec2(cx + a * 0.5f, cy - a), ImVec2(cx - a * 0.5f, cy), col, 1.8f * s);
    dl->AddLine(ImVec2(cx - a * 0.5f, cy), ImVec2(cx + a * 0.5f, cy + a), col, 1.8f * s);
    drawText(dl, f.bodyBold, ImVec2(p.x + 34.0f * s, p.y + (h - F(f.bodyBold)->FontSize) * 0.5f), col, "Mods");
}

void drawPluginPage(const Model& model, const Actions& actions) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    const PluginView* plugin = findPlugin(model, g.plugin);
    if (!plugin) {
        g.plugin.clear();
        return;
    }
    drawBackButton();
    ImGui::Dummy(ImVec2(0.0f, 4.0f * s));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float titleH = F(f.title)->FontSize;
    drawText(dl, f.title, p, U(t.text), plugin->name);
    const float nameW = textSize(f.title, plugin->name).x;
    const float chipH = 26.0f * s;
    drawChip(dl, ImVec2(p.x + nameW + 16.0f * s, p.y + (titleH - chipH) * 0.5f + 2.0f * s), chipH, f.small,
             statusLabel(plugin->status, false), statusColour(plugin->status), false);
    std::string meta = plugin->version;
    if (!plugin->author.empty()) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + plugin->author;
    if (!plugin->statusText.empty()) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + plugin->statusText;
    drawText(dl, f.small, ImVec2(p.x + 1.0f * s, p.y + titleH + 3.0f * s), U(t.muted), meta);
    ImGui::Dummy(ImVec2(width, titleH + F(f.small)->FontSize + 8.0f * s));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec(t.surface, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, t.radius * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * s, 16.0f * s));
    const bool shown = ImGui::BeginChild("##plugin_page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar(2);
    if (shown) {
        if (plugin->hasMenu && actions.drawPluginPage) {
            ImGui::PushID(plugin->id.c_str());
            actions.drawPluginPage(plugin->id);
            ImGui::PopID();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.muted));
            ImGui::TextUnformatted("This mod has no settings.");
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void drawHotkeys(const Model& model, const Actions& actions) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();

    // finish a running key capture
    if (g.captureHandle > 0) {
        bool known = false;
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

    ImGui::PushFont(F(f.small));
    ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.muted));
    ImGui::TextUnformatted("Click a key to change it.");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 2.0f * s));

    if (model.hotkeys.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.text2));
        ImGui::TextUnformatted("No hotkeys registered.");
        ImGui::PopStyleColor();
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
        ImGui::TableSetupColumn("Conflict", ImGuiTableColumnFlags_WidthStretch, 1.6f);
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
            if (ImGui::Button((label + "##key").c_str(), ImVec2(-FLT_MIN, 0.0f)) && !capturing) {
                g.captureHandle = h.handle;
                if (actions.beginKeyCapture) actions.beginKeyCapture(h.handle);
            }
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
    const bool shown = ImGui::BeginChild("##log", ImVec2(0.0f, 0.0f),
                                         ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
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

void aboutRow(const char* label, const std::string& value) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    const float labelW = 160.0f * s;
    const float x0 = ImGui::GetCursorPosX();
    (void)f;
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

void drawAbout(const Model& model) {
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec(t.surface, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, t.radius * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * s, 20.0f * s));
    const bool shown = ImGui::BeginChild("##about", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
    ImGui::PopStyleVar(2);
    if (shown) {
        aboutRow("Horizon Host", model.hostVersion);
        aboutRow("Game", model.gameName);
        aboutRow("Game version", model.exeVersion);
        aboutRow("Profile", model.profileName.empty() ? "none" : model.profileName);
        aboutRow("Menu key", model.menuKey);
        aboutRow("Log file", model.logPath);
        if (!model.notes.empty()) {
            ImGui::Dummy(ImVec2(0.0f, 8.0f * s));
            ImGui::PushFont(F(f.small));
            ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.muted));
            ImGui::TextUnformatted("Notes");
            ImGui::PopStyleColor();
            ImGui::PopFont();
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
        ImGui::PushFont(F(f.small));
        ImGui::PushStyleColor(ImGuiCol_Text, toVec(t.muted));
        ImGui::TextUnformatted("Dear ImGui (MIT, Omar Cornut)");
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
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

} // namespace

void showPluginPage(const std::string& id) {
    g.page = PageMods;
    g.plugin = id;
}

void showPage(const char* page) {
    if (!page) return;
    g.plugin.clear();
    if (std::strcmp(page, "hotkeys") == 0) g.page = PageHotkeys;
    else if (std::strcmp(page, "log") == 0) g.page = PageLog;
    else if (std::strcmp(page, "about") == 0) g.page = PageAbout;
    else g.page = PageMods;
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
                                        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##horizon_panel", nullptr, panelFlags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    if (g.page == PageMods && !g.plugin.empty() && !findPlugin(model, g.plugin)) g.plugin.clear();

    drawHeader(model, open);
    ImGui::Dummy(ImVec2(0.0f, 8.0f * s));
    drawNavRow(model);
    ImGui::Dummy(ImVec2(0.0f, 8.0f * s));

    const float footerH = 36.0f * s;
    const float contentH = ImGui::GetContentRegionAvail().y - footerH;
    const bool pluginPage = g.page == PageMods && !g.plugin.empty();
    const bool innerScroll = pluginPage || g.page == PageHotkeys || g.page == PageLog || g.page == PageAbout;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##content", ImVec2(0.0f, contentH), 0, innerScroll ? ImGuiWindowFlags_NoScrollbar : 0);
    ImGui::PopStyleVar();
    switch (g.page) {
    case PageMods:
        if (pluginPage) drawPluginPage(model, actions);
        else drawMods(model, actions);
        break;
    case PageHotkeys: drawHotkeys(model, actions); break;
    case PageLog: drawLog(model, actions); break;
    default: drawAbout(model); break;
    }
    ImGui::EndChild();
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
