#include "common/setup.h"

#include <cmath>
#include <cstring>

#include "common/json.h"
#include "common/settings.h"
#include "common/util.h"
#include "horizon_plugin.h"

namespace horizon {
namespace setup {

namespace {
bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// A flat JSON object of the scalar values in o (nested values are not settings).
Json flatOf(const Json& o) {
    Json out;
    out.type = Json::Object;
    if (o.type != Json::Object) return out;
    for (const auto& kv : o.obj)
        if (kv.second.type == Json::String || kv.second.type == Json::Number || kv.second.type == Json::Bool) out.obj.push_back(kv);
    return out;
}

Json parseOrEmpty(const std::string& text) {
    Json j;
    std::string err;
    if (!JsonParser::parse(text, j, err) || j.type != Json::Object) {
        j = Json();
        j.type = Json::Object;
    }
    return flatOf(j);
}

std::string textOf(const Json& j) {
    std::string out;
    jsonWrite(out, j);
    return out;
}

Json str(const std::string& s) {
    Json j;
    j.type = Json::String;
    j.str = s;
    return j;
}
} // namespace

std::string pluginOfKey(const std::string& key) {
    for (const char* prefix : {"plugin.", "panel.", "hotkey."}) {
        if (!startsWith(key, prefix)) continue;
        const size_t a = strlen(prefix);
        const size_t b = key.find('.', a);
        if (b == std::string::npos || b == a) return std::string();
        return key.substr(a, b - a);
    }
    return std::string();
}

bool pluginGameSpecific(uint32_t manifestGames) {
    const uint32_t g = manifestGames & HORIZON_GAME_ALL;
    return g != 0 && g != HORIZON_GAME_ALL;
}

bool isLocalHostKey(const std::string& key, GameSpecificFn gameSpecific, void* user) {
    if (startsWith(key, "bridge.") || startsWith(key, "setup.") || startsWith(key, "pwin.") || key == "OnlineFix") return true;
    const std::string id = pluginOfKey(key);
    return !id.empty() && gameSpecific && gameSpecific(id, user);
}

bool isExportableHostKey(const std::string& key) { return !startsWith(key, "bridge.") && !startsWith(key, "setup."); }

int sharedDefault(bool hostJsonExisted) { return hostJsonExisted ? 0 : 1; }

void copyKeys(const SettingsFile& src, SettingsFile& dst, PickFn pick, void* user, bool replace) {
    if (replace) {
        for (const std::string& k : dst.keys())
            if ((!pick || pick(k, user)) && !src.has(k)) dst.remove(k);
    }
    for (const std::string& k : src.keys())
        if (!pick || pick(k, user)) dst.copyKey(src, k);
}

std::string exportText(const std::string& hostJson, const std::vector<PluginSettings>& plugins, const std::string& game,
                       const std::string& hostVersion) {
    Json root;
    root.type = Json::Object;
    root.obj.emplace_back("format", str("horizon-setup"));
    Json ver;
    ver.type = Json::Number;
    ver.num = 1;
    root.obj.emplace_back("version", ver);
    root.obj.emplace_back("host_version", str(hostVersion));
    root.obj.emplace_back("game", str(game));
    root.obj.emplace_back("host", parseOrEmpty(hostJson));
    Json ps;
    ps.type = Json::Object;
    for (const PluginSettings& p : plugins) {
        if (!validId(p.id)) continue;
        Json flat = parseOrEmpty(p.json);
        if (flat.obj.empty()) continue;
        ps.obj.emplace_back(p.id, flat);
    }
    root.obj.emplace_back("plugins", ps);
    std::string out = textOf(root);
    out += "\n";
    return out;
}

bool importText(const std::string& text, Imported& out, std::string& err) {
    out = Imported();
    Json root;
    if (!JsonParser::parse(text, root, err)) return false;
    if (root.type != Json::Object) {
        err = "not a JSON object";
        return false;
    }
    const Json* format = root.get("format");
    if (!format || format->asString() != "horizon-setup") {
        err = "not a Horizon setup file";
        return false;
    }
    const Json* version = root.get("version");
    if (!version || !version->isNumber() || version->asInt() < 1) {
        err = "no version";
        return false;
    }
    if (version->asInt() > 1) {
        err = "made by a newer Horizon Client";
        return false;
    }
    if (const Json* g = root.get("game")) out.game = g->asString();
    if (const Json* hv = root.get("host_version")) out.hostVersion = hv->asString();
    const Json* host = root.get("host");
    Json flatHost = host ? flatOf(*host) : flatOf(Json());
    // the pairing and the setup switch never come from a file
    Json kept;
    kept.type = Json::Object;
    for (const auto& kv : flatHost.obj)
        if (isExportableHostKey(kv.first)) kept.obj.push_back(kv);
    out.hostJson = textOf(kept);
    if (const Json* ps = root.get("plugins")) {
        if (ps->type == Json::Object) {
            for (const auto& kv : ps->obj) {
                if (!validId(kv.first) || kv.second.type != Json::Object) continue;
                out.plugins.push_back(PluginSettings{kv.first, textOf(flatOf(kv.second))});
            }
        }
    }
    return true;
}

Anchor anchorOf(float x, float y, float w, float h, float screenW, float screenH) {
    Anchor a;
    if (screenW <= 0.0f || screenH <= 0.0f || x < 0.0f || y < 0.0f) return a;
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    a.ax = cx < screenW / 3.0f ? 0.0f : cx > screenW * 2.0f / 3.0f ? 1.0f : 0.5f;
    a.ay = cy < screenH / 3.0f ? 0.0f : cy > screenH * 2.0f / 3.0f ? 1.0f : 0.5f;
    a.ox = a.ax == 0.0f ? x / screenW : a.ax == 1.0f ? (screenW - (x + w)) / screenW : (cx - screenW * 0.5f) / screenW;
    a.oy = a.ay == 0.0f ? y / screenH : a.ay == 1.0f ? (screenH - (y + h)) / screenH : (cy - screenH * 0.5f) / screenH;
    a.valid = true;
    return a;
}

void placeAt(const Anchor& a, float w, float h, float screenW, float screenH, float& x, float& y) {
    if (a.ax == 0.0f) x = a.ox * screenW;
    else if (a.ax == 1.0f) x = screenW - a.ox * screenW - w;
    else x = screenW * 0.5f + a.ox * screenW - w * 0.5f;
    if (a.ay == 0.0f) y = a.oy * screenH;
    else if (a.ay == 1.0f) y = screenH - a.oy * screenH - h;
    else y = screenH * 0.5f + a.oy * screenH - h * 0.5f;
    x = std::round(std::fmax(0.0f, std::fmin(x, screenW - w)));
    y = std::round(std::fmax(0.0f, std::fmin(y, screenH - h)));
}

} // namespace setup
} // namespace horizon
