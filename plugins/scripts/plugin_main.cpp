// Horizon Scripts: the AngelScript script engine as a Horizon plugin.
//
// Scripts live in <game>\horizon\scripts\<id>\ (info.toml + *.as) and run on every supported game with
// the same API (src/api_*.cpp, docs/scripting.md). This file holds the plugin exports and the "Scripts"
// tab: the list with on/off, reload, folders, permissions, errors with file:line, the settings page
// built from [Setting] globals and each script's own RenderMenu() page. The plugin's page in the menu
// is two panels (ABI 1.2): "Scripts" first, then "Options"; there is no DrawMenu export.
// Script widgets (UI::AddWidget + RenderWidget) become host panels (HUD, off until shown): the layout mode
// moves, resizes and minimises them like any module's widget and the host saves their place per widget.
#define HORIZON_PLUGIN_BUILD
#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "horizon_imgui.h"
#include "horizon_plugin.h"

#include "angelscript.h"
#include "engine.h"
#include "fsutil.h"

namespace {

using namespace hs;

const HorizonHostApi* g_host = nullptr;
bool g_imgui = false;
bool g_panels = false;
bool g_enabled = true;
Manager g_mgr;
std::string g_selected;
bool g_cp = false;
ULONGLONG g_cpCheckAt = 0;
uint64_t g_lastFrameUs = 0;
std::string g_initError;

// ---------------------------------------------------------------- style helpers ------------------
const HorizonStyle* style() { return g_host && g_host->style ? g_host->style() : nullptr; }
ImVec4 tok(HorizonColor HorizonStyle::*c, ImVec4 fallback) {
    const HorizonStyle* s = style();
    return (s && s->size >= sizeof(HorizonStyle)) ? horizon_color(s->*c) : fallback;
}
ImVec4 accent() { return tok(&HorizonStyle::accent, ImVec4(1.0f, 0.42f, 0.10f, 1.0f)); }
ImVec4 muted() { return tok(&HorizonStyle::muted, ImVec4(0.56f, 0.59f, 0.64f, 1.0f)); }
ImVec4 text2() { return tok(&HorizonStyle::text2, ImVec4(0.77f, 0.79f, 0.82f, 1.0f)); }
ImVec4 okc() { return tok(&HorizonStyle::ok, ImVec4(0.24f, 0.86f, 0.52f, 1.0f)); }
ImVec4 warn() { return tok(&HorizonStyle::warn, ImVec4(0.95f, 0.76f, 0.31f, 1.0f)); }
ImVec4 bad() { return tok(&HorizonStyle::bad, ImVec4(1.0f, 0.36f, 0.36f, 1.0f)); }
float uiScale() {
    const HorizonStyle* s = style();
    return (s && s->scale > 0.0f) ? s->scale : 1.0f;
}
ImFont* font(void* HorizonStyle::*f) {
    const HorizonStyle* s = style();
    return s && s->size >= sizeof(HorizonStyle) ? static_cast<ImFont*>(s->*f) : nullptr;
}

void mutedText(const std::string& t) {
    ImGui::PushStyleColor(ImGuiCol_Text, muted());
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopStyleColor();
}

void wrapped(const std::string& t, ImVec4 c) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

ImVec4 stateColor(ScriptState s) {
    switch (s) {
    case ScriptState::Running: return okc();
    case ScriptState::Paused:
    case ScriptState::BlockedCp: return warn();
    case ScriptState::Failed:
    case ScriptState::Stopped:
    case ScriptState::Suspended: return bad();
    default: return muted();
    }
}

void chip(const char* label, ImVec4 c) {
    const float s = uiScale();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 pad(8.0f * s, 3.0f * s);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ts.x + pad.x * 2, ts.y + pad.y * 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 0.16f)), size.y * 0.5f);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::ColorConvertFloat4ToU32(c), label);
    ImGui::Dummy(size);
}

void dot(ImVec4 c) {
    const float s = uiScale();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 5.0f * s, p.y + h * 0.5f), 4.0f * s, ImGui::ColorConvertFloat4ToU32(c));
    ImGui::Dummy(ImVec2(12.0f * s, h));
}

void openFolder(const std::wstring& path) {
    makeDirs(path);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- state ---------------------------
void checkCompetitionPatch() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_cpCheckAt) return;
    g_cpCheckAt = now + 1000;
    g_cp = (g_host->game_info()->game & HORIZON_GAME_FOREVER) && GetModuleHandleW(L"TMCompPatch.dll") != nullptr;
}

void updateGate() {
    checkCompetitionPatch();
    GateInput in;
    in.game = g_mgr.game;
    in.online = g_host->is_online() != 0;
    in.cpLoaded = g_cp;
    in.hostAllows = g_host->actions_allowed(g_host->plugin) != 0;
    g_mgr.setGateInput(in);
}

void loadConfig() {
    g_mgr.cfg.callLimitMs = std::clamp(g_host->get_int(g_host->plugin, "CallLimitMs", 25), 5, 200);
    g_mgr.cfg.readLimit = std::clamp(g_host->get_int(g_host->plugin, "ReadLimit", 4000), 500, 20000);
    g_mgr.cfg.hideInCaptures = g_host->get_int(g_host->plugin, "HideInCaptures", 1) != 0;
}

// ---------------------------------------------------------------- settings page ------------------
void drawSettingWidget(Script& s, ScriptSetting& st) {
    const std::string label = (st.meta.name.empty() ? st.var : st.meta.name) + "##" + st.var;
    bool changed = false;
    const int stringType = g_mgr.stringTypeId();
    const SettingMeta& m = st.meta;
    const bool range = m.hasMin && m.hasMax;
    switch (st.typeId) {
    case asTYPEID_BOOL: changed = ImGui::Checkbox(label.c_str(), static_cast<bool*>(st.addr)); break;
    case asTYPEID_INT32: {
        int* v = static_cast<int*>(st.addr);
        if (!m.options.empty()) {
            std::vector<const char*> items;
            for (const std::string& o : m.options) items.push_back(o.c_str());
            changed = ImGui::Combo(label.c_str(), v, items.data(), static_cast<int>(items.size()));
        } else if (range) {
            changed = ImGui::SliderInt(label.c_str(), v, static_cast<int>(m.min), static_cast<int>(m.max));
        } else {
            changed = ImGui::InputInt(label.c_str(), v);
        }
        break;
    }
    case asTYPEID_UINT32: {
        uint32_t* v = static_cast<uint32_t*>(st.addr);
        if (range) {
            const uint32_t lo = static_cast<uint32_t>(m.min), hi = static_cast<uint32_t>(m.max);
            changed = ImGui::SliderScalar(label.c_str(), ImGuiDataType_U32, v, &lo, &hi);
        } else {
            changed = ImGui::InputScalar(label.c_str(), ImGuiDataType_U32, v);
        }
        break;
    }
    case asTYPEID_FLOAT: {
        float* v = static_cast<float*>(st.addr);
        changed = range ? ImGui::SliderFloat(label.c_str(), v, static_cast<float>(m.min), static_cast<float>(m.max), "%.2f")
                        : ImGui::InputFloat(label.c_str(), v, 0.0f, 0.0f, "%.3f");
        break;
    }
    case asTYPEID_DOUBLE: {
        double* v = static_cast<double*>(st.addr);
        changed = range ? ImGui::SliderScalar(label.c_str(), ImGuiDataType_Double, v, &m.min, &m.max, "%.3f")
                        : ImGui::InputDouble(label.c_str(), v, 0.0, 0.0, "%.4f");
        break;
    }
    default:
        if (st.typeId == stringType) {
            std::string* v = static_cast<std::string*>(st.addr);
            char buf[4096]; // like the host settings read
            strncpy_s(buf, sizeof(buf), v->c_str(), _TRUNCATE);
            changed = m.multiline ? ImGui::InputTextMultiline(label.c_str(), buf, sizeof(buf), ImVec2(0, 80.0f * uiScale()))
                                  : ImGui::InputText(label.c_str(), buf, sizeof(buf));
            if (changed) *v = buf;
        }
        break;
    }
    if (!m.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.description.c_str());
    if (changed) g_mgr.settingChanged(s, st);
}

void drawSettings(Script& s) {
    int shown = 0;
    ImGui::PushItemWidth(260.0f * uiScale());
    for (ScriptSetting& st : s.settings) {
        if (st.meta.hidden) continue;
        drawSettingWidget(s, st);
        shown++;
    }
    ImGui::PopItemWidth();
    if (shown && ImGui::SmallButton("Reset to defaults")) g_mgr.resetSettings(s);
}

// ---------------------------------------------------------------- scripts tab ---------------------
void drawMessages(Script& s, bool problems) {
    ImFont* mono = font(&HorizonStyle::font_mono);
    int n = 0;
    for (const Message& m : s.messages) {
        const bool problem = m.kind == Message::Error || m.kind == Message::Warning;
        if (problem != problems) continue;
        n++;
    }
    if (!n) {
        mutedText(problems ? "No errors." : "Nothing logged yet.");
        return;
    }
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float pad = ImGui::GetStyle().WindowPadding.y * 2.0f;
    const float h = std::min(static_cast<float>(n) * lineH * 1.5f + pad + 4.0f * uiScale(), problems ? 220.0f * uiScale() : 160.0f * uiScale());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tok(&HorizonStyle::bg2, ImVec4(0.06f, 0.07f, 0.10f, 1.0f)));
    if (ImGui::BeginChild(problems ? "##problems" : "##log", ImVec2(0, h), ImGuiChildFlags_AlwaysUseWindowPadding)) {
        for (const Message& m : s.messages) {
            const bool problem = m.kind == Message::Error || m.kind == Message::Warning;
            if (problem != problems) continue;
            const ImVec4 c = m.kind == Message::Error ? bad() : m.kind == Message::Warning ? warn() : text2();
            if (!m.where.empty()) {
                if (mono) ImGui::PushFont(mono);
                ImGui::PushStyleColor(ImGuiCol_Text, accent());
                ImGui::TextUnformatted(m.where.c_str());
                ImGui::PopStyleColor();
                if (mono) ImGui::PopFont();
                ImGui::SameLine();
            }
            wrapped(m.text, c);
        }
        if (!problems && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void drawDetails(Script& s) {
    const float sc = uiScale();
    ImFont* title = font(&HorizonStyle::font_title);
    const std::string name = s.hasManifest && !s.manifest.name.empty() ? s.manifest.name : s.id;
    if (title) ImGui::PushFont(title);
    ImGui::TextUnformatted(name.c_str());
    if (title) ImGui::PopFont();
    std::string meta = s.id;
    if (s.hasManifest) {
        meta += "  \xC2\xB7  " + s.manifest.version;
        if (!s.manifest.author.empty()) meta += "  \xC2\xB7  " + s.manifest.author;
    }
    mutedText(meta);
    if (s.hasManifest && !s.manifest.description.empty()) wrapped(s.manifest.description, text2());

    // state
    ImGui::Spacing();
    dot(stateColor(s.state));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, stateColor(s.state));
    ImGui::TextUnformatted(stateLabel(s.state));
    ImGui::PopStyleColor();
    if (!s.stateText.empty()) {
        ImGui::SameLine();
        wrapped(s.stateText, text2());
    }

    // buttons
    ImGui::Spacing();
    if (ImGui::Button(HZ_ICON_REFRESH_CW_U8 " Reload")) g_mgr.reload(s);
    ImGui::SameLine();
    if (ImGui::Button(HZ_ICON_FOLDER_OPEN_U8 " Open folder")) openFolder(s.folder);
    ImGui::SameLine();
    if (ImGui::Button("Data folder")) openFolder(s.dataFolder);
    if (s.loaded()) {
        ImGui::SameLine();
        char b[96];
        snprintf(b, sizeof(b), "last call %.2f ms, slowest %.2f ms", s.lastMs, s.maxMs);
        mutedText(b);
    }

    // permissions, online, games
    if (s.hasManifest) {
        ImGui::Spacing();
        const uint32_t perms[] = {PermUi, PermSettings, PermReadGame, PermGameControl};
        bool any = false;
        for (uint32_t p : perms) {
            if (!(s.manifest.perms & p)) continue;
            if (any) ImGui::SameLine();
            chip(permLabel(p), p == PermGameControl ? warn() : accent());
            any = true;
        }
        if (any) ImGui::SameLine();
        chip(s.manifest.online == OnlineMode::OnlineOk ? "Online OK" : "Solo only", text2());
        ImGui::SameLine();
        chip(gamesText(s.manifest.games).c_str(), muted());
        if (!any) mutedText("No permissions: log, time and its own data files only.");
    }

    ImGui::Spacing();
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Problems (%d)###problems", s.errorCount + s.warningCount);
    ImGui::SeparatorText(hdr);
    drawMessages(s, true);
    if (s.errorCount + s.warningCount && ImGui::SmallButton("Clear")) {
        s.messages.erase(std::remove_if(s.messages.begin(), s.messages.end(), [](const Message& m) { return m.kind == Message::Error || m.kind == Message::Warning; }),
                         s.messages.end());
        s.errorCount = s.warningCount = 0;
    }

    bool hasSettings = false;
    for (const ScriptSetting& st : s.settings) hasSettings |= !st.meta.hidden;
    if (hasSettings) {
        ImGui::SeparatorText("Settings");
        drawSettings(s);
    }
    if (s.cb[CbRenderMenu] && (s.manifest.perms & PermUi)) {
        ImGui::SeparatorText("Script page");
        if (s.state == ScriptState::Running) {
            ImGui::PushID(s.id.c_str());
            g_mgr.renderMenu(s);
            ImGui::PopID();
        } else {
            mutedText("Shown while the script runs.");
        }
    }
    ImGui::SeparatorText("Log");
    drawMessages(s, false);
    (void)sc;
}

void drawScriptsTab() {
    if (!g_mgr.ready()) {
        wrapped("The script engine did not start: " + g_initError, bad());
        return;
    }
    const float s = uiScale();
    auto& scripts = g_mgr.scripts();
    int running = 0;
    for (auto& sp : scripts) running += sp->state == ScriptState::Running;
    char line[128];
    snprintf(line, sizeof(line), "%d script%s, %d running", static_cast<int>(scripts.size()), scripts.size() == 1 ? "" : "s", running);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(line);
    ImGui::SameLine();
    if (ImGui::Button(HZ_ICON_FOLDER_OPEN_U8 " Scripts folder")) openFolder(g_mgr.scriptsDir());
    ImGui::SameLine();
    if (ImGui::Button(HZ_ICON_REFRESH_CW_U8 " Reload all")) {
        g_mgr.rescan();
        g_mgr.reloadAll();
    }
    if (g_cp) wrapped("Competition Patch is loaded: scripts with game control stay off. Reading the game stays allowed.", warn());
    if (g_mgr.online) wrapped("Online: solo-only scripts are paused.", warn());
    ImGui::Spacing();

    if (scripts.empty()) {
        wrapped("No scripts yet. Put a folder with info.toml and a .as file into " + narrow(g_mgr.scriptsDir()) +
                    " - it loads on its own. The guide is docs/scripting.md.",
                text2());
        return;
    }
    if (g_selected.empty() || !g_mgr.find(g_selected)) g_selected = scripts.front()->id;

    const float listW = 250.0f * s;
    float height = ImGui::GetContentRegionAvail().y;
    if (height < 420.0f * s) height = 420.0f * s;
    if (ImGui::BeginChild("##list", ImVec2(listW, height), ImGuiChildFlags_Borders)) {
        for (auto& sp : scripts) {
            Script& sc = *sp;
            ImGui::PushID(sc.id.c_str());
            bool on = sc.enabled;
            if (ImGui::Checkbox("##on", &on)) g_mgr.setEnabled(sc, on);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(on ? "On" : "Off");
            ImGui::SameLine();
            const std::string name = sc.hasManifest && !sc.manifest.name.empty() ? sc.manifest.name : sc.id;
            const ImVec2 start = ImGui::GetCursorPos();
            if (ImGui::Selectable("##sel", g_selected == sc.id, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2.1f))) {
                g_selected = sc.id;
                g_host->set_string(g_host->plugin, "Selected", g_selected.c_str()); // the tab opens on it next time
            }
            ImGui::SetCursorPos(start);
            ImGui::BeginGroup();
            ImGui::TextUnformatted(name.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, stateColor(sc.state));
            std::string sub = stateLabel(sc.state);
            if (sc.errorCount) sub += "  \xC2\xB7  " + std::to_string(sc.errorCount) + (sc.errorCount == 1 ? " error" : " errors");
            ImGui::TextUnformatted(sub.c_str());
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            ImGui::PopID();
            ImGui::Spacing();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##details", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding)) {
        if (Script* sel = g_mgr.find(g_selected)) drawDetails(*sel);
    }
    ImGui::EndChild();
}

void drawGeneralSettings() {
    const float s = uiScale();
    ImGui::PushItemWidth(260.0f * s);
    int limit = static_cast<int>(g_mgr.cfg.callLimitMs);
    if (ImGui::SliderInt("Time limit per call", &limit, 5, 200, "%d ms")) {
        g_mgr.cfg.callLimitMs = limit;
        g_host->set_int(g_host->plugin, "CallLimitMs", limit);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A script call that runs longer is stopped (Main() and global initialisers get 4x).");
    int reads = g_mgr.cfg.readLimit;
    if (ImGui::SliderInt("Game reads per frame", &reads, 500, 20000)) {
        g_mgr.cfg.readLimit = reads;
        g_host->set_int(g_host->plugin, "ReadLimit", reads);
    }
    ImGui::PopItemWidth();
    bool hide = g_mgr.cfg.hideInCaptures;
    if (ImGui::Checkbox("Hide script overlays in photos and videos", &hide)) {
        g_mgr.cfg.hideInCaptures = hide;
        g_host->set_int(g_host->plugin, "HideInCaptures", hide ? 1 : 0);
    }
    ImGui::Spacing();
    mutedText("Scripts folder: " + narrow(g_mgr.scriptsDir()));
    char b[96];
    snprintf(b, sizeof(b), "AngelScript %s", ANGELSCRIPT_VERSION_STRING);
    mutedText(b);
}

void HORIZON_CALL drawScriptsPanel(void*) {
    if (g_imgui && g_host) drawScriptsTab();
}

void HORIZON_CALL drawOptionsPanel(void*) {
    if (g_imgui && g_host) drawGeneralSettings();
}

// ---------------------------------------------------------------- script widgets ------------------
// One host panel per UI::AddWidget of a running script. The entries stay allocated until Shutdown (their
// address is the panel's user pointer); "live" says whether the host has the panel right now.
struct WidgetReg {
    std::string key, script, widget, title;
    float x = 1.0f, y = 0.0f;
    bool live = false;
};
std::vector<std::unique_ptr<WidgetReg>> g_widgets;

std::string slug(const std::string& in, size_t maxLen) {
    std::string o;
    for (char c : in) {
        if (o.size() >= maxLen) break;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        const bool good = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (good) o += c;
        else if (!o.empty() && o.back() != '-') o += '-';
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    return o.empty() ? std::string("x") : o;
}

// "w-<script>-<widget>-<hash>": [a-z0-9-], at most 38 characters, stable for a script id + widget id.
std::string widgetKey(const std::string& script, const std::string& id) {
    uint32_t h = 0x811c9dc5u;
    for (char c : script + "/" + id) h = (h ^ static_cast<unsigned char>(c)) * 0x01000193u;
    char hx[8];
    snprintf(hx, sizeof(hx), "%06x", h & 0xffffffu);
    return "w-" + slug(script, 16) + "-" + slug(id, 12) + "-" + hx;
}

// 0..1 per axis -> the host's slot (left/centre/right, top/middle/bottom).
float slotFraction(float v) { return v < 0.25f ? 0.05f : v > 0.75f ? 0.9f : 0.45f; }

void HORIZON_CALL drawWidget(void* user) {
    WidgetReg* w = static_cast<WidgetReg*>(user);
    if (!g_host || !g_enabled || !g_mgr.ready() || !w || !w->live) return;
    if (Script* s = g_mgr.find(w->script)) g_mgr.renderWidget(*s, w->widget);
}

// Registers the widgets of running scripts and removes the others (game thread, once a frame).
void syncWidgets() {
    if (!g_host || !g_panels || !g_host->register_panel || !g_host->unregister_panel) return;
    std::vector<WidgetReg*> wanted;
    for (auto& sp : g_mgr.scripts()) {
        Script& s = *sp;
        if (s.state != ScriptState::Running || !(s.manifest.perms & PermUi) || !s.cb[CbRenderWidget]) continue;
        for (const WidgetDef& d : s.widgets) {
            const std::string key = widgetKey(s.id, d.id);
            WidgetReg* r = nullptr;
            for (auto& e : g_widgets)
                if (e->key == key) r = e.get();
            if (!r) {
                g_widgets.push_back(std::make_unique<WidgetReg>());
                r = g_widgets.back().get();
                r->key = key;
                r->script = s.id;
                r->widget = d.id;
            }
            const bool changed = !r->live || r->title != d.title || r->x != d.x || r->y != d.y;
            r->title = d.title;
            r->x = d.x;
            r->y = d.y;
            if (changed) {
                HorizonPanelDef def = {};
                def.size = sizeof(def);
                def.key = r->key.c_str();
                def.title = r->title.c_str();
                def.icon = HZ_ICON_SCROLL_TEXT;
                def.flags = HORIZON_PANEL_HUD | HORIZON_PANEL_START_HIDDEN;
                def.draw = drawWidget;
                def.user = r;
                def.default_x = slotFraction(r->x);
                def.default_y = slotFraction(r->y);
                def.default_w = 0.0f; // fits its content: resizing in layout mode scales it
                def.default_h = 0.0f;
                r->live = g_host->register_panel(g_host->plugin, &def) == HORIZON_OK;
            }
            if (r->live) wanted.push_back(r);
        }
    }
    for (auto& e : g_widgets)
        if (e->live && std::find(wanted.begin(), wanted.end(), e.get()) == wanted.end()) {
            g_host->unregister_panel(g_host->plugin, e->key.c_str());
            e->live = false;
        }
}

void dropWidgets() {
    if (g_host && g_host->unregister_panel)
        for (auto& e : g_widgets)
            if (e->live) g_host->unregister_panel(g_host->plugin, e->key.c_str());
    g_widgets.clear();
}

void HORIZON_CALL onPeekHotkey(void*) {
    if (g_enabled) g_mgr.peek();
}

} // namespace

extern "C" {

static const HorizonPluginInfo kInfo = {
    HORIZON_ABI,
    "scripts",
    "Horizon Scripts",
    "0.2.0",
    HORIZON_GAME_ESWC | HORIZON_GAME_SUNRISE | HORIZON_GAME_ORIGINAL | HORIZON_GAME_TMNF | HORIZON_GAME_TMUF,
    HORIZON_CAP_OVERLAY | HORIZON_CAP_GAME_EVENTS | HORIZON_CAP_GAME_CONTROL, // page = two panels (ABI 1.2), no DrawMenu
    HORIZON_FLAG_ONLINE_OK | HORIZON_FLAG_LIVE_TOGGLE,
    "cheatoskar",
    "Runs scripts from horizon\\scripts.",
    HORIZON_ABI_MINOR,
};

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) { return &kInfo; }

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    if (!host || host->abi != HORIZON_ABI) return 1;
    g_host = host;
    g_imgui = horizon_imgui_attach(host);
    if (!g_imgui) host->log(host->plugin, HORIZON_LOG_WARN, "ImGui attach failed: no page, no script UI");
    const HorizonGameInfo* gi = host->game_info();
    const std::wstring gameFolder = widen(gi && gi->game_folder ? gi->game_folder : "");
    const std::wstring scriptsDir = gameFolder + L"horizon\\scripts\\";
    const wchar_t* df = host->data_folder_w ? host->data_folder_w(host->plugin) : nullptr;
    const std::wstring dataDir = df && *df ? std::wstring(df) : gameFolder + L"horizon\\data\\scripts\\";
    loadConfig();
    char sel[96];
    host->get_string(host->plugin, "Selected", "", sel, sizeof(sel));
    g_selected = sel;
    if (!g_mgr.init(host, scriptsDir, dataDir, gi ? gi->game : 0)) {
        g_initError = "AngelScript could not be set up (see the log).";
        host->log(host->plugin, HORIZON_LOG_ERROR, "script engine did not start");
    }
    host->subscribe(host->plugin, HORIZON_EVENT_BIT(HORIZON_EVENT_MAP_LOADED) | HORIZON_EVENT_BIT(HORIZON_EVENT_RACE_START) |
                                      HORIZON_EVENT_BIT(HORIZON_EVENT_RESPAWN) | HORIZON_EVENT_BIT(HORIZON_EVENT_CHECKPOINT) |
                                      HORIZON_EVENT_BIT(HORIZON_EVENT_FINISH) | HORIZON_EVENT_BIT(HORIZON_EVENT_RACE_END) |
                                      HORIZON_EVENT_BIT(HORIZON_EVENT_MENU_ENTERED) | HORIZON_EVENT_BIT(HORIZON_EVENT_ONLINE_CHANGED) |
                                      HORIZON_EVENT_BIT(HORIZON_EVENT_GAME_STATE));
    if (host->abi_minor >= 2 && host->register_panel) {
        HorizonPanelDef d = {};
        d.size = sizeof(d);
        d.key = "scripts";
        d.title = "Scripts";
        d.icon = HZ_ICON_SCROLL_TEXT;
        d.flags = HORIZON_PANEL_MENU_PAGE;
        d.draw = drawScriptsPanel;
        d.default_x = 0.1f;
        d.default_y = 0.1f;
        d.default_w = 0.6f;
        d.default_h = 0.7f;
        g_panels = host->register_panel(host->plugin, &d) == HORIZON_OK;
        d.key = "options";
        d.title = "Options";
        d.icon = HZ_ICON_SLIDERS_HORIZONTAL;
        d.draw = drawOptionsPanel;
        d.default_w = 0.3f;
        d.default_h = 0.3f;
        g_panels = host->register_panel(host->plugin, &d) == HORIZON_OK && g_panels;
    }
    // one hotkey for all scripts: calls OnPeek(), e.g. Clock and Map Info show themselves again for a while
    if (host->register_hotkey) host->register_hotkey(host->plugin, "peek", "Show script widgets", "Ctrl+F4", onPeekHotkey, nullptr);
    if (!g_panels) host->log(host->plugin, HORIZON_LOG_WARN, "this host has no panels (ABI 1.2): scripts run, but the Scripts page needs host 0.2.0 or newer");
    if (g_mgr.ready()) {
        updateGate();
        g_mgr.tick(GetTickCount64()); // first scan: scripts load right away
    }
    host->log(host->plugin, HORIZON_LOG_INFO, "AngelScript %s, scripts folder %s, %u scripts", ANGELSCRIPT_VERSION_STRING,
              narrow(scriptsDir).c_str(), static_cast<unsigned>(g_mgr.scripts().size()));
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    dropWidgets();
    g_mgr.shutdown();
    g_host = nullptr;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_SetEnabled(int32_t enabled) { g_enabled = enabled != 0; }

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawOverlay(void) {
    if (!g_host || !g_mgr.ready()) return;
    const uint64_t us = Budget::qpcMicros();
    if (g_lastFrameUs) g_mgr.frameMs = static_cast<float>(us - g_lastFrameUs) / 1000.0f;
    g_lastFrameUs = us;
    updateGate();
    g_mgr.tick(GetTickCount64());
    syncWidgets();
    reflectionBeginFrame(g_mgr.cfg.readLimit);
    if (!g_imgui || !g_enabled) return;
    const bool capture = g_host->abi_minor >= 2 && g_host->host_flags && (g_host->host_flags() & HORIZON_HOST_FLAG_CAPTURE);
    if (capture && g_mgr.cfg.hideInCaptures) return;
    g_mgr.render();
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_OnEvent(const HorizonEvent* e) {
    if (!g_host || !e || !g_mgr.ready() || !g_enabled) return;
    if (e->type == HORIZON_EVENT_ONLINE_CHANGED) {
        updateGate();
        return;
    }
    updateGate();
    g_mgr.dispatch(*e);
}

} // extern "C"
