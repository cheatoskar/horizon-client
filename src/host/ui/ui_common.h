// Drawing helpers shared by ui_menu.cpp and ui_bar.cpp (header only, no state).
#pragma once

#include "ui.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <string>
#include <vector>

namespace horizon {
namespace ui {
namespace detail {

inline Rgba hex(unsigned v, float a = 1.0f) {
    return Rgba{static_cast<float>((v >> 16) & 255u) / 255.0f, static_cast<float>((v >> 8) & 255u) / 255.0f,
                static_cast<float>(v & 255u) / 255.0f, a};
}
inline ImVec4 toVec(const Rgba& c, float alpha = -1.0f) {
    return ImVec4(c.r, c.g, c.b, alpha >= 0.0f ? alpha : c.a);
}
// Draw-list colour; follows the style alpha so fade-ins reach hand-drawn shapes too.
inline ImU32 U(const Rgba& c, float alpha = -1.0f) {
    ImVec4 v = toVec(c, alpha);
    v.w *= ImGui::GetStyle().Alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}
inline ImU32 W(float alpha) { return U(Rgba{1.0f, 1.0f, 1.0f, 1.0f}, alpha); }
inline Rgba lighten(const Rgba& c, float amount) {
    return Rgba{std::min(1.0f, c.r + amount), std::min(1.0f, c.g + amount), std::min(1.0f, c.b + amount), c.a};
}
inline ImFont* F(ImFont* f) { return f ? f : ImGui::GetFont(); }
inline ImVec2 textSize(ImFont* f, const std::string& t) {
    f = F(f);
    return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, t.c_str());
}
inline void drawText(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 col, const std::string& t) {
    f = F(f);
    dl->AddText(f, f->FontSize, p, col, t.c_str());
}
inline void drawTextCentred(ImDrawList* dl, ImFont* f, ImVec2 centre, ImU32 col, const std::string& t) {
    const ImVec2 sz = textSize(f, t);
    drawText(dl, f, ImVec2(centre.x - sz.x * 0.5f, centre.y - sz.y * 0.5f), col, t);
}

// Cuts the text to maxW with "..." at the end.
inline std::string ellipsize(ImFont* f, const std::string& text, float maxW) {
    if (textSize(f, text).x <= maxW) return text;
    std::string cut = text;
    while (!cut.empty()) {
        cut.pop_back();
        // do not stop inside a UTF-8 sequence
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0u) == 0x80u) cut.pop_back();
        if (textSize(f, cut + "...").x <= maxW) return cut + "...";
    }
    return "...";
}

// Greedy word wrap into at most maxLines lines; the last line gets "..." when text was cut.
inline std::vector<std::string> wrapLines(ImFont* f, const std::string& text, float maxW, int maxLines) {
    std::vector<std::string> lines;
    size_t i = 0;
    std::string cur;
    while (i < text.size() && static_cast<int>(lines.size()) < maxLines) {
        size_t j = text.find(' ', i);
        if (j == std::string::npos) j = text.size();
        const std::string word = text.substr(i, j - i);
        i = j + 1;
        if (word.empty()) continue;
        const std::string trial = cur.empty() ? word : cur + " " + word;
        if (cur.empty() || textSize(f, trial).x <= maxW) {
            cur = trial;
            continue;
        }
        lines.push_back(cur);
        cur = word;
        if (static_cast<int>(lines.size()) == maxLines) break;
    }
    if (static_cast<int>(lines.size()) < maxLines) {
        if (!cur.empty()) lines.push_back(cur);
        if (!lines.empty()) lines.back() = ellipsize(f, lines.back(), maxW);
        return lines;
    }
    // cut: the remaining text did not fit
    if (!lines.empty()) {
        std::string last = lines.back();
        // append the rest so ellipsize cuts at the width
        last += " " + cur + (i < text.size() ? " " + text.substr(i) : std::string());
        lines.back() = ellipsize(f, last, maxW);
    }
    return lines;
}

struct AccentColour {
    Rgba main;
    Rgba ink;
};
// One colour of the launcher's accent family, picked from the id.
inline AccentColour accentFor(const std::string& id) {
    static const AccentColour family[6] = {
        {hex(0xff8a45), hex(0x1a0b02)}, {hex(0x4d8dff), hex(0x04122b)}, {hex(0x2ec27e), hex(0x03200f)},
        {hex(0x9d6bff), hex(0x14062e)}, {hex(0xff5468), hex(0x2a0408)}, {hex(0x22c1b0), hex(0x021f1c)}};
    std::uint32_t h = 2166136261u;
    for (unsigned char c : id) {
        h ^= c;
        h *= 16777619u;
    }
    return family[h % 6u];
}

inline std::string initials(const std::string& name) {
    std::string out;
    std::vector<std::string> words;
    std::string w;
    for (unsigned char c : name) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            w.push_back(static_cast<char>(c));
        } else if (!w.empty()) {
            words.push_back(w);
            w.clear();
        }
    }
    if (!w.empty()) words.push_back(w);
    if (words.empty()) return "?";
    if (words.size() == 1) {
        out = words[0].substr(0, 2);
    } else {
        out.push_back(words[0][0]);
        out.push_back(words[1][0]);
    }
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

// A picture icon (rounded square) if the host has one for key; true when drawn.
inline bool drawPictureIcon(ImDrawList* dl, ImVec2 centre, float radius, const std::string& key) {
    const unsigned long long tex = iconFor(key);
    if (!tex) return false;
    const ImVec2 a(centre.x - radius, centre.y - radius), b(centre.x + radius, centre.y + radius);
    dl->AddImageRounded(static_cast<ImTextureID>(tex), a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, radius * 0.45f);
    return true;
}

inline void drawIcon(ImDrawList* dl, ImVec2 centre, float radius, const std::string& id, const std::string& name,
                     ImFont* font) {
    if (drawPictureIcon(dl, centre, radius, id)) return;
    const AccentColour a = accentFor(id);
    dl->AddCircleFilled(centre, radius, U(a.main), 40);
    dl->AddCircle(centre, radius, W(0.18f), 40, 1.0f);
    drawTextCentred(dl, font, ImVec2(centre.x, centre.y - 0.5f), U(a.ink), initials(name));
}

inline const char* statusLabel(PluginStatus s, bool shortForm) {
    switch (s) {
    case PluginStatus::On: return "On";
    case PluginStatus::Off: return "Off";
    case PluginStatus::Failed: return "Failed";
    case PluginStatus::Blocked: return "Blocked";
    case PluginStatus::NeedsRestart: return shortForm ? "Restart" : "Restart needed";
    case PluginStatus::WrongGame: return "Wrong game";
    }
    return "";
}
inline Rgba statusColour(PluginStatus s) {
    const Tokens& t = tokens();
    switch (s) {
    case PluginStatus::On: return t.ok;
    case PluginStatus::Off: return t.muted;
    case PluginStatus::Failed: return t.bad;
    case PluginStatus::Blocked: return t.warn;
    case PluginStatus::NeedsRestart: return t.warn;
    case PluginStatus::WrongGame: return t.muted;
    }
    return t.muted;
}
inline bool canToggle(PluginStatus s) {
    return s != PluginStatus::Failed && s != PluginStatus::WrongGame;
}

// Pill with a tinted background. Returns its width.
inline float chipWidth(ImFont* f, const std::string& text, bool dot) {
    const float s = scale();
    return textSize(f, text).x + 20.0f * s + (dot ? 12.0f * s : 0.0f);
}
inline float drawChip(ImDrawList* dl, ImVec2 p, float h, ImFont* f, const std::string& text, const Rgba& colour,
                      bool dot) {
    const float s = scale();
    const float w = chipWidth(f, text, dot);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U(colour, 0.15f), h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), U(colour, 0.28f), h * 0.5f, 0, 1.0f);
    float x = p.x + 10.0f * s;
    if (dot) {
        dl->AddCircleFilled(ImVec2(x + 3.0f * s, p.y + h * 0.5f), 3.0f * s, U(colour), 12);
        x += 12.0f * s;
    }
    const ImVec2 sz = textSize(f, text);
    drawText(dl, f, ImVec2(x, p.y + (h - sz.y) * 0.5f), U(colour), text);
    return w;
}

// Pill switch. t: animation 0..1 (position).
inline void drawSwitch(ImDrawList* dl, ImVec2 p, ImVec2 size, float t, bool usable, bool hovered) {
    const Tokens& tk = tokens();
    const float r = size.y * 0.5f;
    const Rgba off{0.26f, 0.29f, 0.34f, 1.0f};
    Rgba track = off;
    if (usable) {
        const Rgba on = hovered ? tk.accent2 : tk.accent;
        track = Rgba{off.r + (on.r - off.r) * t, off.g + (on.g - off.g) * t, off.b + (on.b - off.b) * t, 1.0f};
        if (hovered && t < 0.5f) track = lighten(track, 0.05f);
    } else {
        track = Rgba{0.16f, 0.18f, 0.21f, 1.0f};
    }
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), U(track), r);
    const float knobR = r - 3.0f * scale();
    const float x0 = p.x + r, x1 = p.x + size.x - r;
    const ImVec2 c(x0 + (x1 - x0) * t, p.y + r);
    dl->AddCircleFilled(c, knobR, usable ? U(Rgba{1.0f, 1.0f, 1.0f, 1.0f}) : U(Rgba{0.45f, 0.48f, 0.53f, 1.0f}), 24);
}

} // namespace detail
} // namespace ui
} // namespace horizon
