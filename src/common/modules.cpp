#include "common/modules.h"

#include "common/json.h"
#include "common/util.h"

namespace horizon {

const ModuleGroup* ModulesConfig::groupOf(const std::string& pluginId) const {
    for (const ModuleGroup& g : groups)
        for (const GroupMember& m : g.members)
            if (!m.host && m.id == pluginId) return &g;
    return nullptr;
}

const ModuleGroup* ModulesConfig::group(const std::string& groupId) const {
    for (const ModuleGroup& g : groups)
        if (g.id == groupId) return &g;
    return nullptr;
}

int ModulesConfig::defaultFor(const std::string& pluginId) const {
    for (const auto& d : defaults)
        if (d.first == pluginId) return d.second ? 1 : 0;
    return -1;
}

namespace {
void readSwitches(const Json* o, std::vector<std::pair<std::string, bool>>& out) {
    if (!o || o->type != Json::Object) return;
    for (const auto& kv : o->obj)
        if (validId(kv.first) && kv.second.type == Json::Bool) out.emplace_back(kv.first, kv.second.b);
}
} // namespace

ModulesConfig parseModulesConfig(const std::string& text) {
    ModulesConfig c;
    Json root;
    if (!JsonParser::parse(text, root, c.error)) return c;
    if (root.type != Json::Object) {
        c.error = "not a JSON object";
        return c;
    }
    const Json* format = root.get("format");
    if (!format || format->asInt() != 1) {
        c.error = format && format->asInt() > 1 ? "made for a newer Horizon Client" : "no \"format\": 1";
        return c;
    }
    if (const Json* gs = root.get("groups")) {
        if (gs->type == Json::Array) {
            for (const Json& gj : gs->arr) {
                if (gj.type != Json::Object) continue;
                ModuleGroup g;
                if (const Json* v = gj.get("id")) g.id = v->asString();
                if (const Json* v = gj.get("name")) g.name = v->asString();
                if (const Json* v = gj.get("description")) g.description = v->asString();
                if (const Json* v = gj.get("icon")) g.icon = v->asString();
                if (!validId(g.id) || g.name.empty() || g.icon.find("..") != std::string::npos) continue;
                if (const Json* ms = gj.get("members")) {
                    if (ms->type != Json::Array) continue;
                    for (const Json& mj : ms->arr) {
                        GroupMember m;
                        if (const Json* v = mj.get("id")) m.id = v->asString();
                        if (const Json* v = mj.get("host")) {
                            m.id = v->asString();
                            m.host = true;
                        }
                        if (const Json* v = mj.get("label")) m.label = v->asString();
                        if (!validId(m.id)) continue;
                        if (m.label.empty()) m.label = m.id;
                        bool taken = false; // a module belongs to one group only
                        for (const ModuleGroup& other : c.groups)
                            for (const GroupMember& om : other.members)
                                if (!m.host && !om.host && om.id == m.id) taken = true;
                        if (!taken) g.members.push_back(m);
                    }
                }
                if (!g.members.empty() && !c.group(g.id)) c.groups.push_back(g);
            }
        }
    }
    readSwitches(root.get("defaults"), c.defaults);
    readSwitches(root.get("previous_defaults"), c.previousDefaults);
    c.valid = true;
    return c;
}

} // namespace horizon
