// Panels (ABI minor 2), see panels.cpp.
#pragma once

#include <string>
#include <vector>

#include "horizon_plugin.h"
#include "host/ui/ui.h"

namespace horizon {

struct Plugin;

HorizonResult panelRegister(Plugin* p, const HorizonPanelDef* def);
void panelUnregister(Plugin* p, const char* key);
void panelsRemovePlugin(Plugin* p);
int32_t panelVisible(Plugin* p, const char* key);
uint32_t hostFlagsAll();                       // OR of every running plugin's set_host_flag bits

std::vector<ui::PanelView> panelViews(const std::string& pluginId);
std::string panelTabLabel(const ui::PanelView& v); // icon (if the font has it) + title
void panelDrawTab(const std::string& pluginId, const std::string& key);

// What the windows may do this frame.
struct PanelsFrame {
    bool menuOpen = false;       // the Horizon menu is open: windows have title bars and can be moved
    bool layout = false;         // layout mode: windows take no input, panelsDrawLayout draws the frames
    bool iconsClickable = false; // minimised widgets' edge icons take a click (menu, layout mode or not racing)
};

void panelsBeginFrame();
bool panelsWantFrame(bool menuOpen);           // a window or a minimised widget's icon has to be drawn
bool panelsDrawWindows(const PanelsFrame& f);  // true: at least one window was drawn
// Layout mode: the frames (move, resize, minimise, hide) over every widget and the bar at the top.
void panelsDrawLayout();
// Every widget back to its slot, normal size, not minimised.
void panelsResetLayout();

} // namespace horizon
