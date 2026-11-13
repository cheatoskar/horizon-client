// Input Display: steering, gas and brake as the car sees them (arrow keys, a controller, analog bars, a wheel), the
// speed and, where the game keeps them in plain fields, gear and RPM. A HUD widget with five styles, drawn without a
// window background (only the widget itself; the host shows the frame while the menu is open or in layout mode).
//
// Read only: the values come from the local player's car (client-common/gameread) by reflection member names
// (ESWC/Sunrise CSceneToyVehicle.InputSteer/InputGas/InputBrake; TMNF/TMUF CTrackManiaRace.CarSpeedDisplay and
// the car's input fields), every read guarded and checked for plausibility. Nothing is written, no input is
// touched, nothing is hooked.
#define HORIZON_PLUGIN_BUILD
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "imgui.h"
#include "horizon_imgui.h"
#include "horizon_plugin.h"

#include "ckui.h"
#include "gameread.h"

namespace {

const HorizonHostApi* g_host = nullptr;
bool g_ui = false;
bool g_enabled = true;
bool g_forever = false;
ck::LiveMem g_mem;
std::unique_ptr<ck::Reader> g_reader;
ck::Car g_car;  // as read (the "Now" values, the test hook)
ck::Car g_view; // as drawn: digital inputs held steady (see Axis)
bool g_inRace = false;

// a short trail of the inputs (graph style)
constexpr int kTrail = 180;
float g_trailSteer[kTrail] = {}, g_trailGas[kTrail] = {}, g_trailBrake[kTrail] = {};
int g_trailAt = 0;

enum Style { kBars = 0, kKeys = 1, kWheel = 2, kMinimal = 3, kController = 4, kStyleCount = 5 };
// Defaults since 0.2.0: the arrow keys, no graph. Settings saved by 0.1.x keep their look (see migrateDefaults).
constexpr int kDefaultStyle = kKeys;
constexpr int kDefaultGraph = 0;

int setting(const char* k, int fallback) { return g_host->get_int(g_host->plugin, k, fallback); }
float settingF(const char* k, float fallback) { return g_host->get_float(g_host->plugin, k, fallback); }
int styleSetting() {
    const int s = setting("Style", kDefaultStyle);
    return s >= 0 && s < kStyleCount ? s : kDefaultStyle;
}
bool graphOn() { return setting("Graph", kDefaultGraph) != 0; }

// 0.1.x had Bars and the graph as defaults and wrote only what the player changed. Someone who already set
// anything keeps the old look: the old defaults are written for the keys they never touched. A fresh install
// gets the new defaults. Runs once (DefaultsV).
void migrateDefaults() {
    if (setting("DefaultsV", 0) >= 2) return;
    constexpr int kUnset = -12345;
    const char* keys[] = {"Style", "Graph", "ShowSpeed", "Mph", "Gear", "Rpm"};
    bool used = false;
    for (const char* k : keys) used = used || setting(k, kUnset) != kUnset;
    used = used || settingF("Size", -1.0f) >= 0.0f || settingF("Threshold", -1.0f) >= 0.0f;
    if (used) {
        if (setting("Style", kUnset) == kUnset) g_host->set_int(g_host->plugin, "Style", kBars);
        if (setting("Graph", kUnset) == kUnset) g_host->set_int(g_host->plugin, "Graph", 1);
        g_host->log(g_host->plugin, HORIZON_LOG_INFO, "settings from 0.1: kept the old look (style %d, graph %d)", setting("Style", kBars),
                    setting("Graph", 1));
    }
    g_host->set_int(g_host->plugin, "DefaultsV", 2);
}

// Keyboard and pad buttons give 0 or 1. Some games drop such a value to 0 for a frame now and then, which made the
// bars flash. An axis counts as analog once it held values in between for a few frames in a row; until then it is
// drawn as on/off and held on for 100 ms after it went off (a short tap still shows, a one-frame drop does not).
struct Axis {
    int between = 0;           // frames in a row with a value between off and full
    bool analog = false;
    float held = 0.0f;         // -1 / 1 while held on
    ULONGLONG heldUntil = 0;
    float filter(float v, ULONGLONG now) {
        const float m = std::fabs(v);
        if (m > 0.05f && m < 0.95f) {
            if (++between >= 6) analog = true;
        } else {
            between = 0;
        }
        if (analog) return v;
        if (m >= 0.5f) {
            held = v > 0.0f ? 1.0f : -1.0f;
            heldUntil = now + 100;
            return held;
        }
        return now < heldUntil ? held : 0.0f;
    }
};
Axis g_axSteer, g_axGas, g_axBrake;
bool pedalsAnalog() { return g_axGas.analog || g_axBrake.analog; }

std::string speedText(float kmh) {
    const bool mph = setting("Mph", 0) != 0;
    char b[32];
    snprintf(b, sizeof(b), "%d", static_cast<int>(std::lround(mph ? kmh * 0.621371f : kmh)));
    return b;
}
const char* speedUnit() { return setting("Mph", 0) ? "mph" : "km/h"; }

void bar(ImDrawList* dl, ImVec2 a, ImVec2 b, float fill, bool vertical, ImU32 back, ImU32 front, float r) {
    dl->AddRectFilled(a, b, back, r);
    if (fill <= 0.001f) return;
    if (vertical) dl->AddRectFilled(ImVec2(a.x, b.y - (b.y - a.y) * fill), b, front, r);
    else dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * fill, b.y), front, r);
}

// The steering bar: centre line, fill from the centre to the side.
void steerBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float steer, const ck::Palette& p, float r) {
    dl->AddRectFilled(a, b, ck::u32(p.surface), r);
    const float mid = (a.x + b.x) * 0.5f;
    const float x = mid + (b.x - a.x) * 0.5f * steer;
    if (steer < -0.01f) dl->AddRectFilled(ImVec2(x, a.y), ImVec2(mid, b.y), ck::u32(p.accent), r);
    if (steer > 0.01f) dl->AddRectFilled(ImVec2(mid, a.y), ImVec2(x, b.y), ck::u32(p.accent), r);
    dl->AddLine(ImVec2(mid, a.y - 2), ImVec2(mid, b.y + 2), ck::u32(p.text, 0.6f), 2.0f);
}

void key(ImDrawList* dl, ImVec2 a, float size, bool on, float amount, const char* label, const ImVec4& col, const ck::Palette& p, ImFont* f) {
    const ImVec2 b(a.x + size, a.y + size);
    const float r = size * 0.18f;
    dl->AddRectFilled(a, b, ck::u32(p.surface, 0.95f), r);
    if (amount > 0.01f) dl->AddRectFilled(a, b, ck::u32(col, 0.25f + 0.65f * amount), r);
    dl->AddRect(a, b, on ? ck::u32(col) : ck::u32(p.muted, 0.5f), r, 0, on ? 2.5f : 1.2f);
    const float fs = size * 0.42f;
    const ImVec2 ts = ck::textSize(f, fs, label);
    ck::text(dl, f, fs, ImVec2(a.x + (size - ts.x) * 0.5f, a.y + (size - ts.y) * 0.5f), on ? ck::u32(p.text) : ck::u32(p.muted), label);
}

float speedFont(float s, bool big) { return (big ? 40.0f : 28.0f) * s; }
// Height of speedBlock: the number and the unit line.
float speedBlockH(float s, bool big) {
    return ck::textSize(ck::font(g_host, 1), speedFont(s, big), "0").y + ck::textSize(ck::font(g_host, 0), 13.0f * s, "km/h").y;
}

void speedBlock(ImDrawList* dl, ImVec2 at, float w, const ck::Palette& p, float s, bool big) {
    ImFont* title = ck::font(g_host, 1);
    ImFont* body = ck::font(g_host, 0);
    const float fs = speedFont(s, big);
    const std::string v = g_view.haveSpeed ? speedText(g_view.speedKmh) : std::string("-");
    const ImVec2 ts = ck::textSize(title, fs, v);
    ck::text(dl, title, fs, ImVec2(at.x + (w - ts.x) * 0.5f, at.y), ck::u32(p.text), v);
    const float us = 13.0f * s;
    std::string unit = speedUnit();
    if (setting("Gear", 1) && g_view.haveGear) unit += "   gear " + std::to_string(g_view.gear);
    const ImVec2 uz = ck::textSize(body, us, unit);
    ck::text(dl, body, us, ImVec2(at.x + (w - uz.x) * 0.5f, at.y + ts.y), ck::u32(p.muted), unit);
}

void rpmArc(ImDrawList* dl, ImVec2 c, float radius, const ck::Palette& p, float s) {
    if (!(setting("Rpm", 1) && g_view.haveRpm)) return;
    const float maxRpm = 11000.0f;
    const float t = g_view.rpm / maxRpm > 1.0f ? 1.0f : g_view.rpm / maxRpm;
    const float a0 = 3.14159f * 0.75f, a1 = 3.14159f * 2.25f;
    dl->PathArcTo(c, radius, a0, a1, 48);
    dl->PathStroke(ck::u32(p.surface), 0, 6.0f * s);
    dl->PathArcTo(c, radius, a0, a0 + (a1 - a0) * t, 48);
    dl->PathStroke(ck::u32(t > 0.85f ? p.bad : p.accent2), 0, 6.0f * s);
}

// ---- sizes: the widget is exactly as large as what it draws (the host fits the window to it) -----------
constexpr float kPedalW = 26, kPedalH = 86, kGraphGap = 12, kGraphH = 44;  // bars
constexpr float kKey = 46, kKeyGap = 8;                                    // keys
constexpr float kWheelR = 50, kWheelArc = 12, kWheelBarW = 18, kWheelBarGap = 8; // wheel
constexpr float kTriW = 50, kTriH = 76, kPadW = 44, kPadGap = 6;           // controller

bool wheelBars() { return pedalsAnalog(); } // digital pedals: no side bars (they only blinked on and off)

ImVec2 contentSize(int style, float s) {
    const bool speed = setting("ShowSpeed", 1) != 0;
    switch (style) {
    case kKeys: {
        const float w = (kKey * 3 + kKeyGap * 2) * s;
        float h = (kKey * 2 + kKeyGap) * s;
        if (speed) h += 4.0f * s + speedBlockH(s, false);
        return ImVec2(w, h);
    }
    case kWheel: {
        float w = (kWheelR + kWheelArc) * 2.0f * s;
        if (wheelBars()) w += 2.0f * (kWheelBarW + kWheelBarGap) * s;
        float h = (kWheelR + kWheelArc) * 2.0f * s;
        if (speed) h += 6.0f * s + speedBlockH(s, false);
        return ImVec2(w, h);
    }
    case kMinimal: return ImVec2(300.0f * s, 32.0f * s);
    case kController: {
        const float w = (kTriW * 2 + kPadW + kPadGap * 2) * s;
        float h = kTriH * s;
        if (speed) h += 8.0f * s + speedBlockH(s, false);
        return ImVec2(w, h);
    }
    default: {
        const float w = (speed ? 240.0f : 160.0f) * s;
        float h = (26 + kPedalH) * s;
        if (graphOn()) h += (kGraphGap + kGraphH) * s;
        return ImVec2(w, h);
    }
    }
}

void drawBars(ImDrawList* dl, ImVec2 o, float w, const ck::Palette& p, float s) {
    const float r = 6.0f * s;
    steerBar(dl, ImVec2(o.x, o.y), ImVec2(o.x + w, o.y + 14 * s), g_view.steer, p, r);
    const float pedalW = kPedalW * s, pedalH = kPedalH * s, top = o.y + 26 * s;
    bar(dl, ImVec2(o.x, top), ImVec2(o.x + pedalW, top + pedalH), g_view.brake, true, ck::u32(p.surface), ck::u32(p.bad), r);
    bar(dl, ImVec2(o.x + w - pedalW, top), ImVec2(o.x + w, top + pedalH), g_view.gas, true, ck::u32(p.surface), ck::u32(p.ok), r);
    const float mid = w - 2 * pedalW - 20 * s;
    if (setting("ShowSpeed", 1)) speedBlock(dl, ImVec2(o.x + pedalW + 10 * s, top + 8 * s), mid, p, s, true);
    if (graphOn()) {
        // the last three seconds: steer (orange, around the middle), gas (green) and brake (red) as lines
        const float gy = top + pedalH + kGraphGap * s, gh = kGraphH * s;
        dl->AddRectFilled(ImVec2(o.x, gy), ImVec2(o.x + w, gy + gh), ck::u32(p.surface, 0.7f), r);
        auto line = [&](const float* v, ImU32 col, bool centred) {
            ImVec2 prev;
            for (int i = 0; i < kTrail; i++) {
                const int k = (g_trailAt + i) % kTrail;
                const float x = o.x + w * static_cast<float>(i) / (kTrail - 1);
                const float y = centred ? gy + gh * 0.5f - v[k] * gh * 0.45f : gy + gh - 2 * s - v[k] * (gh - 4 * s);
                const ImVec2 pt(x, y);
                if (i) dl->AddLine(prev, pt, col, 1.6f * s);
                prev = pt;
            }
        };
        line(g_trailGas, ck::u32(p.ok, 0.9f), false);
        line(g_trailBrake, ck::u32(p.bad, 0.9f), false);
        line(g_trailSteer, ck::u32(p.accent), true);
    }
}

void drawKeys(ImDrawList* dl, ImVec2 o, float w, const ck::Palette& p, float s) {
    const float th = settingF("Threshold", 0.2f);
    const float size = kKey * s, gap = kKeyGap * s;
    ImFont* f = ck::font(g_host, 1);
    const float x0 = o.x + (w - (size * 3 + gap * 2)) * 0.5f;
    key(dl, ImVec2(x0 + size + gap, o.y), size, g_view.gas > th, g_view.gas, HZ_ICON_CHEVRON_UP_U8, p.ok, p, f);
    key(dl, ImVec2(x0, o.y + size + gap), size, g_view.steer < -th, -g_view.steer, HZ_ICON_CHEVRON_LEFT_U8, p.accent, p, f);
    key(dl, ImVec2(x0 + size + gap, o.y + size + gap), size, g_view.brake > th, g_view.brake, HZ_ICON_CHEVRON_DOWN_U8, p.bad, p, f);
    key(dl, ImVec2(x0 + 2 * (size + gap), o.y + size + gap), size, g_view.steer > th, g_view.steer, HZ_ICON_CHEVRON_RIGHT_U8, p.accent, p, f);
    if (setting("ShowSpeed", 1)) speedBlock(dl, ImVec2(o.x, o.y + 2 * size + gap + 4 * s), w, p, s, false);
}

void drawWheel(ImDrawList* dl, ImVec2 o, float w, const ck::Palette& p, float s) {
    const float radius = kWheelR * s;
    const ImVec2 c(o.x + w * 0.5f, o.y + (kWheelR + kWheelArc) * s);
    rpmArc(dl, c, radius + 9 * s, p, s);
    // wheel turned by the steering (up to 90 degrees)
    const float ang = g_view.steer * 1.5708f;
    dl->AddCircle(c, radius, ck::u32(p.text2), 48, 7.0f * s);
    const float cs = cosf(ang), sn = sinf(ang);
    auto rot = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
    dl->AddLine(rot(-radius, 0), rot(radius, 0), ck::u32(p.text2), 6.0f * s);
    dl->AddLine(rot(0, 0), rot(0, radius), ck::u32(p.text2), 6.0f * s);
    dl->AddCircleFilled(rot(0, -radius), 6.0f * s, ck::u32(p.accent));
    dl->AddCircleFilled(c, 12 * s, ck::u32(p.surface));
    if (wheelBars()) {
        const float pw = kWheelBarW * s, ph = 2 * radius;
        bar(dl, ImVec2(o.x, c.y - radius), ImVec2(o.x + pw, c.y - radius + ph), g_view.brake, true, ck::u32(p.surface), ck::u32(p.bad), 5 * s);
        bar(dl, ImVec2(o.x + w - pw, c.y - radius), ImVec2(o.x + w, c.y - radius + ph), g_view.gas, true, ck::u32(p.surface), ck::u32(p.ok), 5 * s);
    }
    if (setting("ShowSpeed", 1)) speedBlock(dl, ImVec2(o.x, c.y + (kWheelR + kWheelArc) * s + 6 * s), w, p, s, false);
}

void drawMinimal(ImDrawList* dl, ImVec2 o, float w, const ck::Palette& p, float s) {
    ImFont* title = ck::font(g_host, 1);
    const float h = 30 * s;
    float x = o.x;
    if (setting("ShowSpeed", 1)) {
        const std::string v = (g_view.haveSpeed ? speedText(g_view.speedKmh) : std::string("-"));
        ck::text(dl, title, 26 * s, ImVec2(x, o.y), ck::u32(p.text), v);
        x += 70 * s;
    }
    const float bw = (o.x + w - x - 16 * s) / 3.0f;
    bar(dl, ImVec2(x, o.y + 8 * s), ImVec2(x + bw, o.y + h - 6 * s), g_view.brake, false, ck::u32(p.surface), ck::u32(p.bad), 4 * s);
    steerBar(dl, ImVec2(x + bw + 8 * s, o.y + 8 * s), ImVec2(x + 2 * bw + 8 * s, o.y + h - 6 * s), g_view.steer, p, 4 * s);
    bar(dl, ImVec2(x + 2 * bw + 16 * s, o.y + 8 * s), ImVec2(x + 3 * bw + 16 * s, o.y + h - 6 * s), g_view.gas, false, ck::u32(p.surface), ck::u32(p.ok), 4 * s);
}

// A steering triangle: tip at tipX, the flat side at baseX, filled from the flat side towards the tip by amount.
// Points go clockwise on screen (Dear ImGui's anti-aliasing needs it).
void steerTriangle(ImDrawList* dl, float tipX, float baseX, float cy, float hh, float amount, const ck::Palette& p, float s) {
    const bool left = tipX < baseX;
    const ImVec2 tip(tipX, cy), top(baseX, cy - hh), bottom(baseX, cy + hh);
    if (left) dl->AddTriangleFilled(tip, top, bottom, ck::u32(p.surface, 0.85f));
    else dl->AddTriangleFilled(top, tip, bottom, ck::u32(p.surface, 0.85f));
    const float a = std::min(1.0f, std::max(0.0f, amount));
    if (a > 0.01f) {
        const ImU32 col = ck::u32(p.accent);
        if (a >= 0.99f) {
            if (left) dl->AddTriangleFilled(tip, top, bottom, col);
            else dl->AddTriangleFilled(top, tip, bottom, col);
        } else {
            const float xf = baseX + (tipX - baseX) * a, hf = hh * (1.0f - a);
            ImVec2 q[4];
            if (left) {
                q[0] = top; q[1] = bottom; q[2] = ImVec2(xf, cy + hf); q[3] = ImVec2(xf, cy - hf);
            } else {
                q[0] = top; q[1] = ImVec2(xf, cy - hf); q[2] = ImVec2(xf, cy + hf); q[3] = bottom;
            }
            dl->AddConvexPolyFilled(q, 4, col);
        }
    }
    const ImU32 edge = a > 0.01f ? ck::u32(p.accent) : ck::u32(p.muted, 0.6f);
    if (left) dl->AddTriangle(tip, top, bottom, edge, (a > 0.01f ? 2.0f : 1.4f) * s);
    else dl->AddTriangle(top, tip, bottom, edge, (a > 0.01f ? 2.0f : 1.4f) * s);
}

// A pedal box: filled from the side facing the middle (gas grows up, brake grows down).
void pedalBox(ImDrawList* dl, ImVec2 a, ImVec2 b, float amount, bool growsUp, const ImVec4& col, const ck::Palette& p, float s) {
    const float r = 6.0f * s;
    dl->AddRectFilled(a, b, ck::u32(p.surface, 0.85f), r);
    const float v = std::min(1.0f, std::max(0.0f, amount));
    if (v > 0.01f) {
        const float h = (b.y - a.y) * v;
        if (growsUp) dl->AddRectFilled(ImVec2(a.x, b.y - h), b, ck::u32(col), r);
        else dl->AddRectFilled(a, ImVec2(b.x, a.y + h), ck::u32(col), r);
    }
    dl->AddRect(a, b, v > 0.01f ? ck::u32(col) : ck::u32(p.muted, 0.6f), r, 0, (v > 0.01f ? 2.0f : 1.4f) * s);
}

// Controller: the classic input display. Steering triangles left and right, gas above and brake below in the middle.
void drawController(ImDrawList* dl, ImVec2 o, float w, const ck::Palette& p, float s) {
    const float tw = kTriW * s, th = kTriH * s, pw = kPadW * s, gap = kPadGap * s;
    const float total = 2 * tw + pw + 2 * gap;
    const float x0 = o.x + (w - total) * 0.5f;
    const float cy = o.y + th * 0.5f, hh = th * 0.5f;
    steerTriangle(dl, x0, x0 + tw, cy, hh, -g_view.steer, p, s);
    steerTriangle(dl, x0 + total, x0 + total - tw, cy, hh, g_view.steer, p, s);
    const float px = x0 + tw + gap;
    pedalBox(dl, ImVec2(px, o.y), ImVec2(px + pw, cy - gap * 0.5f), g_view.gas, true, p.ok, p, s);
    pedalBox(dl, ImVec2(px, cy + gap * 0.5f), ImVec2(px + pw, o.y + th), g_view.brake, false, p.bad, p, s);
    if (setting("ShowSpeed", 1)) speedBlock(dl, ImVec2(o.x, o.y + th + 8 * s), w, p, s, false);
}

void HORIZON_CALL drawInputs(void*) {
    if (!g_ui || !g_host) return;
    const ck::Palette p = ck::palette(g_host);
    const float s = ck::uiScale(g_host) * settingF("Size", 1.0f);
    const int style = styleSetting();
    const ImVec2 size = contentSize(style, s);
    const float w = size.x;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!g_car.ok) {
        // Nothing to show: with the menu closed the widget stays empty (no box, no text over the game). With the menu
        // open it shows its shape at rest and why, so it can be found and placed.
        if (!(g_host->menu_open && g_host->menu_open())) return;
        g_view = ck::Car();
        dl->PushClipRect(o, ImVec2(o.x + size.x, o.y + size.y), true);
    }
    const unsigned int vtx0 = static_cast<unsigned int>(dl->VtxBuffer.Size);
    switch (style) {
    case kKeys: drawKeys(dl, o, w, p, s); break;
    case kWheel: drawWheel(dl, o, w, p, s); break;
    case kMinimal: drawMinimal(dl, o, w, p, s); break;
    case kController: drawController(dl, o, w, p, s); break;
    default: drawBars(dl, o, w, p, s); break;
    }
    if (!g_car.ok) {
        // dimmed shape, then the reason on top of it
        for (int i = static_cast<int>(vtx0); i < dl->VtxBuffer.Size; i++) {
            ImU32& c = dl->VtxBuffer[i].col;
            const unsigned a = (c >> IM_COL32_A_SHIFT) & 0xFF;
            c = (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a * 0.35f) << IM_COL32_A_SHIFT);
        }
        const char* why = g_inRace ? "No data" : "Waiting for race";
        ImFont* f = ck::font(g_host, 0);
        const float fs = 14.0f * ck::uiScale(g_host);
        const ImVec2 ts = ck::textSize(f, fs, why);
        ck::text(dl, f, fs, ImVec2(o.x + (size.x - ts.x) * 0.5f, o.y + (size.y - ts.y) * 0.5f), ck::u32(p.text2), why);
        dl->PopClipRect();
    }
}

} // namespace

extern "C" {

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) {
    static const HorizonPluginInfo info = {HORIZON_ABI,
                                           "input-display",
                                           "Input Display",
                                           "0.2.0",
                                           HORIZON_GAME_ALL,
                                           HORIZON_CAP_MENU | HORIZON_CAP_OVERLAY,
                                           HORIZON_FLAG_ONLINE_OK | HORIZON_FLAG_LIVE_TOGGLE,
                                           "cheatoskar",
                                           "Steering, gas, brake and speed on screen.",
                                           HORIZON_ABI_MINOR};
    return &info;
}

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    if (!host || host->abi != HORIZON_ABI) return 1;
    g_host = host;
    g_enabled = true;
    g_car = ck::Car();
    g_view = ck::Car();
    g_axSteer = Axis();
    g_axGas = Axis();
    g_axBrake = Axis();
    g_ui = horizon_imgui_attach(host);
    g_forever = ck::isForever(host);
    migrateDefaults();
    const bool reflect = ck::liveReflectInit(host);
    std::string missing;
    const ck::Layout L = ck::resolveLayout(g_forever ? ck::Engine::Forever : ck::Engine::Legacy, &ck::liveOffset, &missing);
    g_reader.reset(new ck::Reader(L));
    host->log(host->plugin, HORIZON_LOG_INFO, "car read %s (reflection %s); %s", L.vNamed ? "by member names" : "at default offsets, checked",
              reflect ? "ready" : "not available", missing.c_str());
    if (ck::abiAtLeast(host, 2) && host->register_panel) {
        HorizonPanelDef d = {};
        d.size = sizeof(d);
        d.key = "inputs";
        d.title = "Inputs";
        d.icon = HZ_ICON_KEYBOARD;
        // no window background: only the widget itself on screen (hosts before ABI 1.5 ignore the flag)
        d.flags = HORIZON_PANEL_HUD | HORIZON_PANEL_NO_BACKGROUND;
        d.draw = &drawInputs;
        d.default_x = 0.44f;
        d.default_y = 0.74f;
        host->register_panel(host->plugin, &d);
    }
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    g_reader.reset();
    g_host = nullptr;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_SetEnabled(int32_t enabled) { g_enabled = enabled != 0; }

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawOverlay(void) {
    if (!g_host || !g_enabled || !g_reader) return;
    void* race = g_host->engine_pointer(HORIZON_ENGINE_RACE);
    g_reader->poll(g_mem, reinterpret_cast<uintptr_t>(race));
    g_inRace = g_reader->run().race;
    g_car = g_reader->car();
    g_view = g_car;
    if (g_car.ok) {
        const ULONGLONG now = GetTickCount64();
        g_view.steer = g_axSteer.filter(g_car.steer, now);
        g_view.gas = g_axGas.filter(g_car.gas, now);
        g_view.brake = g_axBrake.filter(g_car.brake, now);
    }
    g_trailSteer[g_trailAt] = g_car.ok ? g_view.steer : 0.0f;
    g_trailGas[g_trailAt] = g_car.ok ? g_view.gas : 0.0f;
    g_trailBrake[g_trailAt] = g_car.ok ? g_view.brake : 0.0f;
    g_trailAt = (g_trailAt + 1) % kTrail;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawMenu(void) {
    if (!g_ui || !g_host) return;
    const ck::Palette p = ck::palette(g_host);
    ck::section(p, "Style");
    // in the order players know them; the setting keeps its old numbers
    const int order[kStyleCount] = {kKeys, kController, kBars, kWheel, kMinimal};
    const char* names[kStyleCount] = {"Bars", "Arrows", "Wheel", "Minimal", "Controller"};
    const int style = styleSetting();
    for (int i = 0; i < kStyleCount; i++) {
        const int st = order[i];
        if (i) ck::sameLineIfFits(ck::checkboxWidth(names[st]));
        if (ImGui::RadioButton(names[st], style == st)) g_host->set_int(g_host->plugin, "Style", st);
    }
    // sliders: 220 px at scale 1, less in a narrow pop-out (the label stays visible)
    const float sliderW = std::min(220.0f * ck::uiScale(g_host), std::max(80.0f, ck::roomWidth(g_host) - 90.0f * ck::uiScale(g_host)));
    float size = settingF("Size", 1.0f);
    ImGui::SetNextItemWidth(sliderW);
    if (ImGui::SliderFloat("Size", &size, 0.6f, 2.0f, "%.1fx")) g_host->set_float(g_host->plugin, "Size", size);
    if (style == kKeys) {
        float th = settingF("Threshold", 0.2f);
        ImGui::SetNextItemWidth(sliderW);
        if (ImGui::SliderFloat("Key at", &th, 0.05f, 0.9f, "%.2f")) g_host->set_float(g_host->plugin, "Threshold", th);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stick or pedal travel that lights a key.");
    }
    ck::section(p, "Show");
    bool speed = setting("ShowSpeed", 1) != 0, mph = setting("Mph", 0) != 0, graph = graphOn(), gear = setting("Gear", 1) != 0,
         rpm = setting("Rpm", 1) != 0;
    if (ImGui::Checkbox("Speed", &speed)) g_host->set_int(g_host->plugin, "ShowSpeed", speed ? 1 : 0);
    ck::sameLineIfFits(ck::checkboxWidth("mph"));
    if (ImGui::Checkbox("mph", &mph)) g_host->set_int(g_host->plugin, "Mph", mph ? 1 : 0);
    if (style == kBars) {
        ck::sameLineIfFits(ck::checkboxWidth("Graph"));
        if (ImGui::Checkbox("Graph", &graph)) g_host->set_int(g_host->plugin, "Graph", graph ? 1 : 0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The last three seconds as lines.");
    }
    ck::sameLineIfFits(ck::checkboxWidth("Gear"));
    if (ImGui::Checkbox("Gear", &gear)) g_host->set_int(g_host->plugin, "Gear", gear ? 1 : 0);
    if (style == kWheel) {
        ck::sameLineIfFits(ck::checkboxWidth("RPM"));
        if (ImGui::Checkbox("RPM", &rpm)) g_host->set_int(g_host->plugin, "Rpm", rpm ? 1 : 0);
    }
    ck::section(p, "Now");
    if (g_car.ok) {
        ImGui::TextWrapped("Steer %+.2f   Gas %.2f   Brake %.2f", g_car.steer, g_car.gas, g_car.brake);
        ImGui::TextWrapped("Speed %s   Gear %s   RPM %s", g_car.haveSpeed ? speedText(g_car.speedKmh).c_str() : "-",
                           g_car.haveGear ? std::to_string(g_car.gear).c_str() : "-",
                           g_car.haveRpm ? std::to_string(static_cast<int>(g_car.rpm)).c_str() : "-");
        ck::hint(p, pedalsAnalog() ? "Pedals: analog" : "Pedals: on/off");
    } else {
        ck::hint(p, g_inRace ? "No data" : "Waiting for race");
    }
    ck::hint(p, "Read only");
}

HORIZON_EXPORT int32_t __cdecl HorizonClient_TestHook(const char* cmd, char* out, int32_t outSize) {
    if (!cmd || !g_host) return -1;
    char b[256];
    if (!strcmp(cmd, "state")) {
        snprintf(b, sizeof(b), "car=%d race=%d steer=%.2f gas=%.2f brake=%.2f speed=%d gear=%d rpm=%d", g_car.ok ? 1 : 0, g_inRace ? 1 : 0, g_car.steer,
                 g_car.gas, g_car.brake, g_car.haveSpeed ? static_cast<int>(g_car.speedKmh) : -1, g_car.haveGear ? g_car.gear : -1,
                 g_car.haveRpm ? static_cast<int>(g_car.rpm) : -1);
    } else if (!strcmp(cmd, "view")) {
        // what is drawn: digital inputs held steady, and whether the pedals count as analog
        snprintf(b, sizeof(b), "steer=%.2f gas=%.2f brake=%.2f analog=%d style=%d graph=%d", g_view.steer, g_view.gas, g_view.brake,
                 pedalsAnalog() ? 1 : 0, styleSetting(), graphOn() ? 1 : 0);
    } else if (!strcmp(cmd, "size")) {
        const ImVec2 sz = contentSize(styleSetting(), ck::uiScale(g_host) * settingF("Size", 1.0f));
        snprintf(b, sizeof(b), "%.0fx%.0f", sz.x, sz.y);
    } else if (!strncmp(cmd, "style ", 6)) {
        g_host->set_int(g_host->plugin, "Style", atoi(cmd + 6));
        snprintf(b, sizeof(b), "ok");
    } else {
        return -1;
    }
    if (out && outSize > 0) strncpy_s(out, static_cast<size_t>(outSize), b, _TRUNCATE);
    return static_cast<int32_t>(strlen(b));
}

} // extern "C"
