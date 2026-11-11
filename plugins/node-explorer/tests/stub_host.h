// A small stand-in for Horizon Host for the Node Explorer tests and the preview: settings in a map,
// log lines collected, engine pointers into the fake graph, the caller's ImGui context.
#pragma once

#include <map>
#include <string>

#include "fake_graph.h"
#include "horizon_plugin.h"

namespace stubhost {

extern std::map<std::string, std::string> settings;
extern std::string logs;          // every log line, '\n' separated
extern HorizonGameInfo gameInfo;  // TMNF, exe timestamp 0 (unknown build: reflect scans this exe)
extern HorizonStyle style;        // TM Horizon tokens; fonts NULL unless the caller sets them
extern fake::Graph graph;         // engine_pointer() returns its app / race / challenge

// Fills game info and style, data folder = <exe dir>\data\.
void setup();
HorizonHostApi api();

} // namespace stubhost
