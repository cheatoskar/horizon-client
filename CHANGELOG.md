# Changelog

## 0.2.0 (in progress, branch next)

- Handshake for mods that are not plugins (TMVS): `HorizonHost_GetUiState`, `HorizonHost_SetCaptureActive` (heartbeat, 10 s), `HorizonHost_Version`. While a capture is active Horizon draws nothing and takes no input.
- Plugin ABI 1.3: `open_page(plugin, panel_key)` opens the Horizon menu on the plugin's page (optionally on a panel tab) for an "open my page" hotkey; toggles when it already shows that page. TMVS uses it for F8.
- Plugin ABI 1.2: panels (`register_panel`: tabs on the plugin's page, pop out into windows with keep open, lock layout, click-through, hide in photos and videos, opacity; saved per plugin + key), host flags (`HORIZON_HOST_FLAG_CAPTURE`). The sample plugin has an Events tab and a Clock HUD window.
- `save_replay` works on ESWC (the game's save without dialogs; current run on the end-race screen, else the previous race).
- Sunrise: start, checkpoint, finish and solo race end from the game (static addresses, hooked only when the bytes match; first test).
- Buttons and tabs are one step lighter than the cards.
- Icon font: 53 Lucide icons (ISC, 20 KB subset) merged into every host font; `HZ_ICON_*` codepoints in `horizon_imgui.h`, used in the sidebar and the bar.
- Forever: game events by polling only (map loaded with medal times, race start, finish with time and PB, game state incl. end screen), no hooks (Competition Patch 2.0). gamectl and reflection now rebase their addresses, so game control and state also work on TMLoader's relocated TmForever 2.12.0. Not run in game yet.
- TMX Upload plugin (`tmx-upload`, off by default, all games): offers new replays to TM Horizon or the browser extension over the local bridge protocol of the 100% TMX mod (127.0.0.1:27311-27315, pairing with Allow in the game); stays passive when the Bingo mod already runs the bridge.

## 0.1.0 (2026-11-11, first test build, not released)

- Legacy Loader `dinput8.dll` for ESWC, Sunrise and Forever without TMLoader: loads `horizon\host.dll`, Shift = safe mode, Ctrl+Shift = no host, chains an earlier `dinput8.dll` kept as `dinput8.chain.dll`.
- Host: Direct3D 9 hook through method tables (CreateDevice, Present, Reset), Dear ImGui 1.91.5 menu in the TM Horizon look (tile grid, plugin pages, hotkeys, log, about) and the Horizon bar at the top screen edge, menu key Insert.
- Plugin ABI 1.1 (`include/horizon_plugin.h`): settings, data folder, shared ImGui context, hotkeys with conflict display, game events, game control (map start and menu exit through the legacy-randomizer gamectl module), textures for ImGui, device access.
- ESWC: events (map loaded, race start, checkpoint, respawn, finish, race end, menu, game state), online gate, the online login fix moved over from my eswc-autosave.
- ESWC Autosave 2.0 as the first plugin (the replay save of eswc-autosave on the host's finish/race-end events, settings in the menu).
- Sunrise: menu, bar, plugins, map/state/online. Forever: experimental TMLoader product "Horizon Host", no game hooks.
- `profile.json` from TM Horizon (optional), per-start logs, settings in `horizon\settings`.
- TM Horizon logo in the menu header and the bar, plugin icons (`"icon"` in plugin.json, initials when missing), icons of known other mods (from the TM Horizon launcher), switch for the ESWC online login fix in General (applies on the next start). PNGs decoded with stb_image (public domain/MIT).
