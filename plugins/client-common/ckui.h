// Client mods: shared look (TM Horizon tokens from host->style()), time formatting and small widgets.
// Header only. Include imgui.h first.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

#include "horizon_plugin.h"

namespace ck {

// ---------------------------------------------------------------- times
// "0:24.54" (minutes:seconds.hundredths), "1:02:03.45" past an hour; "-:--.--" unknown.
inline std::string fmtTime(int ms) {
    if (ms < 0) return "-:--.--";
    const int cs = (ms / 10) % 100, s = (ms / 1000) % 60, m = (ms / 60000) % 60, h = ms / 3600000;
    char b[32];
    if (h > 0) snprintf(b, sizeof(b), "%d:%02d:%02d.%02d", h, m, s, cs);
    else snprintf(b, sizeof(b), "%d:%02d.%02d", m, s, cs);
    return b;
}
// "+0.12", "-1.05", "+1:02.30"; "" unknown (INT_MIN).
inline std::string fmtDelta(int ms) {
    if (ms == -2147483647 - 1) return "";
    const char sign = ms < 0 ? '-' : '+';
    const int a = ms < 0 ? -ms : ms;
    char b[32];
    if (a >= 60000) snprintf(b, sizeof(b), "%c%d:%02d.%02d", sign, a / 60000, (a / 1000) % 60, (a / 10) % 100);
    else snprintf(b, sizeof(b), "%c%d.%02d", sign, a / 1000, (a / 10) % 100);
    return b;
}
constexpr int kNoDelta = -2147483647 - 1;

// ---------------------------------------------------------------- colours
#ifdef IMGUI_VERSION
struct Palette {
    ImVec4 accent, accent2, accent3, ok, warn, bad, text, text2, muted, bg, bg2, surface;
    ImVec4 aheadGain, aheadLose, behindGain, behindLose, gold;
};
inline ImVec4 v4(const HorizonColor& c, float a = -1.0f) { return ImVec4(c.r, c.g, c.b, a >= 0.0f ? a : c.a); }
inline ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
inline Palette palette(const HorizonHostApi* host) {
    Palette p;
    const HorizonStyle* s = host && host->style ? host->style() : nullptr;
    if (s) {
        p.accent = v4(s->accent); p.accent2 = v4(s->accent2); p.accent3 = v4(s->accent3);
        p.ok = v4(s->ok); p.warn = v4(s->warn); p.bad = v4(s->bad);
        p.text = v4(s->text); p.text2 = v4(s->text2); p.muted = v4(s->muted);
        p.bg = v4(s->bg); p.bg2 = v4(s->bg2); p.surface = v4(s->surface);
    } else {
        p.accent = ImVec4(1.0f, 0.42f, 0.10f, 1); p.accent2 = ImVec4(1.0f, 0.54f, 0.27f, 1); p.accent3 = ImVec4(1.0f, 0.69f, 0.37f, 1);
        p.ok = ImVec4(0.24f, 0.86f, 0.52f, 1); p.warn = ImVec4(0.95f, 0.76f, 0.31f, 1); p.bad = ImVec4(1.0f, 0.36f, 0.36f, 1);
        p.text = ImVec4(0.933f, 0.941f, 0.953f, 1); p.text2 = ImVec4(0.765f, 0.788f, 0.820f, 1); p.muted = ImVec4(0.557f, 0.592f, 0.639f, 1);
        p.bg = ImVec4(0.039f, 0.047f, 0.067f, 0.97f); p.bg2 = ImVec4(0.063f, 0.075f, 0.102f, 1); p.surface = ImVec4(0.106f, 0.118f, 0.141f, 1);
    }
    // LiveSplit's four states in TrackMania's split colours: blue = faster, red = slower
    p.aheadGain = ImVec4(0.30f, 0.60f, 1.0f, 1.0f); // #4d99ff
    p.aheadLose = mix(p.aheadGain, p.text, 0.45f);
    p.behindLose = p.bad;
    p.behindGain = mix(p.bad, p.text, 0.45f);
    p.gold = ImVec4(1.0f, 0.80f, 0.25f, 1.0f);
    return p;
}
// Colour of a split delta: ahead/behind, and gaining/losing against the previous split's delta.
inline ImVec4 deltaColour(const Palette& p, int delta, int prevDelta, bool gold) {
    if (gold) return p.gold;
    if (delta == kNoDelta) return p.muted;
    const bool ahead = delta <= 0;
    const bool gaining = prevDelta == kNoDelta ? ahead : delta <= prevDelta;
    if (ahead) return gaining ? p.aheadGain : p.aheadLose;
    return gaining ? p.behindGain : p.behindLose;
}
inline ImU32 u32(const ImVec4& c, float alphaMul = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alphaMul));
}
inline ImFont* font(const HorizonHostApi* host, int which) { // 0 body, 1 title, 2 mono
    const HorizonStyle* s = host && host->style ? host->style() : nullptr;
    void* f = !s ? nullptr : which == 1 ? s->font_title : which == 2 ? s->font_mono : s->font_body;
    return f ? static_cast<ImFont*>(f) : ImGui::GetFont();
}
inline float uiScale(const HorizonHostApi* host) {
    const HorizonStyle* s = host && host->style ? host->style() : nullptr;
    return s && s->scale > 0.0f ? s->scale : 1.0f;
}
// Text at a draw-list position in a given font size (scaled from the font's own).
inline void text(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const std::string& t) {
    dl->AddText(f, size, p, col, t.c_str());
}
inline ImVec2 textSize(ImFont* f, float size, const std::string& t) { return f->CalcTextSizeA(size, 1e9f, 0.0f, t.c_str()); }
// A muted one-line hint.
inline void hint(const Palette& p, const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}
// Section label in the accent colour.
inline void section(const Palette& p, const char* s) {
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PushStyleColor(ImGuiCol_Text, p.accent2);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}
#endif

// ---------------------------------------------------------------- host helpers
inline bool abiAtLeast(const HorizonHostApi* h, uint32_t minor) { return h && h->abi_minor >= minor; }
inline std::string getString(const HorizonHostApi* h, const char* key, const char* fallback) {
    char buf[2048];
    const int32_t n = h->get_string(h->plugin, key, fallback, buf, sizeof(buf));
    return std::string(buf, n > 0 ? static_cast<size_t>(n) : 0);
}
inline void toast(const HorizonHostApi* h, uint32_t kind, const char* title, const char* text = "", uint32_t icon = 0) {
    if (!abiAtLeast(h, 3) || !h->notify) return;
    HorizonToast t = {};
    t.size = sizeof(t);
    t.kind = kind;
    t.title = title;
    t.text = text;
    t.icon = icon;
    h->notify(h->plugin, &t);
}
// Site code of the running game for the bridge (docs/client-bridge.md).
inline const char* siteCode(const HorizonHostApi* h) {
    const HorizonGameInfo* g = h && h->game_info ? h->game_info() : nullptr;
    switch (g ? g->game : 0u) {
    case HORIZON_GAME_ESWC: return "tmn";
    case HORIZON_GAME_SUNRISE: return "tms";
    case HORIZON_GAME_ORIGINAL: return "tmo";
    case HORIZON_GAME_TMNF: return "tmnf";
    case HORIZON_GAME_TMUF: return "tmuf";
    default: return "";
    }
}
inline bool isForever(const HorizonHostApi* h) {
    const HorizonGameInfo* g = h && h->game_info ? h->game_info() : nullptr;
    return g && (g->game & HORIZON_GAME_FOREVER) != 0;
}
// URL query value escaping (RFC 3986 unreserved kept).
inline std::string urlEscape(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}
// Nadeo formatting codes removed ("$o$fffName" -> "Name").
inline std::string stripCodes(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] != '$') {
            out += in[i];
            continue;
        }
        if (i + 1 >= in.size()) break;
        const char c = in[i + 1];
        if (c == '$') {
            out += '$';
            i++;
        } else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            i += 3 < in.size() - i ? 3 : in.size() - i - 1;
        } else {
            i++;
        }
    }
    return out;
}

} // namespace ck
