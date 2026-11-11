# Horizon Host

The in-game mod host of my TM Horizon launcher, for the old TrackMania games: one menu for all Horizon mods, a slim bar at the top screen edge, hotkeys with conflict display, one log per start, and a small C plugin ABI so mods stop fighting over Direct3D and game hooks.

Status: first test build (0.1.0). Design: `tm-hub-mono/docs/design/horizon-host.md`.

## Pieces

| Piece | File | What it does |
|---|---|---|
| Legacy Loader | `dinput8.dll` (next to the exe, ~5 KB, no C runtime) | Passes DirectInput through (to `dinput8.chain.dll` if an earlier dinput8 mod was there, else to the system DLL) and loads `horizon\host.dll`. Shift at start = safe mode (no plugins), Ctrl+Shift = no host. Nothing else. |
| Host | `horizon\host.dll` (Forever: TMLoader product `Horizon Host`, `HorizonHost.dll`) | Direct3D 9 hook through method tables only (factory CreateDevice, device Present/Reset; re-checked, coexists with TM Vibrant Shaders and kiero/MinHook mods), Dear ImGui menu and bar, DirectInput blocking while the menu is open (not on Forever), hotkey registry, settings, per-start log, plugin loading, game events, online gate. |
| Plugins | `horizon\plugins\<id>\<id>.dll` + `plugin.json` | Native DLLs against `include/horizon_plugin.h`. |

## Games

| Game | Exe (SHA-256) | Loaded by | What works |
|---|---|---|---|
| TM Nations ESWC, unofficial 1.80 | `983bd127...` | Legacy Loader | menu, bar, plugins, events (map, start, checkpoint, respawn, finish, race end, menu, state), online gate, online login fix |
| TM Sunrise, Win10 repack | `e59dfc7e...` | Legacy Loader (works through the repack's own d3d9.dll proxy) | menu, bar, plugins, map/state/online events; no finish/checkpoint yet |
| TM Nations/United Forever | `3847cf9f...`, `4b6a7b31...`, TMLoader `8c5325d0...`, `0a885e19...` | TMLoader product, or the Legacy Loader | experimental: menu, bar, plugins; no game hooks, input never touched (Competition Patch) |

Any other exe: the host stays off and writes why into the log. No StarForce versions.

## Build

MSVC Build Tools (x86). `build.cmd` builds everything into `dist\legacy\` and the tests into `build\test\`; `build.cmd package` also makes the release zips, the TMLoader product folder and `dist\manifest.json`. `tests\run_tests.cmd` runs every test without a game; `tests\ui_preview_standalone.cmd` renders the menu screens to `screens\`.

## Docs

- `docs/plugin-abi.md`: writing a plugin
- `docs/scripting.md`: writing a script for the Horizon Scripts plugin (AngelScript, runs on every supported game)
- `docs/profile-json.md`: how TM Horizon enables plugins (and what happens without it)
- `docs/plugins/tmx-upload.md`: the TMX Upload plugin (replays offered to TM Horizon or the browser extension)
- `docs/plugins/controller-hotplug.md`: the Controller nachträglich erkennen module (a pad plugged in after the game started works without a restart)
- `docs/plugin-abi.md`, "Standalone builds": modules that also run without Horizon (`controller-hotplug` is built here as a standalone zip)
- `TESTPLAN.md`: my test steps for the game

## Licence

MIT (`LICENSE`). Vendored: Dear ImGui 1.91.5 (MIT), AngelScript 2.38.0 (zlib, Horizon Scripts plugin only), the reflection and game-control modules from my legacy-randomizer repo (MIT), see `third_party/README.md`.
