# Changelog

## 0.2.0 (in progress, branch next)

- Handshake for mods that are not plugins (TMVS): `HorizonHost_GetUiState`, `HorizonHost_SetCaptureActive` (heartbeat, 10 s), `HorizonHost_Version`. While a capture is active Horizon draws nothing and takes no input.
- Plugin ABI 1.2 (header): panels (`register_panel`), host flags (`HORIZON_HOST_FLAG_CAPTURE`).
- Icon font: 53 Lucide icons (ISC, 20 KB subset) merged into every host font; `HZ_ICON_*` codepoints in `horizon_imgui.h`, used in the sidebar and the bar.

## 0.1.0 (2026-11-11, first test build, not released)

- Legacy Loader `dinput8.dll` for ESWC, Sunrise and Forever without TMLoader: loads `horizon\host.dll`, Shift = safe mode, Ctrl+Shift = no host, chains an earlier `dinput8.dll` kept as `dinput8.chain.dll`.
- Host: Direct3D 9 hook through method tables (CreateDevice, Present, Reset), Dear ImGui 1.91.5 menu in the TM Horizon look (tile grid, plugin pages, hotkeys, log, about) and the Horizon bar at the top screen edge, menu key Insert.
- Plugin ABI 1.1 (`include/horizon_plugin.h`): settings, data folder, shared ImGui context, hotkeys with conflict display, game events, game control (map start and menu exit through the legacy-randomizer gamectl module), textures for ImGui, device access.
- ESWC: events (map loaded, race start, checkpoint, respawn, finish, race end, menu, game state), online gate, the online login fix moved over from my eswc-autosave.
- ESWC Autosave 2.0 as the first plugin (the replay save of eswc-autosave on the host's finish/race-end events, settings in the menu).
- Sunrise: menu, bar, plugins, map/state/online. Forever: experimental TMLoader product "Horizon Host", no game hooks.
- `profile.json` from TM Horizon (optional), per-start logs, settings in `horizon\settings`.
- TM Horizon logo in the menu header and the bar, plugin icons (`"icon"` in plugin.json, initials when missing), icons of known other mods (from the TM Horizon launcher), switch for the ESWC online login fix in General (applies on the next start). PNGs decoded with stb_image (public domain/MIT).
