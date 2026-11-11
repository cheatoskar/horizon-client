// Panels (ABI minor 2): pieces of a plugin's UI that the host places. Each panel is a tab on its
// plugin's page in the menu and can be popped out into its own window, which can stay on screen with
// the menu closed. The host owns the windows and their options and saves them per plugin id + panel
// key in host.json ("panel.<plugin>.<key>"). Same model as TM Vibrant Shaders' detached menu pages.
#include "host/panels.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "common/util.h"
#include "host/plugins.h"
#include "imgui.h"

namespace horizon {
namespace {

struct HostPanel {
    Plugin* plugin = nullptr;
    std::string key, title;
    uint32_t icon = 0, flags = 0;
    HorizonPanelDrawFn draw = nullptr;
    void* user = nullptr;
    float dx = 0, dy = 0, dw = 0, dh = 0; // default rect, fractions of the screen
    // layout, saved
    bool detached = false;
    bool keepOpen = false;
    bool locked = false;
    bool clickThrough = false;
    bool hideInCapture = true;
    int opacity = 100;              // percent, 30..100
    float x = -1, y = -1, w = 0, h = 0; // pixels; x < 0 = not placed yet
    // per session
    bool placeNext = true;          // apply x/y/w/h when the window is drawn next
    bool shown = false, shownLast = false;
};

std::vector<std::unique_ptr<HostPanel>> g_panels;

std::string settingKey(const HostPanel& q) { return "panel." + q.plugin->id + "." + q.key; }

void save(const HostPanel& q) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%d,%d,%d,%d,%d,%d,%.0f,%.0f,%.0f,%.0f", q.detached ? 1 : 0, q.keepOpen ? 1 : 0, q.locked ? 1 : 0,
             q.clickThrough ? 1 : 0, q.hideInCapture ? 1 : 0, q.opacity, q.x, q.y, q.w, q.h);
    hostSetSetting(settingKey(q), buf);
}

bool load(HostPanel& q) {
    const std::string v = hostSetting(settingKey(q), "");
    int d = 0, k = 0, l = 0, c = 0, hc = 1, o = 100;
    float x = -1, y = -1, w = 0, h = 0;
    if (v.empty() || sscanf_s(v.c_str(), "%d,%d,%d,%d,%d,%d,%f,%f,%f,%f", &d, &k, &l, &c, &hc, &o, &x, &y, &w, &h) < 6) return false;
    q.detached = d != 0;
    q.keepOpen = k != 0;
    q.locked = l != 0;
    q.clickThrough = c != 0;
    q.hideInCapture = hc != 0;
    q.opacity = o < 30 ? 30 : o > 100 ? 100 : o;
    q.x = x;
    q.y = y;
    q.w = w;
    q.h = h;
    return true;
}

HostPanel* find(const Plugin* p, const std::string& key) {
    for (auto& q : g_panels)
        if (q->plugin == p && q->key == key) return q.get();
    return nullptr;
}

bool validKey(const char* k) {
    if (!k || !*k || strlen(k) > 40) return false;
    for (const char* c = k; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) return false;
    return true;
}

bool sehDraw(HorizonPanelDrawFn fn, void* user) {
    __try {
        fn(user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool usable(const HostPanel& q) { return q.plugin->running && q.plugin->enabled && !pluginGated(q.plugin); }

void drawBody(HostPanel& q) {
    q.shown = true;
    ImGui::PushID(q.key.c_str());
    if (!sehDraw(q.draw, q.user)) pluginCrashed(q.plugin, "a panel");
    ImGui::PopID();
}

// The window options, in the menu tab of a detached panel and in the window's right-click menu.
void drawOptions(HostPanel& q) {
    bool dirty = false;
    dirty |= ImGui::Checkbox("Keep open when the menu closes", &q.keepOpen);
    dirty |= ImGui::Checkbox("Lock layout", &q.locked);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The window can't be moved or resized.");
    dirty |= ImGui::Checkbox("Click-through", &q.clickThrough);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("With the menu closed, clicks go through the window to the game.");
    dirty |= ImGui::Checkbox("Hide in photos and videos", &q.hideInCapture);
    ImGui::SetNextItemWidth(220.0f * ImGui::GetFontSize() / 16.0f);
    dirty |= ImGui::SliderInt("Opacity", &q.opacity, 30, 100, "%d%%");
    if (dirty) save(q);
}

void attach(HostPanel& q) {
    q.detached = false;
    save(q);
}

void detach(HostPanel& q) {
    q.detached = true;
    q.placeNext = true;
    save(q);
}

std::string iconUtf8(uint32_t cp) {
    std::string s;
    if (!cp || !ImGui::GetFont() || !ImGui::GetFont()->FindGlyphNoFallback(static_cast<ImWchar>(cp))) return s;
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s + " ";
}

} // namespace

HorizonResult panelRegister(Plugin* p, const HorizonPanelDef* def) {
    if (!p || !def || def->size < offsetof(HorizonPanelDef, default_x) || !validKey(def->key) || !def->title || !*def->title ||
        !def->draw)
        return HORIZON_ERR_INVALID_ARG;
    HostPanel* q = find(p, def->key);
    const bool fresh = q == nullptr;
    if (fresh) {
        g_panels.push_back(std::make_unique<HostPanel>());
        q = g_panels.back().get();
        q->plugin = p;
        q->key = def->key;
    }
    q->title = def->title;
    q->icon = def->icon;
    q->flags = def->flags ? def->flags : HORIZON_PANEL_MENU_PAGE;
    q->draw = def->draw;
    q->user = def->user;
    if (def->size >= sizeof(HorizonPanelDef)) {
        q->dx = def->default_x;
        q->dy = def->default_y;
        q->dw = def->default_w;
        q->dh = def->default_h;
    }
    if (fresh && !load(*q) && (q->flags & HORIZON_PANEL_HUD)) {
        q->detached = true; // a HUD panel starts in its own window, on screen with the menu closed
        q->keepOpen = true;
    }
    logLine(HORIZON_LOG_INFO, p->id.c_str(), "panel \"%s\" (%s)%s", q->key.c_str(), q->title.c_str(), q->detached ? ", in its own window" : "");
    return HORIZON_OK;
}

void panelUnregister(Plugin* p, const char* key) {
    for (size_t i = 0; i < g_panels.size(); i++) {
        if (g_panels[i]->plugin == p && key && g_panels[i]->key == key) {
            g_panels.erase(g_panels.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void panelsRemovePlugin(Plugin* p) {
    for (size_t i = g_panels.size(); i-- > 0;)
        if (g_panels[i]->plugin == p) g_panels.erase(g_panels.begin() + static_cast<std::ptrdiff_t>(i));
}

int32_t panelVisible(Plugin* p, const char* key) {
    const HostPanel* q = key ? find(p, key) : nullptr;
    return q && q->shownLast ? 1 : 0;
}

uint32_t hostFlagsAll() {
    uint32_t f = 0;
    for (auto& up : plugins())
        if (up->running) f |= up->hostFlags;
    return f;
}

std::vector<ui::PanelView> panelViews(const std::string& pluginId) {
    std::vector<ui::PanelView> out;
    for (auto& q : g_panels) {
        if (q->plugin->id != pluginId) continue;
        ui::PanelView v;
        v.key = q->key;
        v.title = q->title;
        v.icon = q->icon;
        v.detached = q->detached;
        out.push_back(v);
    }
    return out;
}

void panelsBeginFrame() {
    for (auto& q : g_panels) {
        q->shownLast = q->shown;
        q->shown = false;
    }
}

bool panelsWantFrame(bool menuOpen) {
    const bool capture = (hostFlagsAll() & HORIZON_HOST_FLAG_CAPTURE) != 0;
    for (auto& q : g_panels)
        if (q->detached && usable(*q) && (menuOpen || q->keepOpen) && !(capture && q->hideInCapture)) return true;
    return false;
}

// The tab of a panel on its plugin's page in the menu.
void panelDrawTab(const std::string& pluginId, const std::string& key) {
    Plugin* p = findPlugin(pluginId);
    HostPanel* q = p ? find(p, key) : nullptr;
    if (!q) return;
    if (!q->plugin->running) {
        ImGui::TextDisabled("This mod is not running.");
        return;
    }
    if (!q->plugin->enabled) {
        ImGui::TextDisabled("This mod is off.");
        return;
    }
    if (pluginGated(q->plugin)) {
        ImGui::TextDisabled("Not available online.");
        return;
    }
    if (q->detached) {
        ImGui::TextWrapped("%s is in its own window.", q->title.c_str());
        ImGui::Spacing();
        drawOptions(*q);
        ImGui::Spacing();
        if (ImGui::Button("Bring it back")) attach(*q);
        return;
    }
    const char* pop = "Pop out";
    const float bw = ImGui::CalcTextSize(pop).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float x0 = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x0 + ImGui::GetContentRegionAvail().x - bw);
    if (ImGui::SmallButton(pop)) detach(*q);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shows this in its own window that can stay on screen.");
    ImGui::SetCursorPosX(x0);
    drawBody(*q);
}

std::string panelTabLabel(const ui::PanelView& v) { return iconUtf8(v.icon) + v.title; }

bool panelsDrawWindows(bool menuOpen) {
    const bool capture = (hostFlagsAll() & HORIZON_HOST_FLAG_CAPTURE) != 0;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    bool any = false;
    for (size_t i = 0; i < g_panels.size(); i++) {
        HostPanel& q = *g_panels[i];
        if (!q.detached || !usable(q)) continue;
        if (!menuOpen && !q.keepOpen) continue;
        if (capture && q.hideInCapture) continue;
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
        if (!menuOpen) {
            flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
            if (q.clickThrough) flags |= ImGuiWindowFlags_NoInputs;
        }
        if (q.locked) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
        const bool autoSize = q.w <= 0.0f && q.dw <= 0.0f;
        if (autoSize) flags |= ImGuiWindowFlags_AlwaysAutoResize;
        if (q.placeNext) {
            q.placeNext = false;
            const float x = q.x >= 0.0f ? q.x : (q.dx > 0.0f ? q.dx : 0.05f) * screen.x;
            const float y = q.y >= 0.0f ? q.y : (q.dy > 0.0f ? q.dy : 0.15f) * screen.y;
            ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
            if (!autoSize) {
                const float w = q.w > 0.0f ? q.w : q.dw * screen.x;
                const float h = q.h > 0.0f ? q.h : (q.dh > 0.0f ? q.dh * screen.y : 0.0f);
                ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
            }
        }
        ImGui::SetNextWindowBgAlpha(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w * (static_cast<float>(q.opacity) / 100.0f));
        const std::string name = q.title + "###hzpanel." + q.plugin->id + "." + q.key;
        bool open = true;
        if (ImGui::Begin(name.c_str(), menuOpen ? &open : nullptr, flags)) {
            drawBody(q);
            if (menuOpen && ImGui::BeginPopupContextWindow("##panel_options")) {
                drawOptions(q);
                if (ImGui::Selectable("Back to the menu")) open = false;
                ImGui::EndPopup();
            }
            if (menuOpen && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && !ImGui::IsAnyItemHovered())
                ImGui::SetTooltip("Right-click: window options");
            // remember where the player put it
            const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
            const bool moved = std::fabs(pos.x - q.x) > 0.5f || std::fabs(pos.y - q.y) > 0.5f;
            const bool resized = !autoSize && (std::fabs(size.x - q.w) > 0.5f || std::fabs(size.y - q.h) > 0.5f);
            if (moved || resized) {
                q.x = pos.x;
                q.y = pos.y;
                if (!autoSize) {
                    q.w = size.x;
                    q.h = size.y;
                }
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) save(q); // once the drag is over
            }
            any = true;
        }
        ImGui::End();
        if (!open) attach(q);
    }
    return any;
}

} // namespace horizon
