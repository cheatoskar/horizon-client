// Panels (ABI minor 2): pieces of a plugin's UI that the host places. Each panel is a tab on its
// plugin's page in the menu and can be popped out into its own window, which can stay on screen with
// the menu closed (a widget). The host owns the windows and their options and saves them per plugin id +
// panel key in host.json ("panel.<plugin>.<key>"). Same model as TM Vibrant Shaders' detached menu pages.
//
// Widgets (HUD panels and every window kept on screen) get a fixed slot at a screen edge the first time,
// snap to the screen edges and to each other when moved, and have a layout mode (panelsDrawLayout): a frame
// per widget to move, resize, minimise to an edge icon and hide.
#include "host/panels.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "common/util.h"
#include "host/plugins.h"
#include "imgui.h"
#include "imgui_internal.h" // MovingWindow: snapping while a window is dragged
#include "horizon_imgui.h"  // HZ_ICON_* (after imgui.h)

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
    bool minimized = false;         // shown as a small icon at the nearest screen edge
    float scale = 1.0f;             // widgets that fit their content: resized = scaled (0.5..2.5)
    // per session
    bool placeNext = true;          // apply x/y/w/h when the window is drawn next
    int slotFrames = 0;             // > 0: being put into its slot (the size is known after a frame or two)
    float lastW = 0, lastH = 0;     // size drawn last frame
    bool shown = false, shownLast = false;
};

std::vector<std::unique_ptr<HostPanel>> g_panels;
ImVec2 g_screen(0, 0);

float uiScale() {
    const HorizonStyle* st = hostStyle();
    return st && st->scale > 0.0f ? st->scale : 1.0f;
}

std::string settingKey(const HostPanel& q) { return "panel." + q.plugin->id + "." + q.key; }

void save(const HostPanel& q) {
    char buf[200];
    snprintf(buf, sizeof(buf), "%d,%d,%d,%d,%d,%d,%.0f,%.0f,%.0f,%.0f,%d,%.2f", q.detached ? 1 : 0, q.keepOpen ? 1 : 0, q.locked ? 1 : 0,
             q.clickThrough ? 1 : 0, q.hideInCapture ? 1 : 0, q.opacity, q.x, q.y, q.w, q.h, q.minimized ? 1 : 0, q.scale);
    hostSetSetting(settingKey(q), buf);
}

bool load(HostPanel& q) {
    const std::string v = hostSetting(settingKey(q), "");
    int d = 0, k = 0, l = 0, c = 0, hc = 1, o = 100, mn = 0;
    float x = -1, y = -1, w = 0, h = 0, sc = 1.0f;
    if (v.empty() || sscanf_s(v.c_str(), "%d,%d,%d,%d,%d,%d,%f,%f,%f,%f,%d,%f", &d, &k, &l, &c, &hc, &o, &x, &y, &w, &h, &mn, &sc) < 6) return false;
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
    q.minimized = mn != 0;
    q.scale = sc < 0.5f ? 0.5f : sc > 2.5f ? 2.5f : sc;
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
bool autoSized(const HostPanel& q) { return q.w <= 0.0f && q.dw <= 0.0f; }
bool isWidget(const HostPanel& q) { return q.detached && q.keepOpen; }   // on screen with the menu closed
bool capturing() { return (hostFlagsAll() & HORIZON_HOST_FLAG_CAPTURE) != 0; }
int widgetOpacity() {
    const int v = hostSettingInt("widgets.opacity", 65);
    return v < 20 ? 20 : v > 100 ? 100 : v;
}

void drawBody(HostPanel& q, bool scaled) {
    q.shown = true;
    ImGui::PushID(q.key.c_str());
    // a widget that fits its content is resized by scaling it: the plugin reads style()->scale while drawing
    HorizonStyle* st = const_cast<HorizonStyle*>(hostStyle());
    const float before = st ? st->scale : 1.0f;
    if (scaled && q.scale != 1.0f) {
        if (st) st->scale = before * q.scale;
        ImGui::SetWindowFontScale(q.scale);
    }
    const bool ok = sehDraw(q.draw, q.user);
    if (scaled && q.scale != 1.0f) {
        if (st) st->scale = before;
        ImGui::SetWindowFontScale(1.0f);
    }
    if (!ok) pluginCrashed(q.plugin, "a panel");
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

// Shows a hidden widget again (layout mode bar): in its own window, kept open, full size.
void showWidget(HostPanel& q) {
    q.detached = true;
    q.keepOpen = true;
    q.minimized = false;
    q.placeNext = true;
    save(q);
}

std::string iconUtf8(uint32_t cp, bool space = true) {
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
    return space ? s + " " : s;
}

// ---- slots and snapping ------------------------------------------------------------------------------

float margin() { return 16.0f * uiScale(); }
float gap() { return 8.0f * uiScale(); }

// 0 / 0.5 / 1 per axis (left, centre, right; top, middle, bottom) from the plugin's default place.
ImVec2 slotOf(const HostPanel& q) {
    const float x = q.dx > 0.0f ? q.dx : 0.05f, y = q.dy > 0.0f ? q.dy : 0.15f;
    return ImVec2(x < 0.34f ? 0.0f : x > 0.6f ? 1.0f : 0.5f, y < 0.34f ? 0.0f : y > 0.6f ? 1.0f : 0.5f);
}

struct Rect {
    float x, y, w, h;
    bool overlaps(const Rect& o, float pad) const {
        return x < o.x + o.w + pad && o.x < x + w + pad && y < o.y + o.h + pad && o.y < y + h + pad;
    }
};

// Widgets on screen now (placed, drawn last frame, not minimised), except self.
std::vector<Rect> otherWidgets(const HostPanel* self) {
    std::vector<Rect> out;
    for (auto& up : g_panels) {
        const HostPanel& o = *up;
        if (&o == self || !isWidget(o) || o.minimized || !usable(o) || o.x < 0.0f || !o.shownLast || o.lastW <= 0.0f) continue;
        if (o.slotFrames > 0) continue;
        out.push_back(Rect{o.x, o.y, o.lastW, o.lastH});
    }
    return out;
}

// The place of a widget in its slot with this size: at the edge, below/above the widgets already there.
ImVec2 slotPlace(const HostPanel& q, float w, float h) {
    const ImVec2 a = slotOf(q);
    const float m = margin(), W = g_screen.x, H = g_screen.y;
    Rect r{a.x == 0.0f ? m : a.x == 1.0f ? W - m - w : std::floor((W - w) * 0.5f),
           a.y == 0.0f ? m : a.y == 1.0f ? H - m - h : std::floor((H - h) * 0.5f), w, h};
    const std::vector<Rect> others = otherWidgets(&q);
    for (int tries = 0; tries < 12; tries++) {
        const Rect* hit = nullptr;
        for (const Rect& o : others)
            if (r.overlaps(o, 0.0f)) hit = &o;
        if (!hit) break;
        if (a.y == 1.0f) r.y = hit->y - gap() - h; // bottom slots grow upwards
        else r.y = hit->y + hit->h + gap();         // top and middle slots grow downwards
    }
    r.x = std::fmax(0.0f, std::fmin(r.x, W - w));
    r.y = std::fmax(0.0f, std::fmin(r.y, H - h));
    return ImVec2(r.x, r.y);
}

// Snaps a window's top-left corner to the screen edges (at the slot margin), the centre lines and the edges
// of other widgets, each within a few pixels.
ImVec2 snap(const HostPanel* self, ImVec2 p, ImVec2 sz) {
    const float th = 12.0f * uiScale(), m = margin(), g = gap(), W = g_screen.x, H = g_screen.y;
    float bestX = th, bestY = th;
    ImVec2 out = p;
    auto tryX = [&](float x) {
        if (std::fabs(x - p.x) < bestX) {
            bestX = std::fabs(x - p.x);
            out.x = x;
        }
    };
    auto tryY = [&](float y) {
        if (std::fabs(y - p.y) < bestY) {
            bestY = std::fabs(y - p.y);
            out.y = y;
        }
    };
    tryX(m);
    tryX(W - m - sz.x);
    tryX(std::floor((W - sz.x) * 0.5f));
    tryX(0.0f);
    tryX(W - sz.x);
    tryY(m);
    tryY(H - m - sz.y);
    tryY(std::floor((H - sz.y) * 0.5f));
    tryY(0.0f);
    tryY(H - sz.y);
    for (const Rect& o : otherWidgets(self)) {
        tryX(o.x + o.w + g);    // right of it
        tryX(o.x - g - sz.x);   // left of it
        tryX(o.x);              // left edges aligned
        tryX(o.x + o.w - sz.x); // right edges aligned
        tryY(o.y + o.h + g);
        tryY(o.y - g - sz.y);
        tryY(o.y);
        tryY(o.y + o.h - sz.y);
    }
    out.x = std::fmax(0.0f, std::fmin(out.x, W - sz.x));
    out.y = std::fmax(0.0f, std::fmin(out.y, H - sz.y));
    return out;
}

// ---- minimised widgets: an icon at the nearest screen edge ---------------------------------------------

float iconSize() { return 40.0f * uiScale(); }

ImVec2 iconPlace(const HostPanel& q, size_t index) {
    const float d = iconSize(), inset = 6.0f * uiScale(), W = g_screen.x, H = g_screen.y;
    const float w = q.lastW > 0.0f ? q.lastW : d, h = q.lastH > 0.0f ? q.lastH : d;
    const float cx = (q.x >= 0.0f ? q.x : W * 0.5f) + w * 0.5f, cy = (q.y >= 0.0f ? q.y : H * 0.5f) + h * 0.5f;
    const float dl = cx, dr = W - cx, dt = cy, db = H - cy;
    const float best = std::fmin(std::fmin(dl, dr), std::fmin(dt, db));
    ImVec2 p;
    if (best == dl) p = ImVec2(inset, cy - d * 0.5f);
    else if (best == dr) p = ImVec2(W - inset - d, cy - d * 0.5f);
    else if (best == dt) p = ImVec2(cx - d * 0.5f, inset);
    else p = ImVec2(cx - d * 0.5f, H - inset - d);
    // two icons at the same spot: the later one moves along the edge
    (void)index;
    p.x = std::fmax(0.0f, std::fmin(p.x, W - d));
    p.y = std::fmax(0.0f, std::fmin(p.y, H - d));
    return p;
}

void drawIconBadge(ImDrawList* dl, ImVec2 p, float d, const HostPanel& q, bool hovered) {
    const HorizonStyle* st = hostStyle();
    const ImVec2 c(p.x + d * 0.5f, p.y + d * 0.5f);
    dl->AddCircleFilled(c, d * 0.5f, ImGui::GetColorU32(ImVec4(st->bg2.r, st->bg2.g, st->bg2.b, 0.92f)), 32);
    dl->AddCircle(c, d * 0.5f, hovered ? ImGui::GetColorU32(ImVec4(st->accent.r, st->accent.g, st->accent.b, 1.0f))
                                       : ImGui::GetColorU32(ImVec4(1, 1, 1, 0.18f)),
                  32, 1.5f * uiScale());
    const unsigned long long tex = ui::iconFor(q.plugin->id);
    const float r = d * 0.30f;
    if (tex) {
        dl->AddImageRounded(static_cast<ImTextureID>(tex), ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, r * 0.45f);
    } else {
        const std::string g = iconUtf8(q.icon ? q.icon : HZ_ICON_LAYOUT_GRID, false);
        const ImVec2 ts = ImGui::CalcTextSize(g.c_str());
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), ImGui::GetColorU32(ImVec4(st->text.r, st->text.g, st->text.b, 1.0f)), g.c_str());
    }
}

// The edge icons of minimised widgets. Clicking one brings the widget back.
bool drawMinimisedIcons(bool clickable) {
    bool any = false;
    size_t index = 0;
    for (auto& up : g_panels) {
        HostPanel& q = *up;
        if (!isWidget(q) || !q.minimized || !usable(q)) continue;
        if (capturing() && q.hideInCapture) continue;
        const float d = iconSize();
        const ImVec2 p = iconPlace(q, index++);
        ImGui::SetNextWindowPos(p, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(d, d), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
        if (!clickable) flags |= ImGuiWindowFlags_NoInputs;
        const std::string name = "###hzmin." + q.plugin->id + "." + q.key;
        if (ImGui::Begin(name.c_str(), nullptr, flags)) {
            // above the menu and the layout mode's full-screen window, which were made later
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            ImGui::SetCursorScreenPos(p);
            const bool clicked = ImGui::InvisibleButton("##restore", ImVec2(d, d));
            const bool hov = ImGui::IsItemHovered();
            drawIconBadge(ImGui::GetWindowDrawList(), p, d, q, hov);
            if (hov) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s: click to show", q.title.c_str());
            }
            if (clicked) {
                q.minimized = false;
                q.placeNext = true;
                save(q);
            }
            any = true;
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    return any;
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
    if (def->size >= offsetof(HorizonPanelDef, default_h) + sizeof(float)) {
        q->dx = def->default_x;
        q->dy = def->default_y;
        q->dw = def->default_w;
        q->dh = def->default_h;
    }
    if (fresh && !load(*q) && (q->flags & HORIZON_PANEL_HUD) && !(q->flags & HORIZON_PANEL_START_HIDDEN)) {
        q->detached = true; // a HUD panel starts in its own window, on screen with the menu closed
        q->keepOpen = true;
    }
    // START_HIDDEN: stays a tab (listed as hidden in layout mode) until the player shows it
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
    const bool capture = capturing();
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
        if (q->minimized && q->keepOpen) {
            ImGui::TextWrapped("%s is minimised to an icon at the screen edge.", q->title.c_str());
            if (ImGui::Button("Show")) {
                q->minimized = false;
                q->placeNext = true;
                save(*q);
            }
        } else {
            ImGui::TextWrapped("%s is in its own window.", q->title.c_str());
        }
        ImGui::Spacing();
        drawOptions(*q);
        ImGui::Spacing();
        if (ImGui::Button((q->flags & HORIZON_PANEL_HUD) ? "Hide" : "Dock")) attach(*q);
        return;
    }
    // a widget that is not on screen: one click shows it at its place
    if (q->flags & HORIZON_PANEL_HUD) {
        if (ImGui::Button("Show")) showWidget(*q);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show on screen.");
    }
    // the pop-out button: a square icon button the size of a normal button, top right of every panel tab
    const float side = ImGui::GetFrameHeight();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float right = start.x + ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorScreenPos(ImVec2(right - side, start.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    const std::string glyph = iconUtf8(HZ_ICON_SQUARE_ARROW_OUT_UP_RIGHT, false);
    const std::string popLabel = (glyph.empty() ? std::string("^") : glyph) + "##popout";
    if (ImGui::Button(popLabel.c_str(), ImVec2(side, side))) detach(*q);
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Pop out: its own window that can stay on screen.");
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, ImGui::GetCursorScreenPos().y)); // the body starts below the button
    drawBody(*q, false);
}

std::string panelTabLabel(const ui::PanelView& v) { return iconUtf8(v.icon) + v.title; }

bool panelsDrawWindows(const PanelsFrame& f) {
    const bool capture = capturing();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    if (screen.x != g_screen.x || screen.y != g_screen.y) {
        // first frame or a new display size (Reset): place every window again, clamped to the screen
        g_screen = screen;
        for (auto& q : g_panels) q->placeNext = true;
    }
    const bool menuOpen = f.menuOpen;
    bool any = false;
    for (size_t i = 0; i < g_panels.size(); i++) {
        HostPanel& q = *g_panels[i];
        if (!q.detached || !usable(q)) continue;
        if (!menuOpen && !q.keepOpen) continue;
        if (capture && q.hideInCapture) continue;
        if (q.keepOpen && q.minimized) continue; // drawn as an edge icon
        const bool widget = isWidget(q);
        const bool hud = widget && (q.flags & HORIZON_PANEL_HUD);
        // No title bar in either state: with one only while the menu is open, every widget grew by the bar's
        // height and its content jumped down when the menu opened. With the menu open it moves by dragging
        // its body and resizes at its edges; the right-click menu has "Back to the menu".
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
        if (!menuOpen) {
            flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBringToFrontOnFocus;
            if (q.clickThrough || f.layout) flags |= ImGuiWindowFlags_NoInputs; // layout mode: the frames take the mouse
        }
        if (q.locked) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
        const bool autoSize = autoSized(q);
        if (autoSize) flags |= ImGuiWindowFlags_AlwaysAutoResize;
        if (q.placeNext) {
            q.placeNext = false;
            if (q.x < 0.0f && hud) {
                q.slotFrames = 3; // a new widget: into its slot once its size is known
            } else {
                const float w = q.w > 0.0f ? q.w : q.dw * screen.x;
                const float h = q.h > 0.0f ? q.h : (q.dh > 0.0f ? q.dh * screen.y : 0.0f);
                float x = q.x >= 0.0f ? q.x : (q.dx > 0.0f ? q.dx : 0.05f) * screen.x;
                float y = q.y >= 0.0f ? q.y : (q.dy > 0.0f ? q.dy : 0.15f) * screen.y;
                const float cw = autoSize ? (q.lastW > 0.0f ? q.lastW : 0.0f) : w;
                const float ch = autoSize ? (q.lastH > 0.0f ? q.lastH : 0.0f) : h;
                x = std::fmax(0.0f, std::fmin(x, screen.x - std::fmin(cw > 0.0f ? cw : 40.0f, screen.x)));
                y = std::fmax(0.0f, std::fmin(y, screen.y - std::fmin(ch > 0.0f ? ch : 40.0f, screen.y)));
                ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
                if (!autoSize) ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
            }
        }
        if (q.slotFrames > 0) {
            const ImVec2 p = slotPlace(q, q.lastW, q.lastH);
            ImGui::SetNextWindowPos(p, ImGuiCond_Always);
            if (!autoSize) {
                const float w = q.w > 0.0f ? q.w : q.dw * screen.x;
                const float h = q.h > 0.0f ? q.h : (q.dh > 0.0f ? q.dh * screen.y : 0.0f);
                ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
            }
        } else if (f.layout && widget && q.x >= 0.0f) {
            // layout mode moves and sizes the window from the frame
            ImGui::SetNextWindowPos(ImVec2(q.x, q.y), ImGuiCond_Always);
            if (!autoSize && q.w > 0.0f) ImGui::SetNextWindowSize(ImVec2(q.w, q.h), ImGuiCond_Always);
        }
        // widgets: the player's widget background (General, default 65 %) times the panel's own opacity
        const float widgetBg = widget ? static_cast<float>(widgetOpacity()) / 100.0f : 1.0f;
        ImGui::SetNextWindowBgAlpha(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w * widgetBg * (static_cast<float>(q.opacity) / 100.0f));
        const std::string name = q.title + "###hzpanel." + q.plugin->id + "." + q.key;
        bool open = true;
        if (ImGui::Begin(name.c_str(), menuOpen ? &open : nullptr, flags)) {
            drawBody(q, autoSize);
            if (menuOpen && ImGui::BeginPopupContextWindow("##panel_options")) {
                drawOptions(q);
                if (ImGui::Selectable("Back to the menu")) open = false;
                ImGui::EndPopup();
            }
            if (menuOpen && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && !ImGui::IsAnyItemHovered())
                ImGui::SetTooltip("Right-click: window options");
            ImVec2 pos = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            // dragged by its title bar (menu open): snap to the edges and the other widgets
            ImGuiContext& g = *ImGui::GetCurrentContext();
            if (menuOpen && g.MovingWindow && g.MovingWindow->RootWindow == ImGui::GetCurrentWindow()) {
                const ImVec2 snapped = snap(&q, pos, size);
                if (snapped.x != pos.x || snapped.y != pos.y) {
                    ImGui::SetWindowPos(snapped);
                    pos = snapped;
                }
            }
            q.lastW = size.x;
            q.lastH = size.y;
            if (q.slotFrames > 0) {
                // stays in the slot until the size settled, then the place is saved like a move
                if (--q.slotFrames == 0) {
                    q.x = pos.x;
                    q.y = pos.y;
                    save(q);
                    logLine(HORIZON_LOG_INFO, q.plugin->id.c_str(), "widget \"%s\" placed in its slot at %.0f,%.0f", q.key.c_str(), pos.x, pos.y);
                }
            } else {
                // remember where the player put it
                const bool moved = std::fabs(pos.x - q.x) > 0.5f || std::fabs(pos.y - q.y) > 0.5f;
                const bool resized = !autoSize && (std::fabs(size.x - q.w) > 0.5f || std::fabs(size.y - q.h) > 0.5f);
                if ((moved || resized) && !f.layout) {
                    q.x = pos.x;
                    q.y = pos.y;
                    if (!autoSize) {
                        q.w = size.x;
                        q.h = size.y;
                    }
                    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) save(q); // once the drag is over
                }
            }
            any = true;
        }
        ImGui::End();
        if (!open) attach(q);
    }
    any = drawMinimisedIcons(f.iconsClickable || f.menuOpen || f.layout) || any;
    return any;
}

// ---- layout mode -----------------------------------------------------------------------------------------

namespace {
struct Drag {
    HostPanel* panel = nullptr;
    int what = 0;           // 1 move, 2 resize
    ImVec2 startPos, startSize;
    float startScale = 1.0f;
} g_drag;

ImU32 tok(const HorizonColor& c, float a = -1.0f) { return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, a >= 0.0f ? a : c.a)); }

// Small round button with a glyph; true when clicked.
bool roundButton(ImDrawList* dl, const char* id, ImVec2 c, float r, const char* glyph, const char* tip) {
    const HorizonStyle* st = hostStyle();
    ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(r * 2.0f, r * 2.0f));
    const bool hov = ImGui::IsItemHovered();
    dl->AddCircleFilled(c, r, hov ? tok(st->accent) : tok(st->bg2, 0.95f), 24);
    dl->AddCircle(c, r, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.25f)), 24, 1.0f);
    const ImVec2 ts = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), hov ? tok(st->accent_ink) : tok(st->text), glyph);
    if (hov) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", tip);
    }
    return clicked;
}

// A pill button for the layout bar; true when clicked.
bool pill(ImDrawList* dl, const char* id, ImVec2 p, float h, const std::string& label, bool primary) {
    const HorizonStyle* st = hostStyle();
    const float w = ImGui::CalcTextSize(label.c_str()).x + 28.0f * uiScale();
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), primary ? tok(hov ? st->accent2 : st->accent) : ImGui::GetColorU32(ImVec4(1, 1, 1, hov ? 0.14f : 0.07f)),
                      h * 0.5f);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), primary ? tok(st->accent_ink) : tok(st->text), label.c_str());
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}
float pillWidth(const std::string& label) { return ImGui::CalcTextSize(label.c_str()).x + 28.0f * uiScale(); }
} // namespace

void panelsResetLayout() {
    for (auto& up : g_panels) {
        HostPanel& q = *up;
        if (!(q.flags & HORIZON_PANEL_HUD) && !isWidget(q)) continue;
        if ((q.flags & HORIZON_PANEL_START_HIDDEN) && !isWidget(q)) continue; // off until the player shows it
        if ((q.flags & HORIZON_PANEL_HUD) && !(q.flags & HORIZON_PANEL_START_HIDDEN)) {
            q.detached = true;
            q.keepOpen = true;
        }
        q.x = -1;
        q.y = -1;
        if (q.dw <= 0.0f) q.w = q.h = 0;
        q.scale = 1.0f;
        q.minimized = false;
        q.placeNext = true;
        q.slotFrames = 0;
        save(q);
    }
    HLOG("layout: every widget back in its slot");
}

void panelsDrawLayout() {
    const HorizonStyle* st = hostStyle();
    const float s = uiScale();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 screen = io.DisplaySize;
    // a light dim behind the widgets, so the frames stand out from the game
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, 90));

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(screen, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##hzlayout", nullptr, flags);
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 accent = tok(st->accent);

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && g_drag.panel) {
        save(*g_drag.panel);
        g_drag = Drag{};
    }
    for (auto& up : g_panels) {
        HostPanel& q = *up;
        if (!isWidget(q) || q.minimized || !usable(q) || q.x < 0.0f || q.lastW <= 0.0f || q.slotFrames > 0) continue;
        ImGui::PushID(&q);
        const ImVec2 a(q.x, q.y), b(q.x + q.lastW, q.y + q.lastH);
        // move: the whole widget
        ImGui::SetCursorScreenPos(a);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##move", ImVec2(q.lastW, q.lastH));
        const bool moveHov = ImGui::IsItemHovered();
        if (ImGui::IsItemActivated()) g_drag = Drag{&q, 1, a, ImVec2(q.lastW, q.lastH), q.scale};
        // resize: the bottom-right corner
        const float grip = 20.0f * s;
        ImGui::SetCursorScreenPos(ImVec2(b.x - grip, b.y - grip));
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##size", ImVec2(grip, grip));
        const bool sizeHov = ImGui::IsItemHovered();
        if (ImGui::IsItemActivated()) g_drag = Drag{&q, 2, a, ImVec2(q.lastW, q.lastH), q.scale};
        // minimise and hide: top right
        const float r = 12.0f * s;
        // name and buttons above the widget; below it when it sits at the top edge (no room above)
        const bool below = a.y < 2.0f * r + 14.0f * s;
        const float cy = below ? b.y + r + 6.0f * s : a.y - r - 6.0f * s;
        const ImVec2 c1(b.x - r - 4.0f * s, cy), c0(c1.x - 2.0f * r - 6.0f * s, c1.y);
        bool minimise = false, hide = false;
        if (roundButton(dl, "##min", c0, r, HZ_ICON_MINUS_U8, "Minimise")) minimise = true;
        if (roundButton(dl, "##hide", c1, r, HZ_ICON_EYE_OFF_U8, "Hide")) hide = true;

        const bool dragging = g_drag.panel == &q;
        if (dragging && g_drag.what == 1) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            const ImVec2 p = snap(&q, ImVec2(g_drag.startPos.x + d.x, g_drag.startPos.y + d.y), ImVec2(q.lastW, q.lastH));
            q.x = p.x;
            q.y = p.y;
        } else if (dragging && g_drag.what == 2) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            if (autoSized(q)) {
                const float fx = (g_drag.startSize.x + d.x) / std::fmax(1.0f, g_drag.startSize.x);
                const float fy = (g_drag.startSize.y + d.y) / std::fmax(1.0f, g_drag.startSize.y);
                const float f = std::fmax(fx, fy);
                q.scale = std::fmax(0.5f, std::fmin(2.5f, std::round(g_drag.startScale * f * 20.0f) / 20.0f));
            } else {
                q.w = std::fmax(120.0f * s, std::fmin(g_drag.startSize.x + d.x, screen.x - q.x));
                q.h = std::fmax(80.0f * s, std::fmin(g_drag.startSize.y + d.y, screen.y - q.y));
            }
        }
        if (sizeHov || (dragging && g_drag.what == 2)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        else if (moveHov || (dragging && g_drag.what == 1)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

        // the frame, the corner and the name
        const bool hot = moveHov || sizeHov || dragging;
        dl->AddRect(ImVec2(a.x - 2.0f, a.y - 2.0f), ImVec2(b.x + 2.0f, b.y + 2.0f), hot ? accent : tok(st->accent, 0.55f),
                    st->radius_sm, 0, hot ? 2.5f : 1.5f);
        dl->AddTriangleFilled(ImVec2(b.x, b.y - grip * 0.8f), ImVec2(b.x, b.y), ImVec2(b.x - grip * 0.8f, b.y), sizeHov ? accent : tok(st->accent, 0.7f));
        std::string label = q.plugin->name + "  \xC2\xB7  " + q.title;
        if (autoSized(q) && q.scale != 1.0f) label += "  " + std::to_string(static_cast<int>(q.scale * 100.0f + 0.5f)) + "%";
        const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
        const ImVec2 lp(a.x, below ? cy - ls.y * 0.5f : a.y - ls.y - 10.0f * s);
        dl->AddRectFilled(ImVec2(lp.x - 2.0f, lp.y - 3.0f * s), ImVec2(lp.x + ls.x + 14.0f * s, lp.y + ls.y + 3.0f * s), tok(st->bg2, 0.92f),
                          st->radius_sm);
        dl->AddText(ImVec2(lp.x + 6.0f * s, lp.y), tok(st->text), label.c_str());
        ImGui::PopID();
        if (minimise) {
            q.minimized = true;
            save(q);
        }
        if (hide) {
            q.detached = false; // back into the menu: a tab on its plugin's page, listed in the bar to show again
            save(q);
        }
    }

    // the bar at the top: what this is, hidden widgets to show again, Reset, Done
    {
        const float h = 34.0f * s, pad = 14.0f * s, gp = 8.0f * s;
        const std::string title = std::string(HZ_ICON_LAYOUT_GRID_U8) + "  Edit layout";
        const std::string hint = "Drag to move, corner to resize. Esc ends.";
        std::vector<HostPanel*> hidden;
        for (auto& up : g_panels)
            if ((up->flags & HORIZON_PANEL_HUD) && !isWidget(*up) && usable(*up)) hidden.push_back(up.get());
        float w = pad + ImGui::CalcTextSize(title.c_str()).x + 16.0f * s + ImGui::CalcTextSize(hint.c_str()).x + 16.0f * s;
        std::vector<std::string> chips;
        for (HostPanel* q : hidden) {
            chips.push_back(std::string(HZ_ICON_PLUS_U8) + " " + q->title);
            w += pillWidth(chips.back()) + gp;
        }
        w += pillWidth("Reset") + gp + pillWidth("Done") + pad;
        const float barH = h + 16.0f * s;
        const ImVec2 p(std::floor((screen.x - w) * 0.5f), 12.0f * s);
        ImDrawList* wl = ImGui::GetWindowDrawList();
        wl->AddRectFilled(p, ImVec2(p.x + w, p.y + barH), tok(st->bg2, 0.96f), barH * 0.5f);
        wl->AddRect(p, ImVec2(p.x + w, p.y + barH), tok(st->accent, 0.6f), barH * 0.5f, 0, 1.5f);
        // the bar itself takes clicks (nothing below it moves)
        ImGui::SetCursorScreenPos(p);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##layoutbar", ImVec2(w, barH));
        float x = p.x + pad;
        const float ty = p.y + (barH - ImGui::GetTextLineHeight()) * 0.5f;
        wl->AddText(ImVec2(x, ty), tok(st->text), title.c_str());
        x += ImGui::CalcTextSize(title.c_str()).x + 16.0f * s;
        wl->AddText(ImVec2(x, ty), tok(st->muted), hint.c_str());
        x += ImGui::CalcTextSize(hint.c_str()).x + 16.0f * s;
        const float by = p.y + (barH - h) * 0.5f;
        for (size_t i = 0; i < hidden.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            if (pill(wl, "##show", ImVec2(x, by), h, chips[i], false)) showWidget(*hidden[i]);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show %s again", hidden[i]->plugin->name.c_str());
            ImGui::PopID();
            x += pillWidth(chips[i]) + gp;
        }
        if (pill(wl, "##reset", ImVec2(x, by), h, "Reset", false)) panelsResetLayout();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every widget back to its place and size.");
        x += pillWidth("Reset") + gp;
        if (pill(wl, "##done", ImVec2(x, by), h, "Done", true)) setLayoutMode(false, "Done");
    }
    ImGui::End();
}

} // namespace horizon
