// [Setting ...] metadata on a script's global variables, from which the settings page is built:
//
//   [Setting name="Position" options="Top left|Top right|Bottom left|Bottom right"]
//   int position = 1;
//   [Setting name="Size" min=0.5 max=3 description="Text size"]
//   float size = 1.2f;
//
// Pure parsing of the text between the brackets (the script builder hands it over without them).
#pragma once

#include <string>
#include <vector>

namespace hs {

struct SettingMeta {
    std::string name;           // label; empty = the variable's name
    std::string description;    // tooltip
    bool hasMin = false, hasMax = false;
    double min = 0, max = 0;    // sliders for int/float when both are set
    std::vector<std::string> options; // int: a combo with these labels (value = index)
    bool hidden = false;        // saved, but not shown on the page
    bool multiline = false;     // string: a bigger text box
    bool color = false;         // reserved (vec4 colours later); ignored today
};

// False when the metadata is not a Setting at all (other metadata is ignored silently).
// *error gets a message for a Setting with a broken attribute (the setting still works with defaults).
bool parseSettingMeta(const std::string& meta, SettingMeta* out, std::string* error);

} // namespace hs
