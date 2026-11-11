# Changelog

## 0.2.0 (in progress, branch next)

- Plugin ABI 1.3, one minor for three additions, in this fixed order at the end of `HorizonHostApi`: `open_page(plugin, panel_key)` (opens the Horizon menu on the plugin's page, optionally on a panel tab, for an "open my page" hotkey; toggles when it already shows that page; TMVS uses it for F8), toasts (`notify`: short notes in the corner, kinds info/ok/warn/error, "Do not disturb", "Quiet while driving", hidden in photos and videos, list of the last 20 on the General page) and the TM Horizon bridge (`bridge_request`, `bridge_state`: plugins ask TM Horizon on the same PC for TMX data, 8 open requests and 4 per second per plugin, answers on the game thread). TM Horizon's own notifications (upload finished, mod updates, server events) show as toasts too. The bridge has a switch and "Forget pairing" on the General page. Protocol: `docs/client-bridge.md`, plugin side: `docs/plugin-abi.md`. New callback at the end of `HorizonDeviceCallbacks`: `post_present` (right after `Present`, for frame timing and pacing). Plugins built against 1.2 keep working (the host copies only the callback table's `size`; a test plugin built against the 1.2 header is loaded in the test run).
- Client mods, three plugins on the new ABI (`docs/plugins/`): WR Hunter (splits against your PB and the TMX record, gold sectors, history per map), Input Display (steering, gas, brake, speed, gear and RPM as HUD panel in four styles) and TMX Browser (search the TrackMania Exchange from the game, favourites, download and play offline). Read-only: no game memory writes, no hooks, no input changes. Shared code in `plugins/client-common`. Not confirmed in the real game yet: the Forever car input offsets, the ESWC/Sunrise map UID offset.
- Handshake for mods that are not plugins (TMVS): `HorizonHost_GetUiState`, `HorizonHost_SetCaptureActive` (heartbeat, 10 s), `HorizonHost_Version`. While a capture is active Horizon draws nothing and takes no input.
- Horizon Scripts plugin (`docs/scripting.md`): AngelScript 2.38.0, a read-only script API, Scripts tab, example scripts (clock, map info).
- Node Explorer plugin: read-only browser for the game's classes and objects.
- Horizon Performance plugin (`docs/performance.md`): frame times (game, Present, limiter), Direct3D 9 call counts, benchmark, frame limiter in `post_present`, hang log, options with their CP and online safety noted.
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
