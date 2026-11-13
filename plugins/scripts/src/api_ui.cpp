// UI namespace: a small Dear ImGui subset that draws through the host's ImGui context.
//
// Safety: UI calls only work inside Render() / RenderMenu() (an ImGui frame is running) and need the
// "ui" permission. Every Begin/Push a script makes is recorded; a mismatched End/Pop is a script error
// instead of an ImGui assert, and whatever the script left open (also after an exception or a budget
// abort) is closed by uiEndCall(). Format strings reach ImGui only after a check (one number, no %s/%n).
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "angelscript.h"
#include "api_types.h"
#include "engine.h"
#include "imgui.h"
#include "imgui_internal.h" // the widget window's draw list (UI::WidgetAlpha), layout mode window lookup
#include "scriptarray/scriptarray.h"

namespace hs {
namespace {

enum class Open : uint8_t { Window, Child, Table, Color, Var, Id, Group, Disabled, Tooltip, ItemWidth, TextWrap, TreeNode, Font };

std::vector<Open> g_stack;
bool g_allowed = false;
bool g_inWidget = false;
bool g_scopePushed = false;

const char* openName(Open o) {
    switch (o) {
    case Open::Window: return "UI::Begin";
    case Open::Child: return "UI::BeginChild";
    case Open::Table: return "UI::BeginTable";
    case Open::Color: return "UI::PushStyleColor";
    case Open::Var: return "UI::PushStyleVar";
    case Open::Id: return "UI::PushID";
    case Open::Group: return "UI::BeginGroup";
    case Open::Disabled: return "UI::BeginDisabled";
    case Open::Tooltip: return "UI::BeginTooltip";
    case Open::ItemWidth: return "UI::PushItemWidth";
    case Open::TextWrap: return "UI::PushTextWrapPos";
    case Open::TreeNode: return "UI::TreeNode";
    case Open::Font: return "UI::PushFont";
    }
    return "?";
}

void close(Open o) {
    switch (o) {
    case Open::Window: ImGui::End(); break;
    case Open::Child: ImGui::EndChild(); break;
    case Open::Table: ImGui::EndTable(); break;
    case Open::Color: ImGui::PopStyleColor(); break;
    case Open::Var: ImGui::PopStyleVar(); break;
    case Open::Id: ImGui::PopID(); break;
    case Open::Group: ImGui::EndGroup(); break;
    case Open::Disabled: ImGui::EndDisabled(); break;
    case Open::Tooltip: ImGui::EndTooltip(); break;
    case Open::ItemWidth: ImGui::PopItemWidth(); break;
    case Open::TextWrap: ImGui::PopTextWrapPos(); break;
    case Open::TreeNode: ImGui::TreePop(); break;
    case Open::Font: ImGui::PopFont(); break;
    }
}

bool ok(const char* what) {
    if (!requirePerm(PermUi, what)) return false;
    if (!g_allowed) {
        scriptError(std::string(what) + " only works inside Render() and RenderMenu()");
        return false;
    }
    return true;
}

// Closes the innermost `o`; anything else on top is a script error (the guard closes it after the call).
bool pop(Open o, const char* what) {
    if (!ok(what)) return false;
    if (g_stack.empty() || g_stack.back() != o) {
        scriptError(std::string(what) + " without a matching " + openName(o) +
                    (g_stack.empty() ? std::string() : std::string(" (close ") + openName(g_stack.back()) + " first)"));
        return false;
    }
    g_stack.pop_back();
    close(o);
    return true;
}

bool push(Open o) {
    if (g_stack.size() >= 64) {
        scriptError("UI: more than 64 Begin/Push calls open at once");
        return false;
    }
    g_stack.push_back(o);
    return true;
}

ImVec2 iv(const Vec2& v) { return ImVec2(v.x, v.y); }
ImVec4 iv(const Vec4& v) { return ImVec4(v.x, v.y, v.z, v.w); }
Vec2 hv(const ImVec2& v) { return Vec2{v.x, v.y}; }
ImU32 col32(const Vec4& c) { return ImGui::ColorConvertFloat4ToU32(iv(c)); }

// One numeric conversion (d/i/u for ints, f/e/g for floats) with flags/width/precision, "%%" allowed.
std::string safeFormat(const std::string& fmt, bool isFloat) {
    const char* fallback = isFloat ? "%.3f" : "%d";
    if (fmt.size() > 64) return fallback;
    int conversions = 0;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') continue;
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < fmt.size() && strchr("-+ #0", fmt[j])) j++;
        while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') j++;
        if (j < fmt.size() && fmt[j] == '.') {
            j++;
            while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') j++;
        }
        if (j >= fmt.size()) return fallback;
        const char c = fmt[j];
        const bool good = isFloat ? (c == 'f' || c == 'e' || c == 'g') : (c == 'd' || c == 'i' || c == 'u');
        if (!good || ++conversions > 1) return fallback;
        i = j;
    }
    return conversions == 1 ? fmt : fallback;
}

std::string windowName(const std::string& title) {
    if (title.find("##") != std::string::npos) return title;
    Script* s = currentScript();
    return title + "##hs." + (s ? s->id : std::string());
}

const HorizonStyle* hostStyle() {
    Manager* m = currentManager();
    const HorizonHostApi* h = m ? m->host() : nullptr;
    return h && h->style ? h->style() : nullptr;
}

// ---- windows ----
bool begin(const std::string& title, int flags) {
    if (!ok("UI::Begin")) return false;
    if (!push(Open::Window)) return false;
    return ImGui::Begin(windowName(title).c_str(), nullptr, flags);
}
void end() { pop(Open::Window, "UI::End"); }
bool beginChild(const std::string& id, const Vec2& size, bool border, int flags) {
    if (!ok("UI::BeginChild") || !push(Open::Child)) return false;
    return ImGui::BeginChild(id.c_str(), iv(size), border ? ImGuiChildFlags_Borders : 0, flags);
}
void endChild() { pop(Open::Child, "UI::EndChild"); }
void setNextWindowPos(const Vec2& pos, int cond) {
    if (ok("UI::SetNextWindowPos")) ImGui::SetNextWindowPos(iv(pos), cond);
}
void setNextWindowPosPivot(const Vec2& pos, int cond, const Vec2& pivot) {
    if (ok("UI::SetNextWindowPos")) ImGui::SetNextWindowPos(iv(pos), cond, iv(pivot));
}
void setNextWindowSize(const Vec2& size, int cond) {
    if (ok("UI::SetNextWindowSize")) ImGui::SetNextWindowSize(iv(size), cond);
}
void setNextWindowBgAlpha(float a) {
    if (ok("UI::SetNextWindowBgAlpha")) ImGui::SetNextWindowBgAlpha(a);
}
Vec2 getWindowPos() { return ok("UI::GetWindowPos") ? hv(ImGui::GetWindowPos()) : Vec2{0, 0}; }
Vec2 getWindowSize() { return ok("UI::GetWindowSize") ? hv(ImGui::GetWindowSize()) : Vec2{0, 0}; }
Vec2 getScreenSize() { return ok("UI::GetScreenSize") ? hv(ImGui::GetIO().DisplaySize) : Vec2{0, 0}; }
Vec2 getContentRegionAvail() { return ok("UI::GetContentRegionAvail") ? hv(ImGui::GetContentRegionAvail()) : Vec2{0, 0}; }
bool isWindowHovered() { return ok("UI::IsWindowHovered") && ImGui::IsWindowHovered(); }
bool isItemHovered() { return ok("UI::IsItemHovered") && ImGui::IsItemHovered(); }

// ---- text ----
void text(const std::string& t) {
    if (ok("UI::Text")) ImGui::TextUnformatted(t.c_str(), t.c_str() + t.size());
}
void textDisabled(const std::string& t) {
    if (ok("UI::TextDisabled")) ImGui::TextDisabled("%s", t.c_str());
}
void textWrapped(const std::string& t) {
    if (ok("UI::TextWrapped")) ImGui::TextWrapped("%s", t.c_str());
}
void textColored(const Vec4& c, const std::string& t) {
    if (ok("UI::TextColored")) ImGui::TextColored(iv(c), "%s", t.c_str());
}
void labelText(const std::string& label, const std::string& t) {
    if (ok("UI::LabelText")) ImGui::LabelText(label.c_str(), "%s", t.c_str());
}
void bulletText(const std::string& t) {
    if (ok("UI::BulletText")) ImGui::BulletText("%s", t.c_str());
}
void separatorText(const std::string& t) {
    if (ok("UI::SeparatorText")) ImGui::SeparatorText(t.c_str());
}
void setTooltip(const std::string& t) {
    if (ok("UI::SetTooltip")) ImGui::SetTooltip("%s", t.c_str());
}
bool beginTooltip() {
    if (!ok("UI::BeginTooltip")) return false;
    if (!ImGui::BeginTooltip()) return false;
    if (!push(Open::Tooltip)) {
        ImGui::EndTooltip();
        return false;
    }
    return true;
}
void endTooltip() { pop(Open::Tooltip, "UI::EndTooltip"); }

// ---- widgets ----
bool button(const std::string& label, const Vec2& size) { return ok("UI::Button") && ImGui::Button(label.c_str(), iv(size)); }
bool smallButton(const std::string& label) { return ok("UI::SmallButton") && ImGui::SmallButton(label.c_str()); }
bool checkbox(const std::string& label, bool v) {
    if (ok("UI::Checkbox")) ImGui::Checkbox(label.c_str(), &v);
    return v;
}
bool radioButton(const std::string& label, bool active) { return ok("UI::RadioButton") && ImGui::RadioButton(label.c_str(), active); }
bool selectable(const std::string& label, bool selected) { return ok("UI::Selectable") && ImGui::Selectable(label.c_str(), selected); }
int sliderInt(const std::string& label, int v, int mn, int mx, const std::string& fmt) {
    if (ok("UI::SliderInt")) ImGui::SliderInt(label.c_str(), &v, mn, mx, safeFormat(fmt, false).c_str());
    return v;
}
float sliderFloat(const std::string& label, float v, float mn, float mx, const std::string& fmt) {
    if (ok("UI::SliderFloat")) ImGui::SliderFloat(label.c_str(), &v, mn, mx, safeFormat(fmt, true).c_str());
    return v;
}
int inputInt(const std::string& label, int v, int step) {
    if (ok("UI::InputInt")) ImGui::InputInt(label.c_str(), &v, step);
    return v;
}
float inputFloat(const std::string& label, float v, float step) {
    if (ok("UI::InputFloat")) ImGui::InputFloat(label.c_str(), &v, step);
    return v;
}
std::string inputText(const std::string& label, const std::string& value, int maxLength) {
    if (!ok("UI::InputText")) return value;
    const size_t cap = static_cast<size_t>(maxLength < 1 ? 1 : maxLength > 65536 ? 65536 : maxLength) + 1;
    std::vector<char> buf(cap, '\0');
    memcpy(buf.data(), value.data(), value.size() < cap - 1 ? value.size() : cap - 1);
    ImGui::InputText(label.c_str(), buf.data(), cap);
    return std::string(buf.data());
}
std::string inputTextMultiline(const std::string& label, const std::string& value, const Vec2& size, int maxLength) {
    if (!ok("UI::InputTextMultiline")) return value;
    const size_t cap = static_cast<size_t>(maxLength < 1 ? 1 : maxLength > 65536 ? 65536 : maxLength) + 1;
    std::vector<char> buf(cap, '\0');
    memcpy(buf.data(), value.data(), value.size() < cap - 1 ? value.size() : cap - 1);
    ImGui::InputTextMultiline(label.c_str(), buf.data(), cap, iv(size));
    return std::string(buf.data());
}
Vec3 colorEdit3(const std::string& label, const Vec3& c) {
    Vec3 v = c;
    if (ok("UI::ColorEdit3")) ImGui::ColorEdit3(label.c_str(), &v.x);
    return v;
}
Vec4 colorEdit4(const std::string& label, const Vec4& c) {
    Vec4 v = c;
    if (ok("UI::ColorEdit4")) ImGui::ColorEdit4(label.c_str(), &v.x);
    return v;
}
int combo(const std::string& label, int current, const CScriptArray& items) {
    if (!ok("UI::Combo")) return current;
    std::vector<const char*> p;
    for (asUINT i = 0; i < items.GetSize() && i < 256; i++) p.push_back(static_cast<const std::string*>(items.At(i))->c_str());
    ImGui::Combo(label.c_str(), &current, p.data(), static_cast<int>(p.size()));
    return current;
}
void progressBar(float fraction, const Vec2& size, const std::string& overlay) {
    if (ok("UI::ProgressBar")) ImGui::ProgressBar(fraction, iv(size), overlay.empty() ? nullptr : overlay.c_str());
}
bool collapsingHeader(const std::string& label) { return ok("UI::CollapsingHeader") && ImGui::CollapsingHeader(label.c_str()); }
bool treeNode(const std::string& label) {
    if (!ok("UI::TreeNode")) return false;
    if (!ImGui::TreeNode(label.c_str())) return false;
    if (!push(Open::TreeNode)) {
        ImGui::TreePop();
        return false;
    }
    return true;
}
void treePop() { pop(Open::TreeNode, "UI::TreePop"); }

// ---- layout ----
void sameLine(float offset, float spacing) {
    if (ok("UI::SameLine")) ImGui::SameLine(offset, spacing);
}
void separator() {
    if (ok("UI::Separator")) ImGui::Separator();
}
void spacing() {
    if (ok("UI::Spacing")) ImGui::Spacing();
}
void newLine() {
    if (ok("UI::NewLine")) ImGui::NewLine();
}
void dummy(const Vec2& size) {
    if (ok("UI::Dummy")) ImGui::Dummy(iv(size));
}
void indent(float w) {
    if (ok("UI::Indent")) ImGui::Indent(w);
}
void unindent(float w) {
    if (ok("UI::Unindent")) ImGui::Unindent(w);
}
void beginGroup() {
    if (ok("UI::BeginGroup") && push(Open::Group)) ImGui::BeginGroup();
}
void endGroup() { pop(Open::Group, "UI::EndGroup"); }
void pushItemWidth(float w) {
    if (ok("UI::PushItemWidth") && push(Open::ItemWidth)) ImGui::PushItemWidth(w);
}
void popItemWidth() { pop(Open::ItemWidth, "UI::PopItemWidth"); }
void setNextItemWidth(float w) {
    if (ok("UI::SetNextItemWidth")) ImGui::SetNextItemWidth(w);
}
void pushTextWrapPos(float x) {
    if (ok("UI::PushTextWrapPos") && push(Open::TextWrap)) ImGui::PushTextWrapPos(x);
}
void popTextWrapPos() { pop(Open::TextWrap, "UI::PopTextWrapPos"); }
void alignTextToFramePadding() {
    if (ok("UI::AlignTextToFramePadding")) ImGui::AlignTextToFramePadding();
}
Vec2 calcTextSize(const std::string& t) { return ok("UI::CalcTextSize") ? hv(ImGui::CalcTextSize(t.c_str())) : Vec2{0, 0}; }
Vec2 getCursorPos() { return ok("UI::GetCursorPos") ? hv(ImGui::GetCursorPos()) : Vec2{0, 0}; }
void setCursorPos(const Vec2& p) {
    if (ok("UI::SetCursorPos")) ImGui::SetCursorPos(iv(p));
}

// ---- tables ----
bool beginTable(const std::string& id, int columns, int flags) {
    if (!ok("UI::BeginTable")) return false;
    if (columns < 1 || columns > 64) {
        scriptError("UI::BeginTable: 1 to 64 columns");
        return false;
    }
    if (!ImGui::BeginTable(id.c_str(), columns, flags)) return false;
    if (!push(Open::Table)) {
        ImGui::EndTable();
        return false;
    }
    return true;
}
void endTable() { pop(Open::Table, "UI::EndTable"); }
bool inTable(const char* what) {
    if (!ok(what)) return false;
    for (auto it = g_stack.rbegin(); it != g_stack.rend(); ++it) {
        if (*it == Open::Table) return true;
        if (*it == Open::Window || *it == Open::Child) break;
    }
    scriptError(std::string(what) + " outside UI::BeginTable");
    return false;
}
void tableNextRow() {
    if (inTable("UI::TableNextRow")) ImGui::TableNextRow();
}
bool tableNextColumn() { return inTable("UI::TableNextColumn") && ImGui::TableNextColumn(); }
bool tableSetColumnIndex(int i) { return inTable("UI::TableSetColumnIndex") && ImGui::TableSetColumnIndex(i); }
void tableSetupColumn(const std::string& label, int flags, float width) {
    if (inTable("UI::TableSetupColumn")) ImGui::TableSetupColumn(label.c_str(), flags, width);
}
void tableHeadersRow() {
    if (inTable("UI::TableHeadersRow")) ImGui::TableHeadersRow();
}

// ---- style ----
void pushStyleColor(int idx, const Vec4& c) {
    if (!ok("UI::PushStyleColor")) return;
    if (idx < 0 || idx >= ImGuiCol_COUNT) {
        scriptError("UI::PushStyleColor: unknown colour");
        return;
    }
    if (push(Open::Color)) ImGui::PushStyleColor(idx, iv(c));
}
void popStyleColor(int count) {
    for (int i = 0; i < count; i++)
        if (!pop(Open::Color, "UI::PopStyleColor")) return;
}
bool styleVarIsVec2(int idx) {
    switch (idx) {
    case ImGuiStyleVar_WindowPadding:
    case ImGuiStyleVar_FramePadding:
    case ImGuiStyleVar_ItemSpacing:
    case ImGuiStyleVar_ItemInnerSpacing:
    case ImGuiStyleVar_CellPadding:
    case ImGuiStyleVar_WindowMinSize:
    case ImGuiStyleVar_WindowTitleAlign:
    case ImGuiStyleVar_ButtonTextAlign:
    case ImGuiStyleVar_SelectableTextAlign: return true;
    default: return false;
    }
}
void pushStyleVarF(int idx, float v) {
    if (!ok("UI::PushStyleVar")) return;
    if (idx < 0 || idx >= ImGuiStyleVar_COUNT || styleVarIsVec2(idx)) {
        scriptError("UI::PushStyleVar: this style value takes a vec2 (or is unknown)");
        return;
    }
    if (push(Open::Var)) ImGui::PushStyleVar(idx, v);
}
void pushStyleVarV(int idx, const Vec2& v) {
    if (!ok("UI::PushStyleVar")) return;
    if (idx < 0 || idx >= ImGuiStyleVar_COUNT || !styleVarIsVec2(idx)) {
        scriptError("UI::PushStyleVar: this style value takes a float (or is unknown)");
        return;
    }
    if (push(Open::Var)) ImGui::PushStyleVar(idx, iv(v));
}
void popStyleVar(int count) {
    for (int i = 0; i < count; i++)
        if (!pop(Open::Var, "UI::PopStyleVar")) return;
}
void pushIdS(const std::string& id) {
    if (ok("UI::PushID") && push(Open::Id)) ImGui::PushID(id.c_str());
}
void pushIdI(int id) {
    if (ok("UI::PushID") && push(Open::Id)) ImGui::PushID(id);
}
void popId() { pop(Open::Id, "UI::PopID"); }
void beginDisabled(bool disabled) {
    if (ok("UI::BeginDisabled") && push(Open::Disabled)) ImGui::BeginDisabled(disabled);
}
void endDisabled() { pop(Open::Disabled, "UI::EndDisabled"); }
void pushFont(int which) {
    if (!ok("UI::PushFont")) return;
    const HorizonStyle* st = hostStyle();
    void* f = nullptr;
    if (st && st->size >= sizeof(HorizonStyle)) f = which == 1 ? st->font_title : which == 2 ? st->font_mono : st->font_body;
    // no host font yet: push the current one, so PopFont stays balanced
    if (push(Open::Font)) ImGui::PushFont(f ? static_cast<ImFont*>(f) : ImGui::GetFont());
}
void popFont() { pop(Open::Font, "UI::PopFont"); }

// ---- drawing (background list: under every window, the Horizon menu stays on top) ----
void drawText(const Vec2& pos, const Vec4& c, const std::string& t, float size) {
    if (!ok("UI::DrawText")) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (size > 0) dl->AddText(ImGui::GetFont(), size, iv(pos), col32(c), t.c_str(), t.c_str() + t.size());
    else dl->AddText(iv(pos), col32(c), t.c_str(), t.c_str() + t.size());
}
void drawRect(const Vec2& a, const Vec2& b, const Vec4& c, float rounding, float thickness) {
    if (ok("UI::DrawRect")) ImGui::GetBackgroundDrawList()->AddRect(iv(a), iv(b), col32(c), rounding, 0, thickness);
}
void drawRectFilled(const Vec2& a, const Vec2& b, const Vec4& c, float rounding) {
    if (ok("UI::DrawRectFilled")) ImGui::GetBackgroundDrawList()->AddRectFilled(iv(a), iv(b), col32(c), rounding);
}
void drawLine(const Vec2& a, const Vec2& b, const Vec4& c, float thickness) {
    if (ok("UI::DrawLine")) ImGui::GetBackgroundDrawList()->AddLine(iv(a), iv(b), col32(c), thickness);
}
void drawCircleFilled(const Vec2& center, float r, const Vec4& c) {
    if (ok("UI::DrawCircleFilled")) ImGui::GetBackgroundDrawList()->AddCircleFilled(iv(center), r, col32(c));
}

// The same shapes on the current window's draw list: they sit inside the window (above its background, below
// windows opened later), so a script can draw icons in its own overlay. Screen pixels, like the background ones.
Vec2 getCursorScreenPos() { return ok("UI::GetCursorScreenPos") ? hv(ImGui::GetCursorScreenPos()) : Vec2{0, 0}; }
void wDrawLine(const Vec2& a, const Vec2& b, const Vec4& c, float thickness) {
    if (ok("UI::WindowDrawLine")) ImGui::GetWindowDrawList()->AddLine(iv(a), iv(b), col32(c), thickness);
}
void wDrawRectFilled(const Vec2& a, const Vec2& b, const Vec4& c, float rounding) {
    if (ok("UI::WindowDrawRectFilled")) ImGui::GetWindowDrawList()->AddRectFilled(iv(a), iv(b), col32(c), rounding);
}
void wDrawCircleFilled(const Vec2& center, float r, const Vec4& c) {
    if (ok("UI::WindowDrawCircleFilled")) ImGui::GetWindowDrawList()->AddCircleFilled(iv(center), r, col32(c), 24);
}
void wDrawCircle(const Vec2& center, float r, const Vec4& c, float thickness) {
    if (ok("UI::WindowDrawCircle")) ImGui::GetWindowDrawList()->AddCircle(iv(center), r, col32(c), 24, thickness);
}
void wDrawTriangleFilled(const Vec2& a, const Vec2& b, const Vec2& c, const Vec4& col) {
    if (ok("UI::WindowDrawTriangleFilled")) ImGui::GetWindowDrawList()->AddTriangleFilled(iv(a), iv(b), iv(c), col32(col));
}

// ---- host ----
bool isMenuOpen() {
    Manager* m = currentManager();
    const HorizonHostApi* h = m ? m->host() : nullptr;
    return h && h->menu_open && h->menu_open() != 0;
}
float getScale() {
    const HorizonStyle* st = hostStyle();
    return st && st->scale > 0 ? st->scale : 1.0f;
}
Vec4 colorOf(const HorizonColor HorizonStyle::*c, Vec4 fallback) {
    const HorizonStyle* st = hostStyle();
    if (!st || st->size < sizeof(HorizonStyle)) return fallback;
    const HorizonColor& v = st->*c;
    return Vec4{v.r, v.g, v.b, v.a};
}
// ---- widgets ----
// Layout mode (Ctrl+Insert) is on: the host draws its full-screen "##hzlayout" window.
bool isLayoutMode() {
    if (!ImGui::GetCurrentContext()) return false;
    ImGuiWindow* w = ImGui::FindWindowByName("##hzlayout");
    return w && (w->Active || w->WasActive);
}

bool widgetIdOk(const std::string& id) {
    if (id.empty() || id.size() > 24) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ' ')) return false;
    return true;
}

bool addWidget(const std::string& id, const std::string& title, const Vec2& place) {
    if (!requirePerm(PermUi, "UI::AddWidget")) return false;
    Script* s = currentScript();
    if (!s) return false;
    if (!widgetIdOk(id)) {
        scriptError("UI::AddWidget: the id is 1-24 letters, digits, space, '-', '_' or '.'");
        return false;
    }
    for (WidgetDef& w : s->widgets)
        if (w.id == id) { // again: new title and place
            w.title = title.empty() ? id : title.substr(0, 40);
            w.x = place.x;
            w.y = place.y;
            return true;
        }
    if (s->widgets.size() >= 8) {
        scriptError("UI::AddWidget: at most 8 widgets per script");
        return false;
    }
    WidgetDef w;
    w.id = id;
    w.title = title.empty() ? id : title.substr(0, 40);
    w.x = place.x < 0.0f ? 0.0f : place.x > 1.0f ? 1.0f : place.x;
    w.y = place.y < 0.0f ? 0.0f : place.y > 1.0f ? 1.0f : place.y;
    s->widgets.push_back(w);
    return true;
}

// Fades the whole widget, background included. Ignored while the menu or layout mode is open (alpha 1 then).
void widgetAlpha(float alpha) {
    if (!ok("UI::WidgetAlpha")) return;
    if (!g_inWidget) {
        scriptError("UI::WidgetAlpha only works inside RenderWidget()");
        return;
    }
    if (isMenuOpen() || isLayoutMode()) return;
    const float a = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
    if (a >= 0.999f) return;
    // the window background and border are already in the window's draw list: scale their alpha
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < dl->VtxBuffer.Size; i++) {
        ImU32& c = dl->VtxBuffer[i].col;
        const ImU32 na = static_cast<ImU32>(static_cast<float>(c >> IM_COL32_A_SHIFT) * a + 0.5f);
        c = (c & ~IM_COL32_A_MASK) | (na << IM_COL32_A_SHIFT);
    }
    if (push(Open::Var)) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * a);
}

Vec4 accent() { return colorOf(&HorizonStyle::accent, Vec4{1.0f, 0.42f, 0.10f, 1.0f}); }
Vec4 textColor() { return colorOf(&HorizonStyle::text, Vec4{0.93f, 0.94f, 0.95f, 1.0f}); }
Vec4 mutedColor() { return colorOf(&HorizonStyle::muted, Vec4{0.56f, 0.59f, 0.64f, 1.0f}); }

} // namespace

void uiBeginCall() {
    g_stack.clear();
    g_allowed = ImGui::GetCurrentContext() != nullptr;
    g_scopePushed = false;
    if (g_allowed) {
        Script* s = currentScript();
        ImGui::PushID(s ? s->id.c_str() : "script"); // widgets outside windows stay apart per script
        g_scopePushed = true;
    }
}

void uiEndCall() {
    while (!g_stack.empty()) {
        const Open o = g_stack.back();
        g_stack.pop_back();
        close(o);
    }
    if (g_scopePushed) ImGui::PopID();
    g_scopePushed = false;
    g_allowed = false;
}

bool uiAllowed() { return g_allowed; }
bool uiInWidget() { return g_inWidget; }
void uiSetInWidget(bool on) { g_inWidget = on; }

#define R(x)                                                                                                                                         \
    do {                                                                                                                                             \
        if ((x) < 0) return false;                                                                                                                   \
    } while (0)

bool registerUiApi(asIScriptEngine* e) {
    R(e->SetDefaultNamespace("UI"));
    struct EnumValue {
        const char* name;
        int value;
    };
    auto enumeration = [e](const char* name, std::initializer_list<EnumValue> values) {
        if (e->RegisterEnum(name) < 0) return false;
        for (const EnumValue& v : values)
            if (e->RegisterEnumValue(name, v.name, v.value) < 0) return false;
        return true;
    };
    if (!enumeration("WindowFlags", {{"None", 0},
                                     {"NoTitleBar", ImGuiWindowFlags_NoTitleBar},
                                     {"NoResize", ImGuiWindowFlags_NoResize},
                                     {"NoMove", ImGuiWindowFlags_NoMove},
                                     {"NoScrollbar", ImGuiWindowFlags_NoScrollbar},
                                     {"NoCollapse", ImGuiWindowFlags_NoCollapse},
                                     {"AlwaysAutoResize", ImGuiWindowFlags_AlwaysAutoResize},
                                     {"NoBackground", ImGuiWindowFlags_NoBackground},
                                     {"NoSavedSettings", ImGuiWindowFlags_NoSavedSettings},
                                     {"NoMouseInputs", ImGuiWindowFlags_NoMouseInputs},
                                     {"NoFocusOnAppearing", ImGuiWindowFlags_NoFocusOnAppearing},
                                     {"NoNav", ImGuiWindowFlags_NoNav},
                                     {"NoDecoration", ImGuiWindowFlags_NoDecoration},
                                     {"NoInputs", ImGuiWindowFlags_NoInputs}}))
        return false;
    if (!enumeration("Cond", {{"Always", ImGuiCond_Always}, {"Once", ImGuiCond_Once}, {"FirstUseEver", ImGuiCond_FirstUseEver}, {"Appearing", ImGuiCond_Appearing}}))
        return false;
    if (!enumeration("Col", {{"Text", ImGuiCol_Text},
                             {"TextDisabled", ImGuiCol_TextDisabled},
                             {"WindowBg", ImGuiCol_WindowBg},
                             {"ChildBg", ImGuiCol_ChildBg},
                             {"PopupBg", ImGuiCol_PopupBg},
                             {"Border", ImGuiCol_Border},
                             {"FrameBg", ImGuiCol_FrameBg},
                             {"FrameBgHovered", ImGuiCol_FrameBgHovered},
                             {"FrameBgActive", ImGuiCol_FrameBgActive},
                             {"TitleBg", ImGuiCol_TitleBg},
                             {"TitleBgActive", ImGuiCol_TitleBgActive},
                             {"CheckMark", ImGuiCol_CheckMark},
                             {"SliderGrab", ImGuiCol_SliderGrab},
                             {"Button", ImGuiCol_Button},
                             {"ButtonHovered", ImGuiCol_ButtonHovered},
                             {"ButtonActive", ImGuiCol_ButtonActive},
                             {"Header", ImGuiCol_Header},
                             {"HeaderHovered", ImGuiCol_HeaderHovered},
                             {"HeaderActive", ImGuiCol_HeaderActive},
                             {"Separator", ImGuiCol_Separator},
                             {"PlotHistogram", ImGuiCol_PlotHistogram},
                             {"TableHeaderBg", ImGuiCol_TableHeaderBg},
                             {"TableBorderStrong", ImGuiCol_TableBorderStrong},
                             {"TableBorderLight", ImGuiCol_TableBorderLight},
                             {"TableRowBg", ImGuiCol_TableRowBg},
                             {"TableRowBgAlt", ImGuiCol_TableRowBgAlt}}))
        return false;
    if (!enumeration("StyleVar", {{"Alpha", ImGuiStyleVar_Alpha},
                                  {"WindowPadding", ImGuiStyleVar_WindowPadding},
                                  {"WindowRounding", ImGuiStyleVar_WindowRounding},
                                  {"WindowBorderSize", ImGuiStyleVar_WindowBorderSize},
                                  {"ChildRounding", ImGuiStyleVar_ChildRounding},
                                  {"FramePadding", ImGuiStyleVar_FramePadding},
                                  {"FrameRounding", ImGuiStyleVar_FrameRounding},
                                  {"ItemSpacing", ImGuiStyleVar_ItemSpacing},
                                  {"CellPadding", ImGuiStyleVar_CellPadding}}))
        return false;
    if (!enumeration("TableFlags", {{"None", 0},
                                    {"Resizable", ImGuiTableFlags_Resizable},
                                    {"RowBg", ImGuiTableFlags_RowBg},
                                    {"Borders", ImGuiTableFlags_Borders},
                                    {"BordersInnerV", ImGuiTableFlags_BordersInnerV},
                                    {"BordersOuter", ImGuiTableFlags_BordersOuter},
                                    {"SizingFixedFit", ImGuiTableFlags_SizingFixedFit},
                                    {"SizingStretchProp", ImGuiTableFlags_SizingStretchProp},
                                    {"NoHostExtendX", ImGuiTableFlags_NoHostExtendX}}))
        return false;
    if (!enumeration("TableColumnFlags", {{"None", 0}, {"WidthStretch", ImGuiTableColumnFlags_WidthStretch}, {"WidthFixed", ImGuiTableColumnFlags_WidthFixed}}))
        return false;
    if (!enumeration("Font", {{"Body", 0}, {"Title", 1}, {"Mono", 2}})) return false;

    struct Fn {
        const char* decl;
        asSFuncPtr fn;
    };
    const Fn fns[] = {
        {"bool Begin(const string &in title, int flags = 0)", asFUNCTION(begin)},
        {"void End()", asFUNCTION(end)},
        {"bool BeginChild(const string &in id, const vec2 &in size = vec2(0, 0), bool border = false, int flags = 0)", asFUNCTION(beginChild)},
        {"void EndChild()", asFUNCTION(endChild)},
        {"void SetNextWindowPos(const vec2 &in pos, int cond = 0)", asFUNCTION(setNextWindowPos)},
        {"void SetNextWindowPos(const vec2 &in pos, int cond, const vec2 &in pivot)", asFUNCTION(setNextWindowPosPivot)},
        {"void SetNextWindowSize(const vec2 &in size, int cond = 0)", asFUNCTION(setNextWindowSize)},
        {"void SetNextWindowBgAlpha(float alpha)", asFUNCTION(setNextWindowBgAlpha)},
        {"vec2 GetWindowPos()", asFUNCTION(getWindowPos)},
        {"vec2 GetWindowSize()", asFUNCTION(getWindowSize)},
        {"vec2 GetScreenSize()", asFUNCTION(getScreenSize)},
        {"vec2 GetContentRegionAvail()", asFUNCTION(getContentRegionAvail)},
        {"bool IsWindowHovered()", asFUNCTION(isWindowHovered)},
        {"bool IsItemHovered()", asFUNCTION(isItemHovered)},
        {"void Text(const string &in text)", asFUNCTION(text)},
        {"void TextDisabled(const string &in text)", asFUNCTION(textDisabled)},
        {"void TextWrapped(const string &in text)", asFUNCTION(textWrapped)},
        {"void TextColored(const vec4 &in color, const string &in text)", asFUNCTION(textColored)},
        {"void LabelText(const string &in label, const string &in text)", asFUNCTION(labelText)},
        {"void BulletText(const string &in text)", asFUNCTION(bulletText)},
        {"void SeparatorText(const string &in text)", asFUNCTION(separatorText)},
        {"void SetTooltip(const string &in text)", asFUNCTION(setTooltip)},
        {"bool BeginTooltip()", asFUNCTION(beginTooltip)},
        {"void EndTooltip()", asFUNCTION(endTooltip)},
        {"bool Button(const string &in label, const vec2 &in size = vec2(0, 0))", asFUNCTION(button)},
        {"bool SmallButton(const string &in label)", asFUNCTION(smallButton)},
        {"bool Checkbox(const string &in label, bool value)", asFUNCTION(checkbox)},
        {"bool RadioButton(const string &in label, bool active)", asFUNCTION(radioButton)},
        {"bool Selectable(const string &in label, bool selected = false)", asFUNCTION(selectable)},
        {"int SliderInt(const string &in label, int value, int min, int max, const string &in format = \"%d\")", asFUNCTION(sliderInt)},
        {"float SliderFloat(const string &in label, float value, float min, float max, const string &in format = \"%.3f\")", asFUNCTION(sliderFloat)},
        {"int InputInt(const string &in label, int value, int step = 1)", asFUNCTION(inputInt)},
        {"float InputFloat(const string &in label, float value, float step = 0)", asFUNCTION(inputFloat)},
        {"string InputText(const string &in label, const string &in value, int maxLength = 256)", asFUNCTION(inputText)},
        {"string InputTextMultiline(const string &in label, const string &in value, const vec2 &in size = vec2(0, 0), int maxLength = 4096)",
         asFUNCTION(inputTextMultiline)},
        {"vec3 ColorEdit3(const string &in label, const vec3 &in color)", asFUNCTION(colorEdit3)},
        {"vec4 ColorEdit4(const string &in label, const vec4 &in color)", asFUNCTION(colorEdit4)},
        {"int Combo(const string &in label, int current, const array<string> &in items)", asFUNCTION(combo)},
        {"void ProgressBar(float fraction, const vec2 &in size = vec2(-1, 0), const string &in overlay = \"\")", asFUNCTION(progressBar)},
        {"bool CollapsingHeader(const string &in label)", asFUNCTION(collapsingHeader)},
        {"bool TreeNode(const string &in label)", asFUNCTION(treeNode)},
        {"void TreePop()", asFUNCTION(treePop)},
        {"void SameLine(float offset = 0, float spacing = -1)", asFUNCTION(sameLine)},
        {"void Separator()", asFUNCTION(separator)},
        {"void Spacing()", asFUNCTION(spacing)},
        {"void NewLine()", asFUNCTION(newLine)},
        {"void Dummy(const vec2 &in size)", asFUNCTION(dummy)},
        {"void Indent(float width = 0)", asFUNCTION(indent)},
        {"void Unindent(float width = 0)", asFUNCTION(unindent)},
        {"void BeginGroup()", asFUNCTION(beginGroup)},
        {"void EndGroup()", asFUNCTION(endGroup)},
        {"void PushItemWidth(float width)", asFUNCTION(pushItemWidth)},
        {"void PopItemWidth()", asFUNCTION(popItemWidth)},
        {"void SetNextItemWidth(float width)", asFUNCTION(setNextItemWidth)},
        {"void PushTextWrapPos(float x = 0)", asFUNCTION(pushTextWrapPos)},
        {"void PopTextWrapPos()", asFUNCTION(popTextWrapPos)},
        {"void AlignTextToFramePadding()", asFUNCTION(alignTextToFramePadding)},
        {"vec2 CalcTextSize(const string &in text)", asFUNCTION(calcTextSize)},
        {"vec2 GetCursorPos()", asFUNCTION(getCursorPos)},
        {"void SetCursorPos(const vec2 &in pos)", asFUNCTION(setCursorPos)},
        {"bool BeginTable(const string &in id, int columns, int flags = 0)", asFUNCTION(beginTable)},
        {"void EndTable()", asFUNCTION(endTable)},
        {"void TableNextRow()", asFUNCTION(tableNextRow)},
        {"bool TableNextColumn()", asFUNCTION(tableNextColumn)},
        {"bool TableSetColumnIndex(int column)", asFUNCTION(tableSetColumnIndex)},
        {"void TableSetupColumn(const string &in label, int flags = 0, float width = 0)", asFUNCTION(tableSetupColumn)},
        {"void TableHeadersRow()", asFUNCTION(tableHeadersRow)},
        {"void PushStyleColor(Col color, const vec4 &in value)", asFUNCTION(pushStyleColor)},
        {"void PopStyleColor(int count = 1)", asFUNCTION(popStyleColor)},
        {"void PushStyleVar(StyleVar var, float value)", asFUNCTION(pushStyleVarF)},
        {"void PushStyleVar(StyleVar var, const vec2 &in value)", asFUNCTION(pushStyleVarV)},
        {"void PopStyleVar(int count = 1)", asFUNCTION(popStyleVar)},
        {"void PushID(const string &in id)", asFUNCTION(pushIdS)},
        {"void PushID(int id)", asFUNCTION(pushIdI)},
        {"void PopID()", asFUNCTION(popId)},
        {"void BeginDisabled(bool disabled = true)", asFUNCTION(beginDisabled)},
        {"void EndDisabled()", asFUNCTION(endDisabled)},
        {"void PushFont(Font font)", asFUNCTION(pushFont)},
        {"void PopFont()", asFUNCTION(popFont)},
        {"void DrawText(const vec2 &in pos, const vec4 &in color, const string &in text, float size = 0)", asFUNCTION(drawText)},
        {"void DrawRect(const vec2 &in min, const vec2 &in max, const vec4 &in color, float rounding = 0, float thickness = 1)", asFUNCTION(drawRect)},
        {"void DrawRectFilled(const vec2 &in min, const vec2 &in max, const vec4 &in color, float rounding = 0)", asFUNCTION(drawRectFilled)},
        {"void DrawLine(const vec2 &in a, const vec2 &in b, const vec4 &in color, float thickness = 1)", asFUNCTION(drawLine)},
        {"void DrawCircleFilled(const vec2 &in center, float radius, const vec4 &in color)", asFUNCTION(drawCircleFilled)},
        {"vec2 GetCursorScreenPos()", asFUNCTION(getCursorScreenPos)},
        {"void WindowDrawLine(const vec2 &in a, const vec2 &in b, const vec4 &in color, float thickness = 1)", asFUNCTION(wDrawLine)},
        {"void WindowDrawRectFilled(const vec2 &in min, const vec2 &in max, const vec4 &in color, float rounding = 0)", asFUNCTION(wDrawRectFilled)},
        {"void WindowDrawCircleFilled(const vec2 &in center, float radius, const vec4 &in color)", asFUNCTION(wDrawCircleFilled)},
        {"void WindowDrawCircle(const vec2 &in center, float radius, const vec4 &in color, float thickness = 1)", asFUNCTION(wDrawCircle)},
        {"void WindowDrawTriangleFilled(const vec2 &in a, const vec2 &in b, const vec2 &in c, const vec4 &in color)", asFUNCTION(wDrawTriangleFilled)},
        {"bool IsMenuOpen()", asFUNCTION(isMenuOpen)},
        {"bool IsLayoutMode()", asFUNCTION(isLayoutMode)},
        {"bool AddWidget(const string &in id, const string &in title, const vec2 &in place = vec2(1, 0))", asFUNCTION(addWidget)},
        {"void WidgetAlpha(float alpha)", asFUNCTION(widgetAlpha)},
        {"float GetScale()", asFUNCTION(getScale)},
        {"vec4 AccentColor()", asFUNCTION(accent)},
        {"vec4 TextColor()", asFUNCTION(textColor)},
        {"vec4 MutedColor()", asFUNCTION(mutedColor)},
    };
    for (const Fn& f : fns)
        if (e->RegisterGlobalFunction(f.decl, f.fn, asCALL_CDECL) < 0) return false;
    R(e->SetDefaultNamespace(""));
    return true;
}

} // namespace hs
