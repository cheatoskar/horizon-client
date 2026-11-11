// Play events for TM Horizon's statistics page: profile.json "stats_dir" -> <dir>\<yyyy-mm>.jsonl
// (stats_log.h has the format). Nothing without the key or in safe mode; the writes run on a worker
// thread, so a frame never waits on the disk.
#pragma once

#include "horizon_plugin.h"

namespace horizon {

void statsStart();                      // after pluginsReadProfile()
void statsEvent(const HorizonEvent& e); // game thread, every dispatched event (only run events are used)
void statsStop();                       // writes what is queued, ends the worker
bool statsActive();

} // namespace horizon
