# Horizon Client

An in-game menu and mod loader for the old TrackMania games: TrackMania Nations ESWC, TrackMania Sunrise, TrackMania Original and, through TMLoader, TrackMania Nations/United Forever. One menu for all modules, a slim bar at the top screen edge, hotkeys with conflict display, one log per start, and a small C plugin ABI so mods stop fighting over Direct3D and game hooks. It is the in-game part of the TM Horizon launcher, but it works on its own.

Version 0.3.1. MIT licence. Built with [Claude Code](https://claude.com/claude-code).

![The menu at 1280x720](docs/images/menu-1280x720.png)

*The menu on a new TrackMania Nations ESWC install, Input Display open. Preview render with the real module list, no game running.*

![The menu at 640x480](docs/images/menu-640x480.png)

*In a small window (here 640x480) the menu shrinks and shows the list or the page.*

![The Input Display styles](docs/images/input-display.png)

*Input Display: Bars, Arrows, Wheel, Minimal and Controller, on screen in a race (top) and with the menu open (bottom). Preview render on a stand-in picture, not a game capture.*

## Supported exes

Only these exes. Any other exe: the host stays off and writes why into its log. No StarForce versions.

| Game | Exe | SHA-256 | Loaded by |
|---|---|---|---|
| TrackMania Nations ESWC | `TmNationsESWC.exe`, unofficial 1.80 (0.1.7.9) | `983bd12724739dcb0f876aeb3a4259154b068940f5167f57ede750ca452d8068` | Legacy Loader |
| TrackMania Sunrise | `TmSunrise.exe`, Win10 community repack | `e59dfc7eaa5314e85b2b0df6f40ef7ba01a23bd5a9525f84407f5dff301df4fe` | Legacy Loader (works through the repack's own `d3d9.dll`) |
| TrackMania Original | `TMORIGINAL.EXE` of the community Online Patch v2 (2023) | `75e129d769f4595ddde54a8ef484162915bc2bf998c1a2139cc963294fe8175e` | Legacy Loader (works through the patch's dgVoodoo `d3d9.dll`) |
| TrackMania Nations Forever | `TmForever.exe` 2.11.26 | `3847cf9f20bfc63914450060ed528c12104f743d96ad23d6e76abd178de8c84f` | TMLoader product |
| TrackMania United Forever | `TmForever.exe` 2.11.26 | `4b6a7b31d86766409e94101f1256cd61dffecb23ea497a40b6617506b2ced2d4` | TMLoader product |
| TrackMania Forever (TMLoader) | TMLoader 2.12.0 | `8c5325d04a10d5e6921afb9931770279cca8cdfc7f66425b6365d0a870f5694a` | TMLoader product |
| TrackMania Forever (TMLoader) | TMLoader 2.12.0-compat | `0a885e194131063be9f78d9e5cf81e0f3017dfc4331d176812109b6aefc633bf` | TMLoader product |

The table lives in [`src/host/exe_known.h`](src/host/exe_known.h) (with PE timestamp and file size where they are checked).

## Features per game

| | ESWC | Sunrise | Original | Forever (TMLoader) |
|---|---|---|---|---|
| Menu, bar, hotkeys, log, layout mode | yes | yes | yes | yes (input only in the game's menus) |
| Game events | map, start, checkpoint, respawn, finish, race end, state | map, start, checkpoint, finish, solo race end, state | as Sunrise | map, start, finish, state, by reading only (no hooks) |
| Login fix (TLS 1.0, error 12157) | yes | none (not known to be needed) | yes | no |
| Autosave & Login-Fix (replay after a finish) | solo and online (your previous run), TMX copy | solo | solo | no |
| Controller hot-plug | yes | yes | yes | no |
| Input Display, WR Hunter, TMX Browser, TMX Upload | yes | yes | yes | yes |
| TMX Browser: Play in the game (no TM Horizon needed) | yes (main menu) | yes (main menu) | yes (main menu) | yes, through the game's own TMX link (main menu) |
| Script widgets in layout mode (Horizon Scripts) | yes | yes | yes | yes |
| Horizon Performance, Horizon Scripts | yes | yes | yes | yes |
| Node Explorer (read-only engine browser) | yes | yes | no | yes |
| TM Horizon opens a map in the running game | yes (main menu) | yes (main menu) | yes (main menu) | no |

Sunrise and Original are found statically (addresses checked against the exes); parts of them have not run in a game yet, see [CHANGELOG.md](CHANGELOG.md). Module docs: [docs/plugins/](docs/plugins/).

## Install

**ESWC, Sunrise, Original:** download `Horizon-Client-<version>-<game>.zip`, unzip it into the game folder (next to the exe) and double-click `Horizon-Install.cmd`. It copies the Legacy Loader `dinput8.dll` next to the exe. If another mod already has a `dinput8.dll` there, that file becomes `dinput8.chain.dll` and keeps working: the loader runs it first.

**TMNF/TMUF:** download `Horizon-Client-<version>-TMLoader.zip` and put its `Horizon Host` folder into `%LOCALAPPDATA%\TMLoader\database\TmForever\products\`, then tick "Horizon Client" in TMLoader.

Keys: **Insert** opens the menu (changeable in the menu), **Esc** closes it, **Ctrl+Insert** moves widgets. Hold **Shift** while the game starts for safe mode (no modules), **Ctrl+Shift** to start without Horizon. Log: `horizon\logs\`, settings: `horizon\settings\`.

## Uninstall

ESWC, Sunrise, Original: double-click `Horizon-Uninstall.cmd` (removes Horizon's `dinput8.dll`, gives `dinput8.chain.dll` its old name back), then delete the `horizon` folder. TMLoader: untick it or delete the `Horizon Host` product folder.

## Safety rules

- **Forever is read-only.** No game hooks, no memory writes, no input changes; game events come from reading the game's objects once a frame. With Competition Patch loaded, game control and reflection writes stay off during a run.
- **No validation changes.** No module touches timing, physics, inputs or the replay of a run. Autosave saves after the run with the game's own save code.
- **TMX rules** for every module, online or not: no checkpoint or trigger display, no ideal or brake line, no disabling of camera or MediaTracker behaviour.
- **Online gate.** Modules marked `solo-only` stop while the game is online; game control (start a map) works only from the main menu and never online.

Details: [docs/plugin-abi.md](docs/plugin-abi.md), "Online gate" and "Competition Patch".

## How it is built

| Piece | File | What it does |
|---|---|---|
| Legacy Loader | `dinput8.dll` (next to the exe, a few KB, no C runtime) | Passes DirectInput through (to `dinput8.chain.dll` if there is one, else to the system DLL) and loads `horizon\host.dll`. Shift = safe mode, Ctrl+Shift = no host. Nothing else. |
| Host | `horizon\host.dll` (TMLoader: `Horizon Host\<version>\HorizonHost.dll`) | Direct3D 9 hook through method tables only (CreateDevice, Present, Reset; coexists with TM Vibrant Shaders and kiero/MinHook mods), Dear ImGui menu and bar, hotkeys, settings, per-start log, plugin loading, game events, online gate. |
| Modules | `horizon\plugins\<id>\` (`<id>.dll` + `plugin.json`) | Native DLLs against [`include/horizon_plugin.h`](include/horizon_plugin.h). |

Build needs the MSVC Build Tools (x86 toolset) and Python 3:

```
build.cmd              everything into dist\legacy\, tests into build\test\
build.cmd package      the same, then the release zips (dist\release\) and dist\manifest.json
tests\run_tests.cmd    every test without a game
tests\run_perf_tests.cmd
```

## Docs

- [docs/plugin-abi.md](docs/plugin-abi.md): writing a module
- [docs/scripting.md](docs/scripting.md): scripts for Horizon Scripts (AngelScript)
- [docs/shared-setup.md](docs/shared-setup.md): one setup for every game (shared setup, export/import, widget anchors)
- [docs/profile-json.md](docs/profile-json.md): how TM Horizon switches modules on
- [docs/client-bridge.md](docs/client-bridge.md): the local protocol between the game and TM Horizon
- [docs/performance.md](docs/performance.md): Horizon Performance and the benchmark
- [docs/plugins/](docs/plugins/): one page per module
- [CHANGELOG.md](CHANGELOG.md), [TESTPLAN.md](TESTPLAN.md) (my test steps, German)

## Licence

MIT ([LICENSE](LICENSE)). Vendored: Dear ImGui 1.91.5 (MIT), AngelScript 2.38.0 (zlib, Horizon Scripts only), Lucide icons (ISC), stb_image (MIT/public domain), and the reflection and game-control modules from my legacy-randomizer repo (MIT); see [third_party/README.md](third_party/README.md). TrackMania is a trademark of Ubisoft/Nadeo; this project is not affiliated with them and ships no game files.
