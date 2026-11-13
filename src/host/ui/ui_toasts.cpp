// Toasts in the TM Horizon look: a stack of cards in the top-right corner, below where the bar
// appears. Drawn on the foreground draw list, no window, no input (click-through for the game).
#include "ui_common.h"

#include "horizon_imgui.h"

namespace horizon {
namespace ui {

using namespace detail;

namespace {

const Rgba& kindColour(unsigned kind) {
    const Tokens& t = tokens();
    switch (kind) {
    case 1: return t.ok;
    case 2: return t.warn;
    case 3: return t.bad;
    default: return t.accent;
    }
}

unsigned kindIcon(unsigned kind) {
    switch (kind) {
    case 1: return HZ_ICON_CHECK;
    case 2: return HZ_ICON_TRIANGLE_ALERT;
    case 3: return HZ_ICON_CIRCLE_ALERT;
    default: return HZ_ICON_INFO;
    }
}

std::string utf8Of(unsigned cp) {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

} // namespace

void drawToasts(const std::vector<ToastView>& toasts) {
    if (toasts.empty()) return;
    const float s = scale();
    const Fonts& f = fonts();
    const Tokens& t = tokens();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float width = 340.0f * s, pad = 14.0f * s, gap = 10.0f * s;
    float y = 64.0f * s; // below the bar
    for (const ToastView& v : toasts) {
        const float a = v.alpha;
        if (a <= 0.0f) continue;
        ImFont* titleFont = F(f.bodyBold);
        ImFont* textFont = F(f.small);
        const float textW = width - pad * 2.0f - 26.0f * s;
        const std::string title = ellipsize(titleFont, v.title, textW);
        const std::string text = v.text.empty() ? std::string() : ellipsize(textFont, v.text, textW);
        const std::string owner = v.owner.empty() ? std::string() : ellipsize(textFont, v.owner, textW);
        float h = pad * 2.0f + titleFont->FontSize;
        if (!text.empty()) h += 3.0f * s + textFont->FontSize;
        if (!owner.empty()) h += 3.0f * s + textFont->FontSize;
        const float slide = (1.0f - a) * 24.0f * s;
        const ImVec2 p0(display.x - width - 18.0f * s + slide, y);
        const ImVec2 p1(p0.x + width, p0.y + h);
        const float r = t.radius * s;
        // a plain dark card with a thin border, the same for every kind (no coloured edge, no badge)
        dl->AddRectFilled(p0, p1, U(t.bg2, 0.96f * a), r);
        dl->AddRect(p0, p1, IM_COL32(255, 255, 255, static_cast<int>(22 * a)), r, 0, 1.0f * s);
        // a small icon in the text colour; only an error tints it
        const unsigned icon = v.icon ? v.icon : kindIcon(v.kind);
        const ImVec2 ic(p0.x + pad + 8.0f * s, p0.y + pad + titleFont->FontSize * 0.5f);
        drawTextCentred(dl, F(f.small), ic, U(v.kind == 3 ? kindColour(3) : t.muted, a), utf8Of(icon));
        float x = p0.x + pad + 26.0f * s, ty = p0.y + pad;
        drawText(dl, titleFont, ImVec2(x, ty), U(t.text, a), title);
        ty += titleFont->FontSize + 3.0f * s;
        if (!text.empty()) {
            drawText(dl, textFont, ImVec2(x, ty), U(t.text2, a), text);
            ty += textFont->FontSize + 3.0f * s;
        }
        if (!owner.empty()) drawText(dl, textFont, ImVec2(x, ty), U(t.muted, a), owner);
        y += (h + gap) * (0.4f + 0.6f * a);
    }
}

} // namespace ui
} // namespace horizon
