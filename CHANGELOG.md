# Changelog

## 0.1.0 (2026-11-11, first test build, not released)

- Legacy Loader `dinput8.dll` for ESWC, Sunrise and Forever without TMLoader: loads `horizon\host.dll`, Shift = safe mode, Ctrl+Shift = no host, chains an earlier `dinput8.dll` kept as `dinput8.chain.dll`.
- Host: Direct3D 9 hook through method tables (CreateDevice, Present, Reset), Dear ImGui 1.91.5 menu in the TM Horizon look (tile grid, plugin pages, hotkeys, log, about) and the Horizon bar at the top screen edge, menu key Insert.
- Plugin ABI 1.1 (`include/horizon_plugin.h`): settings, data folder, shared ImGui context, hotkeys with conflict display, game events, game control (map start and menu exit through the legacy-randomizer gamectl module), textures for ImGui, device access.
- ESWC: events (map loaded, race start, checkpoint, respawn, finish, race end, menu, game state), online gate, the online login fix moved over from my eswc-autosave.
- ESWC Autosave 2.0 as the first plugin (the replay save of eswc-autosave on the host's finish/race-end events, settings in the menu).
- Sunrise: menu, bar, plugins, map/state/online. Forever: experimental TMLoader product "Horizon Host", no game hooks.
- `profile.json` from TM Horizon (optional), per-start logs, settings in `horizon\settings`.
