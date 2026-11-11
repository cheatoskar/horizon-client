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

void panelsBeginFrame();
bool panelsWantFrame(bool menuOpen);
bool panelsDrawWindows(bool menuOpen);         // true: at least one window was drawn

} // namespace horizon
