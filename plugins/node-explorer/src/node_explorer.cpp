// Node Explorer: a read-only browser for the engine's classes and live objects (Horizon plugin).
//
// Classes come from the engine reflection (CMwClassInfo list, vendored reflect module); objects start at
// the host's engine pointers (App, Race, Challenge) and are walked by member NAME. The plugin only
// reads memory, page-checked and SEH-guarded, with a hard read limit per frame. It never writes, never
// calls game code (not even getters) and installs no hooks, so it stays read-only next to Competition
// Patch too. Its page only draws (and reads) while it is open in the Horizon menu.
#define HORIZON_PLUGIN_BUILD
#include <windows.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "imgui.h"
#include "horizon_imgui.h"
#include "horizon_plugin.h"

#include "catalog.h"
#include "path.h"
#include "reflect/reflect.h"
#include "safe_read.h"
#include "values.h"

namespace {

using namespace nx;

const HorizonHostApi* g_host = nullptr;
bool g_imgui = false;
Engine g_engine = Engine::Legacy;
Catalog g_cat;
bool g_catTried = false;
ULONGLONG g_catRetryAt = 0;

Navigator g_nav;
ObjectView g_view;
bool g_viewOk = false;
std::string g_pathError;
bool g_needRefresh = false;
ULONGLONG g_lastRead = 0;
int g_lastReads = 0;

// settings
int g_refreshMode = 0;  // 0 manual, 1 every second, 2 every frame
int g_readLimit = 2000; // reads per frame
int g_listItems = 8;
bool g_showMethods = false;

// classes tab
char g_classFilter[96] = "";
std::string g_classFilterUsed = "\x01";
std::vector<const ClassEntry*> g_classList;
const ClassEntry* g_selClass = nullptr;
bool g_inherited = true;
bool g_gotoClasses = false;
std::string g_savedClass;   // class picked last time (setting "Class")
int g_tab = 0, g_startTab = 0;
bool g_applyTab = true;     // open the tab of last time once

char g_memberFilter[96] = "";
bool g_cp = false;       // Competition Patch loaded (Forever)
ULONGLONG g_cpCheckAt = 0;
std::string g_dumpMsg;

const HorizonStyle* style() { return g_host && g_host->style ? g_host->style() : nullptr; }
ImVec4 tok(HorizonColor HorizonStyle::*c, ImVec4 fallback) {
    const HorizonStyle* s = style();
    return (s && s->size >= sizeof(HorizonStyle)) ? horizon_color(s->*c) : fallback;
}
ImVec4 accent() { return tok(&HorizonStyle::accent, ImVec4(1.0f, 0.42f, 0.10f, 1.0f)); }
ImVec4 muted() { return tok(&HorizonStyle::muted, ImVec4(0.56f, 0.59f, 0.64f, 1.0f)); }
ImVec4 warn() { return tok(&HorizonStyle::warn, ImVec4(0.95f, 0.76f, 0.31f, 1.0f)); }
ImVec4 bad() { return tok(&HorizonStyle::bad, ImVec4(1.0f, 0.36f, 0.36f, 1.0f)); }
float uiScale() {
    const HorizonStyle* s = style();
    return (s && s->scale > 0.0f) ? s->scale : 1.0f;
}
ImFont* monoFont() {
    const HorizonStyle* s = style();
    return s ? static_cast<ImFont*>(s->font_mono) : nullptr;
}
ImFont* boldFont() {
    const HorizonStyle* s = style();
    return s ? static_cast<ImFont*>(s->font_title) : nullptr;
}

// Tighter spacing for the long lists (the host's style is roomy for its own pages).
struct Compact {
    Compact() {
        const float s = uiScale();
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f * s, 3.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * s, 4.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * s, 2.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 16.0f * s);
    }
    ~Compact() { ImGui::PopStyleVar(4); }
    Compact(const Compact&) = delete;
    Compact& operator=(const Compact&) = delete;
};

void mutedText(const char* t) {
    ImGui::PushStyleColor(ImGuiCol_Text, muted());
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
}

bool link(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_TextLink, accent());
    const bool clicked = ImGui::TextLink(label);
    ImGui::PopStyleColor();
    return clicked;
}

bool containsNoCase(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    auto it = std::search(hay.begin(), hay.end(), needle, needle + strlen(needle),
                          [](char a, char b) { return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b)); });
    return it != hay.end();
}

// ---------------------------------------------------------------- state --------------------------

void saveSettings() {
    g_host->set_int(g_host->plugin, "Refresh", g_refreshMode);
    g_host->set_int(g_host->plugin, "ReadLimit", g_readLimit);
    g_host->set_int(g_host->plugin, "ListItems", g_listItems);
    g_host->set_int(g_host->plugin, "ShowMethods", g_showMethods ? 1 : 0);
}

void savePins() { g_host->set_string(g_host->plugin, "Pins", g_nav.pinsText().c_str()); }

Path g_pending;
bool g_hasPending = false;

// Navigation is applied after the frame's drawing: the history may reallocate while rows still refer to it.
void openPath(const Path& p) {
    g_pending = p;
    g_hasPending = true;
}

void applyPending() {
    if (!g_hasPending) return;
    g_hasPending = false;
    const Path p = g_pending;
    g_nav.open(p);
    g_needRefresh = true;
    g_memberFilter[0] = 0;
    g_host->set_string(g_host->plugin, "Path", p.text().c_str());
}

void openRoot(const char* root) {
    Path p;
    p.root = root;
    openPath(p);
}

bool ensureCatalog() {
    if (g_cat.loaded()) return true;
    const ULONGLONG now = GetTickCount64();
    if (g_catTried && now < g_catRetryAt) return false;
    g_catTried = true;
    g_catRetryAt = now + 2000;
    // reflect's per-exe table has link addresses; a relocated exe (TMLoader) or an unknown build goes
    // through the scan of the registration code instead (timestamp 0 never matches the table).
    const HorizonGameInfo* gi = g_host->game_info();
    const bool atLinkBase = gi && (gi->exe_base == 0x400000 || gi->exe_base == 0);
    auto tryLoad = [](uint32_t ts) {
        return reflect::init(ts) && reflect::ready() && g_cat.load(g_engine) && g_cat.byName("CMwNod") != nullptr;
    };
    if (!(atLinkBase && gi->exe_timestamp && tryLoad(gi->exe_timestamp)) && !tryLoad(0)) {
        g_cat = Catalog();
        return false;
    }
    g_host->log(g_host->plugin, HORIZON_LOG_INFO, "%u classes in the engine reflection", static_cast<unsigned>(g_cat.classes().size()));
    g_classFilterUsed = "\x01";
    if (!g_savedClass.empty()) g_selClass = g_cat.byName(g_savedClass);
    return true;
}

void refreshNow() {
    g_needRefresh = false;
    g_lastRead = GetTickCount64();
    Path* p = g_nav.current();
    g_viewOk = false;
    g_pathError.clear();
    if (!p) return;
    p->rootObj = reinterpret_cast<uintptr_t>(g_host->engine_pointer(rootEngineId(p->root)));
    int failed = -2;
    if (!resolvePath(g_cat, p, &failed)) {
        if (failed == -1) g_pathError = p->root + " is not available right now.";
        else g_pathError = "Not found: " + p->text(failed + 1);
        g_lastReads = readsUsed();
        return;
    }
    ReadOptions opt;
    opt.listItems = g_listItems;
    opt.methods = g_showMethods;
    g_viewOk = readObject(g_cat, p->object(), opt, &g_view);
    if (!g_viewOk) g_pathError = "This object is gone.";
    g_lastReads = readsUsed();
}

void checkCompetitionPatch() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_cpCheckAt) return;
    g_cpCheckAt = now + 1000;
    g_cp = (g_host->game_info()->game & HORIZON_GAME_FOREVER) && GetModuleHandleW(L"TMCompPatch.dll") != nullptr;
}

// ---------------------------------------------------------------- UI pieces ----------------------

void drawBanner() {
    if (!g_cp) return;
    ImVec4 w = warn();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(w.x, w.y, w.z, 0.14f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * uiScale());
    if (ImGui::BeginChild("##cp", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding)) {
        ImGui::PushStyleColor(ImGuiCol_Text, w);
        ImGui::TextUnformatted("Competition Patch is loaded.");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted("Node Explorer only reads memory here too: no writes, no calls.");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void openClass(const ClassEntry* c) {
    g_selClass = c;
    g_gotoClasses = true;
}

// Value cell of one member row (or one list item).
void valueCell(const Row& r, const Path& here) {
    if (r.nod) {
        if (link(r.value.c_str())) openPath(here.child(r.m->name, -1, r.nod));
        if (ImGui::IsItemHovered() && !r.tip.empty()) ImGui::SetTooltip("%s", r.tip.c_str());
        return;
    }
    if (r.hasColor) {
        ImGui::ColorButton("##c", ImVec4(r.color[0], r.color[1], r.color[2], 1.0f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha,
                           ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
        ImGui::SameLine();
    }
    const bool dim = r.state != RowState::Value;
    if (dim) ImGui::PushStyleColor(ImGuiCol_Text, r.state == RowState::Unreadable ? bad() : muted());
    ImFont* mono = dim ? nullptr : monoFont();
    if (mono) ImGui::PushFont(mono);
    ImGui::TextUnformatted(r.value.c_str());
    if (mono) ImGui::PopFont();
    if (dim) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered() && !r.tip.empty()) ImGui::SetTooltip("%s", r.tip.c_str());
}

void rowMenu(const Row& r, const Path& here, const std::string& value) {
    if (ImGui::BeginPopupContextItem("##row")) {
        if (ImGui::MenuItem("Copy value")) ImGui::SetClipboardText(value.c_str());
        if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText((here.text() + "." + r.m->name).c_str());
        if (ImGui::MenuItem("Show class")) openClass(r.m->owner);
        ImGui::EndPopup();
    }
}

void drawObjectTable(const Path& here) {
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    Compact compact;
    if (!ImGui::BeginTable("##members", 3, tf, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Member", ImGuiTableColumnFlags_WidthStretch, 0.34f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.22f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.44f);
    ImGui::TableHeadersRow();
    const ClassEntry* group = g_view.cls;
    int shown = 0;
    for (size_t i = 0; i < g_view.rows.size(); i++) {
        const Row& r = g_view.rows[i];
        if (!containsNoCase(r.m->name, g_memberFilter)) continue;
        if (r.m->owner != group) {
            group = r.m->owner;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, muted());
            ImGui::Text("from %s", group ? group->name.c_str() : "?");
            ImGui::PopStyleColor();
        }
        shown++;
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const bool expandable = r.count > 0 && !r.items.empty();
        ImGuiTreeNodeFlags nf = ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!expandable) nf |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        const bool open = ImGui::TreeNodeEx(r.m->name.c_str(), nf);
        rowMenu(r, here, r.value);
        if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("##row")) {
            if (r.m->offset >= 0) ImGui::SetTooltip("%s.%s  +0x%X", r.m->owner->name.c_str(), r.m->name.c_str(), static_cast<unsigned>(r.m->offset));
            else ImGui::SetTooltip("%s.%s", r.m->owner->name.c_str(), r.m->name.c_str());
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, muted());
        ImGui::TextUnformatted(typeLabel(r.m->ti, r.m->declared ? r.m->declared->name.c_str() : nullptr).c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2);
        valueCell(r, here);
        if (expandable && open) {
            for (size_t k = 0; k < r.items.size(); k++) {
                const ItemRow& it = r.items[k];
                ImGui::PushID(static_cast<int>(k));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Indent();
                mutedText(("[" + std::to_string(k) + "]").c_str());
                ImGui::Unindent();
                ImGui::TableSetColumnIndex(2);
                if (it.nod) {
                    if (link(it.text.c_str())) openPath(here.child(r.m->name, static_cast<int>(k), it.nod));
                } else {
                    ImFont* mono = monoFont();
                    if (mono) ImGui::PushFont(mono);
                    ImGui::TextUnformatted(it.text.c_str());
                    if (mono) ImGui::PopFont();
                }
                ImGui::PopID();
            }
            if (r.count > static_cast<int>(r.items.size())) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Indent();
                mutedText(("+" + std::to_string(r.count - static_cast<int>(r.items.size())) + " more").c_str());
                ImGui::Unindent();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (shown == 0) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        mutedText(g_view.rows.empty() ? "No members." : "No member matches.");
    }
    ImGui::EndTable();
}

void drawObjects(float height) {
    const float s = uiScale();
    // ---- left: where to start, pinned objects
    if (ImGui::BeginChild("##start", ImVec2(220.0f * s, height), ImGuiChildFlags_ResizeX)) {
        mutedText("Start");
        const struct {
            const char* root;
            uint32_t id;
        } roots[] = {{"App", HORIZON_ENGINE_APP}, {"Race", HORIZON_ENGINE_RACE}, {"Challenge", HORIZON_ENGINE_CHALLENGE}};
        for (const auto& r : roots) {
            const bool there = g_host->engine_pointer(r.id) != nullptr;
            ImGui::BeginDisabled(!there);
            if (ImGui::Button(r.root)) openRoot(r.root);
            ImGui::EndDisabled();
            if (!there && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Not available right now.");
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::Spacing();
        mutedText("Pinned");
        const auto& pins = g_nav.pins();
        if (pins.empty()) mutedText("Nothing pinned yet.");
        for (size_t i = 0; i < pins.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            const Path* cur = g_nav.current();
            const std::string t = pins[i].text();
            if (ImGui::Selectable(t.c_str(), cur && cur->sameAs(pins[i]))) openPath(pins[i]);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.c_str());
            if (ImGui::BeginPopupContextItem("##pin")) {
                if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(t.c_str());
                if (ImGui::MenuItem("Unpin")) {
                    g_nav.unpin(i);
                    savePins();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- right: the open object
    if (ImGui::BeginChild("##object", ImVec2(0, height))) {
        Path* cur = g_nav.current();
        ImGui::BeginDisabled(!g_nav.canBack());
        if (ImGui::Button("Back") && g_nav.back()) g_needRefresh = true;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!g_nav.canForward());
        if (ImGui::Button("Forward") && g_nav.forward()) g_needRefresh = true;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!cur);
        if (ImGui::Button("Refresh")) g_needRefresh = true;
        ImGui::SameLine();
        if (ImGui::Button("Copy path") && cur) ImGui::SetClipboardText(cur->text().c_str());
        ImGui::SameLine();
        const bool pinned = cur && g_nav.isPinned(*cur);
        if (ImGui::Button(pinned ? "Unpin" : "Pin") && cur) {
            if (pinned) {
                for (size_t i = 0; i < g_nav.pins().size(); i++)
                    if (g_nav.pins()[i].sameAs(*cur)) g_nav.unpin(i);
            } else {
                g_nav.pin(*cur);
            }
            savePins();
        }
        ImGui::EndDisabled();

        if (!cur) {
            ImGui::Spacing();
            mutedText("Pick a start object on the left.");
        } else {
            // breadcrumb: every part opens that object
            ImGui::Spacing();
            const int depth = static_cast<int>(cur->steps.size());
            for (int i = -1; i < depth; i++) {
                if (i >= 0) {
                    ImGui::SameLine(0, 0);
                    mutedText(".");
                    ImGui::SameLine(0, 0);
                }
                const std::string label = cur->crumb(i);
                ImGui::PushID(i);
                if (i + 1 < depth) {
                    if (link(label.c_str())) openPath(cur->prefix(static_cast<size_t>(i + 1)));
                } else {
                    ImGui::TextUnformatted(label.c_str());
                }
                ImGui::PopID();
            }
            if (!g_pathError.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, bad());
                ImGui::TextUnformatted(g_pathError.c_str());
                ImGui::PopStyleColor();
                mutedText("Values come back when the object exists again (Refresh).");
            } else if (g_viewOk && g_view.cls) {
                if (link(g_view.cls->name.c_str())) openClass(g_view.cls);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show this class");
                ImGui::SameLine();
                char info[128];
                const double ago = static_cast<double>(GetTickCount64() - g_lastRead) / 1000.0;
                if (g_refreshMode == 2) snprintf(info, sizeof(info), "%s  ·  %d reads", fmtPtr(g_view.obj).c_str(), g_lastReads);
                else snprintf(info, sizeof(info), "%s  ·  %d reads  ·  read %.0f s ago", fmtPtr(g_view.obj).c_str(), g_lastReads, ago);
                mutedText(info);
                if (g_view.skipped > 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, warn());
                    ImGui::Text("Read limit reached: %d values skipped. Raise it under Settings.", g_view.skipped);
                    ImGui::PopStyleColor();
                }
                ImGui::SetNextItemWidth(260.0f * s);
                ImGui::InputTextWithHint("##filter", "Filter members", g_memberFilter, sizeof(g_memberFilter));
                drawObjectTable(*cur);
            }
        }
    }
    ImGui::EndChild();
}

void rebuildClassList() {
    if (g_classFilterUsed == g_classFilter) return;
    g_classFilterUsed = g_classFilter;
    g_classList.clear();
    for (const ClassEntry& c : g_cat.classes())
        if (containsNoCase(c.name, g_classFilter)) g_classList.push_back(&c);
}

void drawClasses(float height) {
    const float s = uiScale();
    rebuildClassList();
    if (ImGui::BeginChild("##classlist", ImVec2(260.0f * s, height), ImGuiChildFlags_ResizeX)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##search", "Search classes", g_classFilter, sizeof(g_classFilter));
        char n[64];
        snprintf(n, sizeof(n), "%u of %u classes", static_cast<unsigned>(g_classList.size()), static_cast<unsigned>(g_cat.classes().size()));
        mutedText(n);
        Compact compact;
        if (ImGui::BeginChild("##classscroll")) {
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(g_classList.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                    const ClassEntry* c = g_classList[static_cast<size_t>(i)];
                    ImGui::PushID(i);
                    if (ImGui::Selectable(c->name.c_str(), c == g_selClass)) g_selClass = c;
                    ImGui::PopID();
                }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##class", ImVec2(0, height))) {
        const ClassEntry* c = g_selClass;
        if (!c) {
            mutedText("Pick a class on the left.");
        } else {
            ImFont* big = boldFont();
            if (big) ImGui::PushFont(big);
            ImGui::TextUnformatted(c->name.c_str());
            if (big) ImGui::PopFont();
            char idText[64];
            snprintf(idText, sizeof(idText), "id %08X  ·  %u own members", c->id, static_cast<unsigned>(c->memberCount));
            mutedText(idText);
            // parent chain, each one opens that class
            const std::vector<const ClassEntry*> ch = g_cat.chain(c);
            mutedText("Parents:");
            ImGui::SameLine();
            if (ch.size() <= 1) mutedText("none");
            for (size_t i = 1; i < ch.size(); i++) {
                if (i > 1) {
                    ImGui::SameLine(0, 0);
                    mutedText("  <  ");
                    ImGui::SameLine(0, 0);
                }
                ImGui::PushID(static_cast<int>(i));
                if (link(ch[i]->name.c_str())) g_selClass = ch[i];
                ImGui::PopID();
            }
            ImGui::Checkbox("Inherited members", &g_inherited);
            const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
            Compact compact;
            if (ImGui::BeginTable("##cmembers", 4, tf, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Member", ImGuiTableColumnFlags_WidthStretch, 0.36f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.26f);
                ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthStretch, 0.14f);
                ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthStretch, 0.24f);
                ImGui::TableHeadersRow();
                const size_t levels = g_inherited ? ch.size() : 1;
                int row = 0;
                for (size_t l = 0; l < levels; l++) {
                    for (const MemberEntry& m : g_cat.members(ch[l])) {
                        ImGui::PushID(row++);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(m.name.c_str());
                        ImGui::TableSetColumnIndex(1);
                        const std::string tl = typeLabel(m.ti, m.declared ? m.declared->name.c_str() : nullptr);
                        if (m.declared && m.ti.kind == Kind::Nod) {
                            if (link(tl.c_str())) g_selClass = m.declared;
                        } else {
                            mutedText(tl.c_str());
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("type number %u", m.type);
                        ImGui::TableSetColumnIndex(2);
                        if (isMethod(m.ti.kind)) {
                            mutedText("method");
                        } else if (m.offset < 0) {
                            mutedText("getter");
                        } else {
                            char off[16];
                            snprintf(off, sizeof(off), "+0x%X", static_cast<unsigned>(m.offset));
                            ImFont* mono = monoFont();
                            if (mono) ImGui::PushFont(mono);
                            ImGui::TextUnformatted(off);
                            if (mono) ImGui::PopFont();
                        }
                        ImGui::TableSetColumnIndex(3);
                        mutedText(m.owner->name.c_str());
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::EndChild();
}

void dumpNow() {
    const HorizonGameInfo* gi = g_host->game_info();
    DumpInfo di;
    di.game = gi && gi->game_name ? gi->game_name : "";
    di.exeVersion = gi && gi->exe_version ? gi->exe_version : "";
    di.exeSha256 = gi && gi->exe_sha256 ? gi->exe_sha256 : "";
    di.hostVersion = gi && gi->host_version ? gi->host_version : "";
    di.exeTimestamp = gi ? gi->exe_timestamp : 0;
    const char* tag = "game";
    if (gi) {
        switch (gi->game) {
        case HORIZON_GAME_ESWC: tag = "eswc"; break;
        case HORIZON_GAME_SUNRISE: tag = "sunrise"; break;
        case HORIZON_GAME_TMNF: tag = "tmnf"; break;
        case HORIZON_GAME_TMUF: tag = "tmuf"; break;
        default: break;
        }
    }
    wchar_t name[96];
    swprintf_s(name, L"classes-%hs-%08x.json", tag, di.exeTimestamp);
    const std::wstring path = std::wstring(g_host->data_folder_w(g_host->plugin)) + name;
    const int n = dumpClasses(g_cat, di, path);
    char msg[512];
    if (n < 0) {
        snprintf(msg, sizeof(msg), "Could not write the file.");
    } else {
        snprintf(msg, sizeof(msg), "%d classes written to %sclasses-%s-%08x.json", n, g_host->data_folder(g_host->plugin), tag, di.exeTimestamp);
    }
    g_dumpMsg = msg;
    g_host->log(g_host->plugin, n < 0 ? HORIZON_LOG_WARN : HORIZON_LOG_INFO, "dump: %s", msg);
}

void drawSettings() {
    const float s = uiScale();
    mutedText("Refresh the open object");
    bool changed = false;
    changed |= ImGui::RadioButton("Manual", &g_refreshMode, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Every second", &g_refreshMode, 1);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Every frame", &g_refreshMode, 2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only the open object, and only while this page is open.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(260.0f * s);
    if (ImGui::SliderInt("Reads per frame", &g_readLimit, 200, 20000, "%d", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hard limit. Values past it are skipped until the next refresh.");
    ImGui::SetNextItemWidth(260.0f * s);
    if (ImGui::SliderInt("List items shown", &g_listItems, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp)) changed = true;
    if (ImGui::Checkbox("Show methods", &g_showMethods)) {
        changed = true;
        g_needRefresh = true;
    }
    if (changed) saveSettings();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    mutedText("Class list for tm-modding-knowledge");
    ImGui::BeginDisabled(!g_cat.loaded());
    if (ImGui::Button("Dump classes to JSON")) dumpNow();
    ImGui::EndDisabled();
    if (!g_dumpMsg.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(g_dumpMsg.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const HorizonGameInfo* gi = g_host->game_info();
    char line[256];
    snprintf(line, sizeof(line), "%s %s  ·  %u classes  ·  type numbers: %s", gi && gi->game_name ? gi->game_name : "?",
             gi && gi->exe_version ? gi->exe_version : "", static_cast<unsigned>(g_cat.classes().size()),
             g_engine == Engine::Forever ? "Forever" : "ESWC/Sunrise");
    mutedText(line);
    mutedText("Read-only: never writes, calls or hooks game code.");
}

} // namespace

extern "C" {

static const HorizonPluginInfo kInfo = {
    HORIZON_ABI,
    "node-explorer",
    "Node Explorer",
    "0.1.0",
    HORIZON_GAME_ESWC | HORIZON_GAME_SUNRISE | HORIZON_GAME_TMNF | HORIZON_GAME_TMUF,
    HORIZON_CAP_MENU,
    HORIZON_FLAG_ONLINE_OK | HORIZON_FLAG_LIVE_TOGGLE,
    "cheatoskar",
    "Browse the engine's classes and live objects. Read-only.",
    HORIZON_ABI_MINOR,
};

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) { return &kInfo; }

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    if (!host || host->abi != HORIZON_ABI) return 1;
    g_host = host;
    g_imgui = horizon_imgui_attach(host);
    if (!g_imgui) host->log(host->plugin, HORIZON_LOG_WARN, "ImGui attach failed, no page");
    const HorizonGameInfo* gi = host->game_info();
    g_engine = (gi && (gi->game & HORIZON_GAME_FOREVER)) ? Engine::Forever : Engine::Legacy;
    g_refreshMode = host->get_int(host->plugin, "Refresh", 0);
    if (g_refreshMode < 0 || g_refreshMode > 2) g_refreshMode = 0;
    g_readLimit = std::clamp(host->get_int(host->plugin, "ReadLimit", 2000), 200, 20000);
    g_listItems = std::clamp(host->get_int(host->plugin, "ListItems", 8), 1, 64);
    g_showMethods = host->get_int(host->plugin, "ShowMethods", 0) != 0;
    char buf[4096];
    host->get_string(host->plugin, "Pins", "", buf, sizeof(buf));
    g_nav.setPinsText(buf);
    host->get_string(host->plugin, "Class", "", buf, sizeof(buf));
    g_savedClass = buf;
    if (g_cat.loaded() && !g_savedClass.empty()) g_selClass = g_cat.byName(g_savedClass);
    g_startTab = std::clamp(host->get_int(host->plugin, "Tab", 0), 0, 2);
    g_tab = g_startTab;
    g_applyTab = true;
    host->get_string(host->plugin, "Path", "", buf, sizeof(buf));
    Path last;
    if (buf[0] && parsePath(buf, &last)) {
        g_nav.open(last);
        g_needRefresh = true;
    }
    // The class list is read on the first draw of the page, not here.
    host->log(host->plugin, HORIZON_LOG_INFO, "loaded (read-only), type numbers %s", g_engine == Engine::Forever ? "Forever" : "ESWC/Sunrise");
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    g_view = ObjectView();
    g_classList.clear();
    g_selClass = nullptr;
    g_host = nullptr;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_SetEnabled(int32_t enabled) {
    if (enabled) g_needRefresh = true;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawMenu(void) {
    if (!g_imgui || !g_host) return;
    nx::beginFrame(g_readLimit);
    checkCompetitionPatch();
    drawBanner();
    if (!ensureCatalog()) {
        ImGui::TextUnformatted("The engine's class list is not available in this game build.");
        mutedText("It is read again every few seconds.");
        return;
    }
    const float s = uiScale();
    float height = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - 6.0f * s;
    if (height < 360.0f * s) height = 360.0f * s;
    const ClassEntry* classBefore = g_selClass;
    const int tabBefore = g_tab;
    auto tabFlags = [](int i) { return (g_applyTab && g_startTab == i) ? ImGuiTabItemFlags_SetSelected : 0; };
    if (ImGui::BeginTabBar("##nx")) {
        if (ImGui::BeginTabItem("Objects", nullptr, tabFlags(0))) {
            g_tab = 0;
            const bool due = g_needRefresh || g_refreshMode == 2 || (g_refreshMode == 1 && GetTickCount64() - g_lastRead >= 1000);
            if (due && g_nav.current()) refreshNow();
            drawObjects(height);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Classes", nullptr, (g_gotoClasses ? ImGuiTabItemFlags_SetSelected : 0) | tabFlags(1))) {
            g_tab = 1;
            if (g_selClass) {
                // keep the picked class visible in the list
                if (!containsNoCase(g_selClass->name, g_classFilter)) g_classFilter[0] = 0;
            }
            drawClasses(height);
            ImGui::EndTabItem();
        }
        g_gotoClasses = false;
        if (ImGui::BeginTabItem("Settings", nullptr, tabFlags(2))) {
            g_tab = 2;
            drawSettings();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    g_applyTab = false;
    if (g_tab != tabBefore) g_host->set_int(g_host->plugin, "Tab", g_tab);
    if (g_selClass != classBefore && g_selClass) g_host->set_string(g_host->plugin, "Class", g_selClass->name.c_str());
    applyPending();
}

} // extern "C"
