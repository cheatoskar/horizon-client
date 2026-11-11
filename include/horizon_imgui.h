// horizon_imgui.h - attach a plugin's Dear ImGui copy to the host's context (C++ only, MIT licence).
//
// Every DLL that links Dear ImGui has its own global "current context" and, with the static C
// runtime (/MT), its own heap. A plugin must therefore, once in HorizonPlugin_Init:
//   1. check that it was built with the host's ImGui version (HORIZON_IMGUI_VERSION_NUM),
//   2. use the host's allocator (memory ImGui allocates in the plugin is freed by the host and back),
//   3. make the host's context current.
// horizon_imgui_attach() does all three. Include imgui.h before this header.
#pragma once

#include "horizon_plugin.h"

#ifndef IMGUI_VERSION_NUM
#error "include imgui.h before horizon_imgui.h"
#endif

#if IMGUI_VERSION_NUM != HORIZON_IMGUI_VERSION_NUM
#error "this plugin's Dear ImGui version differs from the host's (HORIZON_IMGUI_VERSION_NUM)"
#endif

// Returns true when the plugin may draw. On false, log it and don't export/draw menus.
inline bool horizon_imgui_attach(const HorizonHostApi* host) {
    if (!host || !host->imgui_context || host->imgui_version() != IMGUI_VERSION_NUM) return false;
    void* allocFn = nullptr;
    void* freeFn = nullptr;
    void* user = nullptr;
    host->imgui_allocators(&allocFn, &freeFn, &user);
    if (!allocFn || !freeFn) return false;
    ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(allocFn), reinterpret_cast<ImGuiMemFreeFunc>(freeFn), user);
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(host->imgui_context()));
    return ImGui::GetCurrentContext() != nullptr;
}

// ImVec4 from a host style colour.
inline ImVec4 horizon_color(const HorizonColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }
