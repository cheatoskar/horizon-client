// Layout test: a plugin built against the frozen ABI 1.2 header (tests/abi12/horizon_plugin.h, a copy of
// include/horizon_plugin.h before minor 3). tests/run_tests.cmd loads it with the 1.3 host; host_test
// checks the log lines below. It proves that 1.3 only appended fields:
//   - the host's table is at least as large as the 1.2 table, and the 1.2 functions it calls work;
//   - its HorizonDeviceCallbacks is the short 1.2 one. Right behind it in memory sits a trap pointer.
//     A host that copied more than cb->size bytes would take the trap for post_present and call it.
#define HORIZON_PLUGIN_BUILD
#include "horizon_plugin.h" // the 1.2 copy in this folder (the source's folder comes first)
#include "abi12_sizes.h"

static_assert(HORIZON_ABI == 1 && HORIZON_ABI_MINOR == 2, "tests/abi12/horizon_plugin.h must stay the 1.2 header");
static_assert(sizeof(void*) == 4, "32-bit only");
static_assert(sizeof(HorizonHostApi) == ABI12_HOST_API_SIZE, "1.2 HorizonHostApi size changed");
static_assert(sizeof(HorizonDeviceCallbacks) == ABI12_DEVICE_CALLBACKS_SIZE, "1.2 HorizonDeviceCallbacks size changed");
static_assert(sizeof(HorizonPluginInfo) == ABI12_PLUGIN_INFO_SIZE, "1.2 HorizonPluginInfo size changed");

namespace {

const HorizonHostApi* g_host = nullptr;
bool g_preLogged = false;

void HORIZON_CALL onPrePresent(void*, void*) {
    if (!g_preLogged && g_host) {
        g_preLogged = true;
        g_host->log(g_host->plugin, HORIZON_LOG_INFO, "abi12: pre_present called");
    }
}

// The trap: same signature as the 1.3 post_present. It must never run.
void HORIZON_CALL trapPostPresent(void*, void*, int32_t) {
    if (g_host) g_host->log(g_host->plugin, HORIZON_LOG_ERROR, "abi12: FAIL host called past the 1.2 callback table");
}

struct CallbacksWithTrap {
    HorizonDeviceCallbacks cb; // the 1.2 table, cb.size = 28
    void (HORIZON_CALL* trap)(void*, void*, int32_t);
};
CallbacksWithTrap g_cb = {};

const HorizonPluginInfo kInfo = {
    HORIZON_ABI,
    "abi12",
    "ABI 1.2 test",
    "1.0.0",
    HORIZON_GAME_ALL,
    HORIZON_CAP_DEVICE,
    HORIZON_FLAG_ONLINE_OK,
    "test",
    "Built against the ABI 1.2 header; checks that a 1.3 host still loads it.",
    HORIZON_ABI_MINOR,
};

} // namespace

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) { return &kInfo; }

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    if (!host || host->abi != HORIZON_ABI) return 1;
    g_host = host;
    const bool sizeOk = host->size >= sizeof(HorizonHostApi);
    const bool minorOk = host->abi_minor >= 2;
    // a 1.2 function at the end of the 1.2 table: if 1.3 had moved anything, this would be another slot
    const uint32_t flags = host->host_flags ? host->host_flags() : 0xFFFFFFFFu;
    host->log(host->plugin, sizeOk && minorOk && flags != 0xFFFFFFFFu ? HORIZON_LOG_INFO : HORIZON_LOG_ERROR,
              "abi12: host size %u (1.2 table %u), minor %u, host_flags %u -> %s", host->size,
              static_cast<uint32_t>(sizeof(HorizonHostApi)), host->abi_minor, flags,
              sizeOk && minorOk && flags != 0xFFFFFFFFu ? "1.2 layout ok" : "FAIL");
    if (!sizeOk || !minorOk) return 2;

    g_cb.cb.size = sizeof(HorizonDeviceCallbacks);
    g_cb.cb.pre_present = onPrePresent;
    g_cb.trap = trapPostPresent;
    host->set_device_callbacks(host->plugin, &g_cb.cb);
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) { g_host = nullptr; }
