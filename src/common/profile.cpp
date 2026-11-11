#include "common/profile.h"

#include "common/json.h"
#include "common/util.h"
#include "horizon_plugin.h"

namespace horizon {

const ProfilePlugin* Profile::find(const std::string& id) const {
    for (const auto& p : plugins)
        if (p.id == id) return &p;
    return nullptr;
}

namespace {
bool getBool(const Json& obj, const char* key, bool fallback) {
    const Json* v = obj.get(key);
    if (!v) return fallback;
    if (v->type == Json::Bool) return v->b;
    if (v->type == Json::Number) return v->num != 0;
    return fallback;
}
std::string getString(const Json& obj, const char* key) {
    const Json* v = obj.get(key);
    return v && v->type == Json::String ? v->str : std::string();
}
} // namespace

unsigned gameBitFromName(const std::string& name) {
    const std::string n = lower(trim(name));
    if (n == "eswc") return HORIZON_GAME_ESWC;
    if (n == "sunrise") return HORIZON_GAME_SUNRISE;
    if (n == "original") return HORIZON_GAME_ORIGINAL;
    if (n == "tmnf") return HORIZON_GAME_TMNF;
    if (n == "tmuf") return HORIZON_GAME_TMUF;
    return 0;
}

const char* gameKey(unsigned gameBit) {
    switch (gameBit) {
    case HORIZON_GAME_ESWC: return "eswc";
    case HORIZON_GAME_SUNRISE: return "sunrise";
    case HORIZON_GAME_ORIGINAL: return "original";
    case HORIZON_GAME_TMNF: return "tmnf";
    case HORIZON_GAME_TMUF: return "tmuf";
    default: return "";
    }
}

Profile parseProfile(const std::string& text) {
    Profile p;
    p.present = true;
    Json root;
    std::string err;
    if (!JsonParser::parse(text, root, err)) {
        p.error = "profile.json is not valid JSON: " + err;
        return p;
    }
    if (root.type != Json::Object) {
        p.error = "profile.json: the top level must be an object";
        return p;
    }
    const Json* format = root.get("format");
    if (!format || format->type != Json::Number) {
        p.error = "profile.json: \"format\" missing";
        return p;
    }
    p.format = static_cast<int>(format->num);
    if (p.format != 1) {
        p.error = "profile.json: format " + std::to_string(p.format) + " is newer than this host understands (1)";
        return p;
    }
    p.name = getString(root, "profile");
    p.game = getString(root, "game");
    p.safeMode = getBool(root, "safe_mode", false);
    p.menuKey = trim(getString(root, "menu_key"));
    p.bar = getBool(root, "bar", true);
    p.statsDir = trim(getString(root, "stats_dir"));
    p.writtenBy = getString(root, "written_by");
    p.writtenAt = getString(root, "written_at");
    const Json* plugins = root.get("plugins");
    if (plugins && plugins->type != Json::Array) {
        p.error = "profile.json: \"plugins\" must be an array";
        return p;
    }
    if (plugins) {
        for (size_t i = 0; i < plugins->arr.size(); i++) {
            const Json& e = plugins->arr[i];
            ProfilePlugin pp;
            if (e.type == Json::String) { // shorthand: "eswc-autosave"
                pp.id = e.str;
            } else if (e.type == Json::Object) {
                pp.id = getString(e, "id");
                pp.version = getString(e, "version");
                pp.enabled = getBool(e, "enabled", true);
            } else {
                p.warnings.push_back("plugins[" + std::to_string(i) + "]: not an object, skipped");
                continue;
            }
            if (!validId(pp.id)) {
                p.warnings.push_back("plugins[" + std::to_string(i) + "]: id \"" + pp.id + "\" is not a valid plugin id, skipped");
                continue;
            }
            if (p.find(pp.id)) {
                p.warnings.push_back("plugins[" + std::to_string(i) + "]: \"" + pp.id + "\" listed twice, the first entry counts");
                continue;
            }
            p.plugins.push_back(pp);
        }
    }
    p.valid = true;
    return p;
}

PluginManifest parsePluginManifest(const std::string& text, const std::string& folderId) {
    PluginManifest m;
    Json root;
    std::string err;
    if (!JsonParser::parse(text, root, err) || root.type != Json::Object) {
        m.error = "plugin.json is not a valid JSON object" + (err.empty() ? std::string() : ": " + err);
        return m;
    }
    m.id = getString(root, "id");
    if (m.id.empty()) m.id = folderId;
    if (m.id != folderId) {
        m.error = "plugin.json id \"" + m.id + "\" differs from its folder \"" + folderId + "\"";
        return m;
    }
    m.name = getString(root, "name");
    if (m.name.empty()) m.name = m.id;
    m.version = getString(root, "version");
    m.dll = getString(root, "dll");
    if (m.dll.empty()) m.dll = m.id + ".dll";
    if (m.dll.find_first_of("\\/:") != std::string::npos || m.dll.find("..") != std::string::npos) {
        m.error = "plugin.json \"dll\" must be a file name in the plugin's folder";
        return m;
    }
    m.description = getString(root, "description");
    m.icon = getString(root, "icon");
    if (m.icon.find_first_of("\\/:") != std::string::npos || m.icon.find("..") != std::string::npos) m.icon.clear();
    m.author = getString(root, "author");
    const Json* games = root.get("games");
    if (games && games->type == Json::Array) {
        for (const auto& g : games->arr)
            if (g.type == Json::String) m.games |= gameBitFromName(g.str);
    }
    m.soloOnly = lower(getString(root, "online")) != "online-ok";
    m.enabledByDefault = getBool(root, "enabled_by_default", true);
    m.startupOnly = getBool(root, "startup_only", false);
    const Json* abi = root.get("abi");
    m.abi = abi && abi->type == Json::Number ? static_cast<int>(abi->num) : 0;
    m.valid = true;
    return m;
}

} // namespace horizon
