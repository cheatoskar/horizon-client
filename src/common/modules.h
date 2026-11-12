// modules.json next to host.dll (packaging/modules.json): module groups for the menu and the launcher, and the
// first-start defaults that are not in a module's own plugin.json. Pure parsing, no files. docs/modules-json.md.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace horizon {

struct GroupMember {
    std::string id;     // plugin id, or the host feature ("login-fix") when host is true
    std::string label;  // "Autosave", "Controller fix", "Login fix"
    bool host = false;
};

// Several modules shown as one entry ("Legacy Essentials"): one row, one icon, one switch, the members' own
// switches and pages inside. The DLLs stay separate.
struct ModuleGroup {
    std::string id;          // "legacy-essentials" ([a-z0-9-])
    std::string name;        // "Legacy Essentials"
    std::string description;
    std::string icon;        // PNG next to modules.json ("icons/legacy-essentials.png"), "" none
    std::vector<GroupMember> members;
};

struct ModulesConfig {
    bool valid = false;      // false: no file or not usable (then no groups, no extra defaults)
    std::string error;
    std::vector<ModuleGroup> groups;
    // first start without profile.json: module id -> on/off, over plugin.json "enabled_by_default"
    std::vector<std::pair<std::string, bool>> defaults;
    // what 0.3.0 used for modules whose default changed since: an install from before keeps that (written once)
    std::vector<std::pair<std::string, bool>> previousDefaults;

    const ModuleGroup* groupOf(const std::string& pluginId) const;
    const ModuleGroup* group(const std::string& groupId) const;
    // -1 not listed, 0 off, 1 on
    int defaultFor(const std::string& pluginId) const;
};

ModulesConfig parseModulesConfig(const std::string& text);

} // namespace horizon
