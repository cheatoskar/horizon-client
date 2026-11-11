// Sizes of the ABI 1.2 tables (32-bit x86), frozen. Two sides check them:
//   - tests/abi12/abi12_plugin.cpp, built against the frozen 1.2 header next to it: these are its sizes.
//   - tests/unit_tests.cpp, built against include/horizon_plugin.h: the 1.3 fields start exactly here.
// If one of them stops compiling, the 1.3 layout is no longer an append to 1.2.
#pragma once

#define ABI12_HOST_API_SIZE 172u          /* sizeof(HorizonHostApi) in ABI 1.2 */
#define ABI12_DEVICE_CALLBACKS_SIZE 28u   /* sizeof(HorizonDeviceCallbacks) in ABI 1.2 */
#define ABI12_PLUGIN_INFO_SIZE 40u        /* sizeof(HorizonPluginInfo), unchanged in 1.3 */
