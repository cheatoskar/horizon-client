# Horizon Host plugin guide

A plugin is a native 32-bit DLL that Horizon Host loads into a TrackMania game. The host owns the Direct3D hook, the Dear ImGui context, input, hotkeys and the game hooks. My plugin only gets what it needs through one table of functions (`HorizonHostApi`).

The contract is `include/horizon_plugin.h` (C, MIT) and `include/horizon_imgui.h` (C++ helper). Two examples live in `plugins/`: `sample` (small, commented) and `eswc-autosave` (real, with game calls).

## Folder layout

```
horizon\plugins\<id>\
    <id>.dll        the plugin (name set by "dll" in plugin.json)
    plugin.json     what the launcher and the host read before loading the DLL
```

`<id>` is lowercase `[a-z0-9-]` and must equal the `id` in `plugin.json` and in `HorizonPlugin_Info`.

## plugin.json

```json
{
  "id": "eswc-autosave",
  "name": "ESWC Autosave",
  "version": "2.0.0",
  "dll": "eswc-autosave.dll",
  "games": ["eswc"],
  "online": "online-ok",
  "abi": 1,
  "description": "Saves your replay after each finish, without the dialogs.",
  "author": "cheatoskar"
}
```

| Field | Meaning |
|---|---|
| `id` | Folder and settings id. |
| `name`, `description`, `author` | Shown in the menu. Keep the description to one short sentence. |
| `version` | Semver. |
| `dll` | File name of the DLL. Default `<id>.dll`. |
| `games` | Any of `"eswc"`, `"sunrise"`, `"original"`, `"tmnf"`, `"tmuf"`. |
| `online` | `"solo-only"` or `"online-ok"`, see Online gate. |
| `abi` | `HORIZON_ABI` the plugin was built against (1 today). |

The same facts are repeated inside the DLL (`HorizonPlugin_Info`). The JSON lets the launcher show the plugin without loading it. If they disagree, the DLL wins.

## Exports and lifecycle

All exports are `__cdecl`, undecorated names.

| Export | Required | When |
|---|---|---|
| `HorizonPlugin_Info` | yes | Before anything else. Returns a static `HorizonPluginInfo` (abi, id, name, version, games, caps, flags, author, description, abi_minor). |
| `HorizonPlugin_Init(host)` | yes | Once, if abi and game match. Return 0 to load. Any other number shows the plugin as Failed with that number (I use 2 for "wrong exe"). |
| `HorizonPlugin_Shutdown` | yes | Before the DLL is unloaded. Stop threads, free memory. `host` is invalid afterwards. |
| `HorizonPlugin_DrawMenu` | with `CAP_MENU` | Every frame the plugin's page is visible, inside an ImGui child. |
| `HorizonPlugin_DrawOverlay` | with `CAP_OVERLAY` | Every frame while the plugin is enabled and not gated. |
| `HorizonPlugin_OnEvent(const HorizonEvent*)` | with `CAP_GAME_EVENTS` | Per subscribed event. |
| `HorizonPlugin_SetEnabled(int)` | with `FLAG_LIVE_TOGGLE` | User switches the plugin on or off without a restart. |

Capabilities (`HORIZON_CAP_*`): MENU, OVERLAY, DEVICE, GAME_EVENTS, GAME_CONTROL. Flags: exactly one of `SOLO_ONLY` / `ONLINE_OK`, plus optionally `LIVE_TOGGLE`.

Define `HORIZON_PLUGIN_BUILD` before including the header, so the exports get `dllexport`.

## Threading

Every call into my plugin, and every call of mine into the host, happens on the game's main thread. Events are delivered synchronously inside the game's own call, so handlers must be short. Calling game code from a handler is allowed if I know it is safe at that point (the autosave plugin does).

The only function that is safe from any thread is `run_on_game_thread(plugin, callback, user)`. It queues the callback for the end of a frame. A worker thread must not call `log`, `set_*`, `post_replay_saved` or anything else directly. In `Shutdown` I wait for my own threads.

## Events

Subscribe with `host->subscribe(plugin, HORIZON_EVENT_BIT(HORIZON_EVENT_FINISH) | ...)`. Without a subscription `OnEvent` is never called. Check `e->size` before reading fields a newer host may have added.

| Event | Fields | ESWC today | Sunrise / Forever |
|---|---|---|---|
| `MAP_LOADED` | `map` (path, uid, name, author, environment, medal times) | yes | planned |
| `RACE_START` | | yes | not yet |
| `RESPAWN` | `respawns` | yes | not yet |
| `CHECKPOINT` | `time_ms`, `checkpoint_index` | yes | not yet |
| `FINISH` | `time_ms`, `is_local_player`, `respawns`, `play_mode`, `is_pb` | yes, local finishes only (race state 1 to 2) | not yet |
| `RACE_END` | `end_screen`, `engine_replay` | yes | not yet |
| `MENU_ENTERED` | | yes | planned |
| `REPLAY_SAVED` | `path`, `source` ("game" or the plugin id) | yes | not yet |
| `ONLINE_CHANGED` | `is_online` (sent to every plugin, gated or not) | yes | not yet |
| `GAME_STATE` | `game_state` | yes | planned |

Notes for ESWC:

- `FINISH` comes right after `CTrackManiaRace::OnFinishLine` returned. `is_pb` is true if the local best changed or the time was below the best before.
- `RACE_END` comes in two forms. After `StopReplayRecordAndKeepCopy`, when the previous-race replay changed: `end_screen = 0`, `play_mode` NET, `engine_replay` is the new copy. After the solo race-end function: `end_screen = 1`, `play_mode` SOLO, `engine_replay` is the current replay. If it happens inside `OnFinishLine`, the host still delivers it after `FINISH`.
- So "save after a finish" is: remember `FINISH`, act on the next `RACE_END`. A `RACE_END` without a pending finish means nothing. `RACE_START` and `MAP_LOADED` clear a pending finish.
- `engine_race`, `engine_replay`, `engine_challenge` are raw game pointers. They only make sense to a plugin that carries address tables for that exact exe. Verify `game_info()->exe_sha256` first.

Times are milliseconds, `-1` = unknown. Strings may be `""` when the host cannot read them yet.

## Settings and data folder

- `get_int/set_int/get_float/set_float/get_string/set_string(plugin, key, ...)`: stored by the host in `horizon\settings\<id>.json`, written on change. Defaults are the `fallback` argument, so there is nothing to register.
- `data_folder(plugin)` / `data_folder_w(plugin)`: `horizon\data\<id>\` with a trailing backslash, already created. Use it for caches and history, never the game folder.
- `log(plugin, HORIZON_LOG_INFO, "fmt", ...)`: one line in the host log, prefixed with the id. Plain printf formats, UTF-8.

## Hotkeys

```cpp
int32_t h = host->register_hotkey(host->plugin, "save", "Save replay now", "F9", onSave, nullptr);
const char* shown = host->hotkey_text(host->plugin, h); // key now bound, "" = none
```

`action` is a stable id, `name` is shown in the menu, the key text is `"F9"`, `"Ctrl+Tab"`, `"Shift+F11"`, `"Insert"`, or `""` for no default. The user can rebind it and the host shows conflicts. The callback runs on the game thread once per press, not while the menu captures text, not while the plugin is off or gated online.

## Dear ImGui

The host and every plugin that draws must use the same Dear ImGui, 1.91.5 (`HORIZON_IMGUI_VERSION_NUM` 19150) with the default `imconfig.h`. With the static runtime (`/MT`) each DLL has its own heap and its own "current context", so in `Init` I call:

```cpp
#include "imgui.h"
#include "horizon_imgui.h"
bool ok = horizon_imgui_attach(host); // version check, host allocator, host context
```

The allocator matters: memory ImGui allocates inside my DLL is freed by the host and the other way round. Without the host's allocator that crashes. If attach returns false I log it and still work without a page (return early in `DrawMenu`). `host->style()` gives the TM Horizon colours and fonts (fonts are NULL until the first frame has been drawn).

Draw in `DrawMenu` with normal widgets. For an overlay I use `ImGui::GetForegroundDrawList()` or my own borderless window.

## Online gate

`online` in `plugin.json` and the flag in `Info` say what the plugin may do online:

- `solo-only` (`FLAG_SOLO_ONLY`, the default if nothing is set): while the game is online (LAN or internet race, server browser) the host stops its events, overlay, hotkeys and queued calls. `ONLINE_CHANGED` still arrives. `actions_allowed(plugin)` tells me if I may act now.
- `online-ok` (`FLAG_ONLINE_OK`): allowed online, but the TMX rules still apply, because TrackMania Exchange rejects tools that give an edge or that rewrite results.

The TMX rules for every plugin, online or not:

- no checkpoint or trigger display, no ideal line or brake line,
- nothing that changes physics, timing, input or replays of a run (saving a replay file after the fact is fine, editing the run is not),
- no disabling of camera or MediaTracker behaviour.

## Competition Patch (Forever)

Competition Patch is a mod for TMNF/TMUF that makes runs comparable. While it is loaded:

- my plugin never touches timing, physics, input or replay data,
- reflection writes and scripts are off while CP is loaded and a run is going,
- `actions_allowed(plugin)` returns 0 during such a run and `HORIZON_ERR_BLOCKED_ONLINE` comes back from game control calls.

I do not work around this. Read-only plugins (overlays, loggers) are fine.

## Device access (renderers)

A plugin that renders itself (shaders, post effects) sets `HORIZON_CAP_DEVICE` and calls `set_device_callbacks` with a `HorizonDeviceCallbacks` table: `device_created`, `adjust_present_params`, `before_reset`, `after_reset`, `pre_present`. `host->device()` returns the `IDirect3DDevice9*` (NULL before creation). Per frame the order is: device plugins' `pre_present`, then the host's ImGui (menu and overlays), then `Present`. I never hook `Present`, `EndScene`, `Reset` or DirectInput myself.

## Other host functions

| Function | Use |
|---|---|
| `game_info()` | game bit, exe sha256, exe base (rebase link addresses: `base - 0x400000 + link`), folders, host version |
| `engine_pointer(HORIZON_ENGINE_APP / RACE / CHALLENGE)` | raw game objects, NULL if unknown |
| `post_replay_saved(plugin, utf8 path)` | tell everyone I wrote a replay file |
| `play_map_file`, `return_to_menu` | game control (`CAP_GAME_CONTROL`); return a `HorizonResult` |
| `get_game_state`, `is_online`, `menu_open` | current state |
| `request_restart_note(plugin, why)` | "applies after restart" note in the menu |
| `reflect_offset`, `reflect_method` | read-only engine reflection by class and member name |

## ABI versioning

- Every struct starts with its own size (or the ABI number). The host's `HorizonHostApi.size` and `abi_minor` say how much of the table exists.
- Changes are additive only: new fields go at the end, new events get new numbers, nothing is reordered or removed.
- `HORIZON_ABI_MINOR` counts additive changes. A plugin built against a newer minor still loads in an older host. Before using a function added later I check `host->size` (or `abi_minor`) and fall back if it is missing.
- `HORIZON_ABI` changes only if something cannot stay compatible. The host refuses a plugin with a different `abi`.

## Building

32-bit, MSVC, static runtime, `/W4 /WX`. Plugins are built by a `build_plugin.cmd` fragment in the plugin's folder, which `build.cmd` calls with the Visual Studio x86 environment set, the working directory at the repo root, `%CXX%` set to the compile command, and `build\imgui.lib` built:

```bat
@echo off
set "OD=dist\legacy\horizon\plugins\my-plugin"
if not exist build\plugins\my-plugin mkdir build\plugins\my-plugin
if not exist "%OD%" mkdir "%OD%"
%CXX% /Fo"build\plugins\my-plugin\\" /Fd"build\plugins\my-plugin\p.pdb" plugins\my-plugin\my_plugin.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT ^
  /OUT:"%OD%\my-plugin.dll" /PDB:build\plugins\my-plugin\my-plugin.pdb ^
  build\plugins\my-plugin\*.obj build\imgui.lib user32.lib kernel32.lib gdi32.lib || exit /b 1
copy /y plugins\my-plugin\plugin.json "%OD%\plugin.json" >nul || exit /b 1
```

`plugins\sample` builds into `build\plugins\sample` instead of `dist`, so it never ships.

## Testing without the game

`tests\build_fake_host.cmd` builds both plugins and `build\test\fake_host.exe`, then runs them:

```
build\test\fake_host.exe <plugin dll> [--game eswc|sunrise|tmnf] [--expect-init <n>]
```

The fake host stubs the whole `HorizonHostApi`, keeps settings in memory, uses `build\test\data\<id>\` as data folder, runs a headless ImGui frame, sends a short event sequence, presses the hotkeys, toggles `SetEnabled`, calls `Shutdown` and unloads the DLL. `engine_pointer` returns NULL, so a plugin with game addresses must log and skip instead of calling game code.
