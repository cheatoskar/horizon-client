// The TM Horizon bridge (ABI minor 3), see bridge.cpp and docs/client-bridge.md.
#pragma once

#include <string>

#include "horizon_plugin.h"

namespace horizon {

struct Plugin;

void bridgeStart();   // after hostConfigLoad, before plugins load
void bridgeStop();    // before plugins shut down (joins the threads)
int bridgeState();    // HORIZON_BRIDGE_*
std::string bridgeStateText();
bool bridgePaired();
void bridgeSetEnabled(bool on);
void bridgeForget();
// Game thread.
HorizonResult bridgeRequest(Plugin* p, const char* method, const char* path, const void* body, uint32_t len, HorizonBridgeCallback cb,
                            void* user);
// Game thread, end of a frame: answers to the plugins, the key into host.json.
void bridgeDeliver();
void bridgeDropPlugin(Plugin* p);

} // namespace horizon
