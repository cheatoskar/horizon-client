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
  "name": "Autosave & Login-Fix",
  "version": "2.1.0",
  "dll": "eswc-autosave.dll",
  "games": ["eswc", "sunrise", "original"],
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
| `enabled_by_default` | `false`: off until switched on in the menu when there is no profile.json (default `true`). Anything that draws on screen during a race or talks to the network should ship `false`. |
| `startup_only` | `true`: the module must hook the game at start (before the Direct3D device exists, or game code that only runs once). Switching it on in a running game then says "Needs a restart" instead of loading it. Default `false`: the host loads it the moment the player switches it on. |
| `ui` | Optional: `{ "min_w": 520, "min_h": 360, "pref_w": 860, "pref_h": 600 }`, the size of the module's own window (a popped-out panel, the free window from the top bar) in logical pixels at UI scale 1. The host multiplies by `ui_metrics()->scale`, opens the window at `pref` (or at the size the player left it), clamps it to the game window below the top bar and the menu's header, and never makes it smaller than `min`; content that does not fit scrolls. Defaults: min 480 x 320, pref 820 x 560. Values outside 160-4000 are ignored. See "ABI 1.4". |
| `icon` | PNG file name in the plugin's folder, e.g. `"icon.png"` (no sub-folders, no `..`). Square, 64-256 px, transparent corners welcome; shown as a rounded square in the menu and the bar. Missing or unreadable: coloured initials. A second set may lie next to it as `icon-alt.png`: the player picks "Distinct" (`icon`, default) or "Saturn" (`icon-alt.png`) in General -> Display (host.json `ui.icon_alt`, independent of the launcher); the other file is the fallback, then the initials. The modules in this repo ship the flat Distinct set as `icon.png` and the Saturn set as `icon-alt.png` (since 0.3.0; before, the other way round). Without `icon`, `icon.png` is tried. |

The same facts are repeated inside the DLL (`HorizonPlugin_Info`). The JSON lets the launcher show the plugin without loading it. If they disagree, the DLL wins.

## Exports and lifecycle

All exports are `__cdecl`, undecorated names.

| Export | Required | When |
|---|---|---|
| `HorizonPlugin_Info` | yes | Before anything else. Returns a static `HorizonPluginInfo` (abi, id, name, version, games, caps, flags, author, description, abi_minor). |
| `HorizonPlugin_Init(host)` | yes | Once, if abi and game match. Return 0 to load. Any other number shows the plugin as Failed with that number (I use 2 for "wrong exe"). Usually at game start; a module that was off at start runs `Init` later, in the middle of the game, when the player switches it on (see "Loading while the game runs"). |
| `HorizonPlugin_Shutdown` | yes | Before the DLL is unloaded. Stop threads, free memory. `host` is invalid afterwards. |
| `HorizonPlugin_DrawMenu` | with `CAP_MENU` | Every frame the plugin's page is visible, inside an ImGui child. |
| `HorizonPlugin_DrawOverlay` | with `CAP_OVERLAY` | Every frame while the plugin is enabled and not gated, also while the menu, a popped-out panel or a free plugin window is open (a confirm popup opened from it keeps working). |
| `HorizonPlugin_OnEvent(const HorizonEvent*)` | with `CAP_GAME_EVENTS` | Per subscribed event. |
| `HorizonPlugin_SetEnabled(int)` | with `FLAG_LIVE_TOGGLE` | User switches the plugin on or off without a restart. |

Capabilities (`HORIZON_CAP_*`): MENU, OVERLAY, DEVICE, GAME_EVENTS, GAME_CONTROL. Flags: exactly one of `SOLO_ONLY` / `ONLINE_OK`, plus optionally `LIVE_TOGGLE`.

Define `HORIZON_PLUGIN_BUILD` before including the header, so the exports get `dllexport`.

## Threading

Every call into my plugin, and every call of mine into the host, happens on the game's main thread. Events are delivered synchronously inside the game's own call, so handlers must be short. Calling game code from a handler is allowed if I know it is safe at that point (the autosave plugin does).

The only function that is safe from any thread is `run_on_game_thread(plugin, callback, user)`. It queues the callback for the end of a frame. A worker thread must not call `log`, `set_*`, `post_replay_saved` or anything else directly. In `Shutdown` I wait for my own threads.

## Events

Subscribe with `host->subscribe(plugin, HORIZON_EVENT_BIT(HORIZON_EVENT_FINISH) | ...)`. Without a subscription `OnEvent` is never called. Check `e->size` before reading fields a newer host may have added.

| Event | Fields |
|---|---|
| `MAP_LOADED` | `map` (path, uid, name, author, environment, medal times) |
| `RACE_START` | |
| `RESPAWN` | `respawns` |
| `CHECKPOINT` | `time_ms`, `checkpoint_index` |
| `FINISH` | `time_ms`, `is_local_player`, `respawns`, `play_mode`, `is_pb` |
| `RACE_END` | `end_screen`, `engine_replay` |
| `MENU_ENTERED` | |
| `REPLAY_SAVED` | `path`, `source` ("game" or the plugin id) |
| `ONLINE_CHANGED` | `is_online` (sent to every plugin, gated or not) |
| `GAME_STATE` | `game_state` |

Which game delivers what, and how (`poll` = read once a frame from the game's objects, nothing hooked; `hook` = inline hook with the prologue checked byte for byte):

| Event | ESWC | Sunrise (first test) | Original (first test, same as Sunrise) | Forever (TMNF/TMUF) |
|---|---|---|---|---|
| `MAP_LOADED` | poll | poll | poll | poll (uid/author/path only for maps gamectl started; times from the challenge) |
| `RACE_START` | poll (start counter) | hook (`OnStartLine`) | hook (`OnStartLine`) | poll (RaceState to 1, also each restart) |
| `RESPAWN` | poll (respawn counter) | no | no | no |
| `CHECKPOINT` | poll (checkpoint counter) | hook (`OnCheckpoint`) | hook (`OnCheckpoint`) | no |
| `FINISH` | hook (`OnFinishLine`) | hook (`OnFinishLine`) | hook (`OnFinishLine`) | poll (RaceState 1 to 2, time read once) |
| `RACE_END` | hook (replay stop, solo race end) | hook (solo race end, no replay pointer) | hook (solo race end, no replay pointer) | no (no reliable signal; the end screen is the `END_RACE` game state) |
| `MENU_ENTERED`, `GAME_STATE`, `ONLINE_CHANGED` | poll | poll | poll | poll |
| `REPLAY_SAVED` | yes | no | no | no |

Original uses the Sunrise code path (same engine); its addresses are matched statically and not run in game yet. Only the exe of the community "Online Patch v2" is supported. A replay watched from the menu is reported as "Loading" on Sunrise and Original.

Forever never hooks the game or writes game memory, because Competition Patch 2.0 may treat changed code bytes as tampering. The one input exception (host 0.3.2): without Competition Patch the host holds the game's DirectInput keyboard back while it has the keyboard (menu, layout mode, a focused text field), through the method table of the system `dinput8.dll` devices, so Enter and Backspace typed into the menu do not restart or respawn the car; the mouse is never touched there. With Competition Patch loaded the host does nothing with the keyboard: DirectInput is not touched and no key message is held back or made. That also holds when Competition Patch is loaded after the host: `TMCompPatch.dll` is looked for at most once a second; once it is there the original DirectInput functions go back into their slots (one log line) and the host treats the patch as loaded. Everything above is read through reflection member names with every read SEH-guarded. Game states there: `SOLO_RACE` while driving, `END_RACE` once RaceState is finished, `NET_RACE` (online, also with an editor object in the game's editor slot), `EDITOR`, `REPLAY` (replay editor or another race class), `LOADING` (map set, no race yet), `MAIN_MENU`. TMNF/TMUF keep a `CTrackManiaEditorPuzzle` in the editor slot under every solo and network race (as ESWC does): under a race it counts as that race (host 0.3.2; 0.3.1 reported `EDITOR` and play mode editor test then). Replay and editor give no run events. The editor/replay split and the finish time on the end screen are unverified in game (first test).

Notes for ESWC:

- `FINISH` comes right after `CTrackManiaRace::OnFinishLine` returned. `is_pb` is true if the local best changed or the time was below the best before.
- `RACE_END` comes in two forms. After `StopReplayRecordAndKeepCopy`, when the previous-race replay changed: `end_screen = 0`, `play_mode` NET, `engine_replay` is the new copy. After the solo race-end function: `end_screen = 1`, `play_mode` SOLO, `engine_replay` is the current replay. If it happens inside `OnFinishLine`, the host still delivers it after `FINISH`.
- So "save after a finish" is: remember `FINISH`, act on the next `RACE_END`. A `RACE_END` without a pending finish means nothing. `RACE_START` and `MAP_LOADED` clear a pending finish.
- `engine_race`, `engine_replay`, `engine_challenge` are raw game pointers. They only make sense to a plugin that carries address tables for that exact exe. Verify `game_info()->exe_sha256` first.

Times are milliseconds, `-1` = unknown. Strings may be `""` when the host cannot read them yet.

## Settings and data folder

- `get_int/set_int/get_float/set_float/get_string/set_string(plugin, key, ...)`: stored by the host in `horizon\settings\<id>.json` (with the shared setup on and a module made for every game: `%APPDATA%\Horizon Client\shared\<id>.json`, [shared-setup.md](shared-setup.md)), written on change. Read at start; a module may cache what it read. Defaults are the `fallback` argument, so there is nothing to register.
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

If my `DrawMenu`, `DrawOverlay` or a panel draw function throws (C++ exception or access violation), the host switches only my plugin off until the next start, closes the ImGui windows, child windows, IDs, style vars and colours I left open, logs "crashed in DrawMenu" (or wherever it was) and shows the toast "<name> stopped after an error". The menu and the other plugins keep working, and my `set_host_flag` bits end with me.

Draw in `DrawMenu` with normal widgets. For an overlay I use `ImGui::GetForegroundDrawList()` or my own borderless window.

### Icons

The host merges a subset of the Lucide icons (ISC licence) into all its fonts, so a plugin can draw them with any font, including the default one. `horizon_imgui.h` has `HZ_ICON_<NAME>` (codepoint) and `HZ_ICON_<NAME>_U8` (UTF-8 string) for each icon, for example `ImGui::Text(HZ_ICON_SETTINGS_U8 " Settings");`. The codepoints are Lucide's own and ABI: they never change, new icons are only added. A plugin cannot add icons or fonts to the shared atlas itself. If the host has no icon font (resource missing), the icons show as `?`.

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
- reflection writes are off while CP is loaded and a run is going; Horizon Scripts does not load scripts with the `game-control` permission while CP is loaded (read-only scripts keep running, see `docs/scripting.md`),
- `actions_allowed(plugin)` returns 0 during such a run and `HORIZON_ERR_BLOCKED_ONLINE` comes back from game control calls.

I do not work around this. Read-only plugins (overlays, loggers) are fine.

## Device access (renderers)

A plugin that renders itself (shaders, post effects) sets `HORIZON_CAP_DEVICE` and calls `set_device_callbacks` with a `HorizonDeviceCallbacks` table: `device_created`, `adjust_present_params`, `before_reset`, `after_reset`, `pre_present`. `host->device()` returns the `IDirect3DDevice9*` (NULL before creation). Per frame the order is: device plugins' `pre_present`, then the host's ImGui (menu and overlays), then `Present`, then `post_present(user, device, hresult)` (ABI minor 3: the field sits at the end of the table, an older host never calls it; check `host->abi_minor >= 3`). `post_present` is for frame timing and frame pacing (Horizon Performance waits there, so the game reads input right after the wait). I never hook `Present`, `EndScene`, `Reset` or DirectInput myself.

## Other host functions

| Function | Use |
|---|---|
| `game_info()` | game bit, exe sha256, exe base (rebase link addresses: `base - 0x400000 + link`), folders, host version |
| `engine_pointer(HORIZON_ENGINE_APP / RACE / CHALLENGE)` | raw game objects, NULL if unknown |
| `post_replay_saved(plugin, utf8 path)` | tell everyone I wrote a replay file |
| `play_map_file`, `return_to_menu` | game control (`CAP_GAME_CONTROL`); return a `HorizonResult`. The map file may be anywhere (a plugin's data folder). ESWC, Sunrise, Original: copied into `GameData\Tracks\Challenges\Downloaded\_Horizon\`, one rescan of the game's map list, the copy is found by its file name among the game's challenge infos (info+0x14 file, `FileName`; the gold time in its header only if no file name can be read), then the menu gets what the game's track browser sets when a map is activated: `SelectedChallengeInfo` = the info, `ChallengeType` 3, menu result 1 (host 0.3.0; it used to count new list entries and failed with "expected 1, got 2", and on ESWC it set the result 0xf, which the game does not treat as "play" and which reopened the track browser instead). Any menu page, not in a race, the editor or online; refused while a menu button press is still pending. TMNF/TMUF (host 0.3.0): handed to the game like a double-clicked map (the game's single-instance message, `WM_COPYDATA` 'ELIF'), main menu and offline only, no rescan, no memory write; `HORIZON_OK` means handed over, the game opens it a moment later (not run in a game yet). `return_to_menu` (solo race or its end screen): ESWC, Sunrise, Original do what Esc > Quit does (race result "Esc", then the quit dialog's own Quit handler, a few frames later; host 0.3.1 fix: the race's quit button went back to the puzzle editor every solo race runs under); TMNF/TMUF press `CTrackManiaRace::ButtonQuitOnClick`. `HORIZON_OK` means started; the menu comes within a second; a second call while leaving does nothing |
| `get_game_state`, `is_online`, `menu_open` | current state |
| `request_restart_note(plugin, why)` | "applies after restart" note in the menu |
| `reflect_offset`, `reflect_method` | read-only engine reflection by class and member name |
| `create_texture_rgba`, `release_texture` (ABI 1.1) | an ImGui texture from RGBA8 pixels (map thumbnails), game thread; release it before Shutdown returns |
| `save_replay(plugin, name)` (ABI 1.1) | saves the run's replay like the game's "Save replay", without dialogs. ESWC (host 0.2.0): on the solo end-race screen the current run, elsewhere (menus, LAN/online after a race) the previous race's copy; `name` without extension, `""` = the game's default `<map>(mm'ss''cc)`; never overwrites (`name (2)` ...); sends `REPLAY_SAVED`. `HORIZON_ERR_WRONG_STATE` while a solo race runs or nothing is there to save, `HORIZON_ERR_NOT_SUPPORTED` on Sunrise, Original and Forever. Game thread. |
| `register_panel`, `unregister_panel`, `panel_visible` (ABI 1.2) | see Panels |
| `set_host_flag(plugin, flag, on)`, `host_flags()` (ABI 1.2) | `HORIZON_HOST_FLAG_CAPTURE`: a photo or video is being taken; the bar and windows with "Hide in photos and videos" disappear. Each plugin sets its own bits; `host_flags()` is the OR. |
| `open_page(plugin, panel_key)` (ABI 1.3) | opens the Horizon menu on this plugin's page, for an "open my page" hotkey. `panel_key` `NULL` or `""` = Settings tab (or the plain page), else that panel's tab (`HORIZON_ERR_INVALID_ARG` if there is no such panel, the page still opens on Settings). Called while the menu already shows this page it closes the menu (toggle). Game thread only; `HORIZON_ERR_WRONG_STATE` during a capture. |
| `notify(plugin, &toast)` (ABI 1.3) | a toast in the corner of the screen, see "ABI 1.3" |
| `bridge_request(...)`, `bridge_state()` (ABI 1.3) | ask the TM Horizon launcher on the same PC (TMX search, map files, record replays), see "ABI 1.3" |
| `device()` | the game's `IDirect3DDevice9*` for every plugin (callbacks only with `CAP_DEVICE`) |

ABI 1.1, 1.2, 1.3 and 1.4 functions sit at the end of `HorizonHostApi`. A plugin built against 1.0 never sees them; one built against a newer minor checks `host->abi_minor` (>= 1, >= 2, >= 3 or >= 4) before calling them.

## Panels (ABI 1.2)

A panel is a piece of a plugin's UI that the host places. Register it once (Init is fine):

```c
HorizonPanelDef d = {0};
d.size = sizeof(d);
d.key = "events";            /* [a-z0-9-], stable: the saved layout uses plugin id + key */
d.title = "Events";          /* tab and window title, one or two words */
d.icon = HZ_ICON_SCROLL_TEXT; /* horizon_imgui.h, 0 = none */
d.flags = HORIZON_PANEL_MENU_PAGE; /* or HORIZON_PANEL_HUD */
d.draw = drawEvents;         /* void draw(void* user): the body only, no Begin/End, no heading */
d.default_x = 0.05f; d.default_y = 0.2f; d.default_w = 0.22f; d.default_h = 0.3f; /* fractions; w/h 0 = fit */
host->register_panel(host->plugin, &d);
```

- Every panel is a tab on the plugin's page in the menu, after "Settings" (the plugin's `DrawMenu`, if it has one). A plugin without panels keeps the plain page.
- "Pop out" (top right of the tab) puts the panel into its own window. With the menu open the window sits above the menu and can be moved and resized; its close button and **Dock** (right-click) bring it back. A panel with a size (`default_w` > 0) opens at its saved size, else at `default_w` x `default_h` of the screen, at least the plugin's `ui.min_w` x `ui.min_h` (plugin.json), clamped to the screen below the top bar and the menu's header; smaller than its content it scrolls. Right-click in the window, or the tab of a popped-out panel, shows its options: **Keep open** (stays with the menu closed), **Lock layout** (no moving or resizing), **Click-through** (with the menu closed, clicks go to the game), **Hide in photos and videos** (on by default, see `HORIZON_HOST_FLAG_CAPTURE`), **Opacity** 30-100 %.
- With the menu closed, kept windows have no title bar and can't be moved; they take mouse clicks unless they are click-through.
- `HORIZON_PANEL_HUD`: starts popped out and kept open (a timer, a split display). The player can still bring it into the menu.
- `HORIZON_PANEL_HUD | HORIZON_PANEL_NO_BACKGROUND` (host 0.3.1, ABI 1.5; an older host ignores the bit): a widget without window background or border while the menu is closed, only what the panel draws, so it can sit bare in a corner (Input Display). With the menu open and in layout mode the window shows its frame as usual, so it can be found and moved. The window keeps a small padding in both states (the content does not jump) and takes no mouse input while the menu is closed.
- `HORIZON_PANEL_HUD | HORIZON_PANEL_START_HIDDEN` (host 0.3.0, no ABI change; an older host ignores the bit and shows the widget at once): a widget that is off until the player shows it, from the list of hidden widgets in layout mode ("+ Title") or "Show" on its tab. Its tab then has "Hide". Reset in layout mode leaves hidden ones hidden. Horizon Scripts registers every script widget (`UI::AddWidget`) this way.
- The host saves the layout per plugin id + key in `horizon\settings\host.json` (`panel.<plugin>.<key>`), or with the shared setup on in `%APPDATA%\Horizon Client\shared\host.json` ([shared-setup.md](shared-setup.md)).
- Panels of a plugin that is off, failed or gated online are not drawn. `panel_visible(plugin, key)` says whether the body was drawn in the last frame (skip work otherwise).
- The sample plugin shows both kinds: "Events" (a tab) and "Clock" (a HUD window).

Fonts: `style()->font_*` keep the same `ImFont*` for the whole start. When the player changes the content scale the host rebuilds the glyphs inside those same objects between two frames, so a kept pointer stays valid; `style()->scale` changes at that moment. Read both from `style()` when drawing (the fonts are NULL until the first frame).

### Widget places and layout mode (host 0.2.0, no ABI change)

- A HUD panel that has no saved place yet goes to a fixed slot at a screen edge, never to a random spot. The slot comes from `default_x` / `default_y`: x below 0.34 = left, above 0.6 = right, else centre; y the same for top / middle / bottom. Input Display (0.44, 0.74) lands bottom centre, WR Hunter (0.70, 0.22) top right. Two widgets in one slot are stacked (top slots downwards, bottom slots upwards).
- Moving a widget (with the menu open or in layout mode) snaps it to the screen edges, the centre lines and the edges of other widgets.
- Layout mode ("Edit layout", Ctrl+Insert by default, also a button in the top bar and on General): every widget gets a frame. Drag to move, the corner to resize, the two buttons top right minimise it to a small icon at the nearest screen edge (click the icon to bring it back) or hide it (the bar at the top lists hidden widgets to show again). The same key, Esc or "Done" ends it. While it is on, the host draws its own pointer and keeps the mouse (ESWC and Sunrise: also DirectInput) from the game, as with the menu. On TMNF/TMUF (Competition Patch rules) it only starts in the game's menus, never during a race, and ends by itself when a race starts.
- Resizing a widget whose window fits its content (w/h 0) scales it: the host sets `style()->scale` and the window font scale for that panel's draw call, so draw with `style()->scale` (read inside `draw`) and the widget grows and shrinks as a whole. A widget with a fixed size (w/h > 0) gets the new size.
- Saved in `settings\host.json` as before (`panel.<plugin>.<key>`, two fields appended: minimised, scale). Older hosts ignore the extra fields. Since 0.3.1 four more: the anchor (nearest edge per axis and the gap as a share of the window), so a widget keeps its edge at any window size; only the player's moves change it, never a smaller window ([shared-setup.md](shared-setup.md)).

## Free plugin windows (host, no ABI change)

Every plugin icon in the top bar has two clicks. Left click opens (or closes) the plugin's page as a free window: the plugin's Settings (`DrawMenu`) and one tab per panel, an "On" switch, no menu frame or sidebar. It can be moved and resized, has a close button, and several plugins' windows can be open at once. Right click opens the Horizon menu on that plugin's page (the settings). The icon tooltip says "Linksklick: Fenster Â· Rechtsklick: Einstellungen".

- Position and size are saved per plugin in `settings\host.json` as `pwin.<plugin id>` (`x,y,w,h` in pixels). A window without a saved size opens at the plugin's `ui.pref_w` x `ui.pref_h` (plugin.json, default 820 x 560) times the UI scale; it never gets smaller than `ui.min_w` x `ui.min_h` (default 480 x 320, scaled). The rect is clamped to the screen when the window opens and again when the display size changes (Reset), and it starts below the top bar and the menu's header row, so both stay clickable. Content wider or taller than the window scrolls. Whether the window is open is not saved.
- The top bar stays in front of these windows. In a small game window (640x480) its buttons shrink to icons (Menu, Layout, Log, TM Horizon; names in the tooltips) when the module icons do not all fit.
- The window calls the same `DrawMenu` and panel draw functions as the menu page. `ui_metrics()` reports the window's room while they run (see "ABI 1.4"), so a plugin page can switch to its compact layout. A panel that was popped out into its own window shows its "in its own window" note in the tab, as in the menu.
- Input rules: the windows are ordinary ImGui windows drawn after the menu, so they sit above the menu and its dim layer. With the menu closed, mouse clicks over a window (or the bar) are kept from the game, mouse moves are not; a focused text field also takes the keyboard (the game does not see those keys, neither as window messages nor through DirectInput). The pointer is drawn while a window is open. The game's DirectInput mouse is only blocked while the menu is open. Whenever the host has the keyboard (menu, layout mode, a focused text field), no key press or character reaches the game; the release of a key the game saw go down always does, so nothing stays held.
- `HorizonHost_GetUiState` does not report these windows (bit 4 stays for detached panels).

## ABI 1.3

Minor 3 has four new functions at the end of `HorizonHostApi` and one new callback at the end of `HorizonDeviceCallbacks`. They came from three branches (TMVS modules, client mods, Horizon Performance) and were merged into this one minor. The order is fixed and never changes:

| Struct | Field (in this order, after the 1.2 fields) | What for |
|---|---|---|
| `HorizonHostApi` | `open_page` | open the Horizon menu on the plugin's page (TMVS F8), see "Opening the plugin's page" |
| `HorizonHostApi` | `notify` | toasts, see "Toasts" |
| `HorizonHostApi` | `bridge_request`, `bridge_state` | ask TM Horizon (TMX search, map files, record replays), see "The TM Horizon bridge" |
| `HorizonDeviceCallbacks` | `post_present` | frame timing and frame pacing after `Present`, see "Device access" |

`open_page` comes first because TMVS 2.0 (built against the `tmvs-modules` header) reads it right after `host_flags`; that binary keeps working. `offsetof(HorizonHostApi, open_page)` equals the size of the 1.2 table; the unit tests check this with `static_assert`, and a test plugin built against the 1.2 header (`tests/abi12/`) is loaded by the 1.3 host in the test run.

Compatibility both ways:

- A plugin built against 1.2 (or older) never sees the new fields. Its `HorizonDeviceCallbacks` is shorter; the host copies only `cb->size` bytes, so `post_present` stays NULL and is never called.
- A plugin built against 1.3 checks `host->abi_minor >= 3` before calling `open_page`, `notify` or the bridge (the client mods use `ck::abiAtLeast(host, 3)`, which is the same test), and then a NULL check on the pointer. A plugin that cannot work without them (TMX Browser) returns non-zero from `Init` on an older host and shows as failed. A plugin that only wants a toast (WR Hunter) skips the call. An older host never calls `post_present`; Horizon Performance then measures frame times in `pre_present` only and the limiter stays off.
- `HorizonHost_Version` (the host's own version, not the ABI) stays 0.2.0: ABI 1.3 is part of host 0.2.0.

## ABI 1.4: `ui_metrics`

One function at the end of `HorizonHostApi`, after `bridge_state`:

```c
const HorizonUiMetrics* (HORIZON_CALL* ui_metrics)(void);

typedef struct HorizonUiMetrics {
    uint32_t size;               /* sizeof(HorizonUiMetrics): check before reading fields added later */
    float scale;                 /* = style()->scale: screen height / 1080 (1..2) x content scale */
    float content_scale;         /* the player's content scale, 0.8 .. 1.25 */
    float display_w, display_h;  /* the game's back buffer, pixels */
    float menu_w, menu_h;        /* the Horizon menu window, pixels */
    float page_w, page_h;        /* room of the container the plugin is drawn in right now, pixels (see below) */
} HorizonUiMetrics;
```

**`page_w` / `page_h` contract (host 0.3).** They always describe the content region of the container that is calling the plugin's `DrawMenu` or panel draw function right now: the docked menu page, a popped-out panel window or a free plugin window (top bar, left click). The host sets them right before each call and restores them afterwards; outside a draw call they hold the menu page's room (0 before a page was drawn). An auto-sized widget (a HUD panel with `default_w`/`default_h` 0, drawn with the menu closed) gets 0 x 0: draw at your natural size there. No new fields, no ABI change: a plugin built against 1.4 gets this from a 0.3 host as is.

Inside `DrawMenu` and a panel draw function `ImGui::GetContentRegionAvail()` stays the ground truth; `page_w` is never larger. Read `page_w` where the content region is not at hand (before a child window, to size columns ahead). A page that does not fit should switch to a compact layout instead of clipping:

```c
const HorizonUiMetrics* m = host->abi_minor >= 4 && host->ui_metrics ? host->ui_metrics() : NULL;
float room = ImGui::GetContentRegionAvail().x;
if (m && m->size >= sizeof(HorizonUiMetrics) && m->page_w > 0 && m->page_w < room) room = m->page_w;
const bool compact = room < 560.0f * scale;   /* scale = m->scale or style()->scale */
```

In the compact layout:

- one column: cards and side panes below each other, side lists 130-170 px x scale instead of 220-260,
- buttons and checkboxes in a row wrap to the next line when the next one does not fit (`plugins/client-common/ckui.h`: `ck::sameLineIfFits`, `ck::checkboxWidth`, `ck::buttonWidth`, `ck::narrow`, `ck::roomWidth`),
- long lines wrap (`TextWrapped`, `PushTextWrapPos(0)`), tables drop secondary columns, input widths are `min(wanted * scale, room - label)`,
- a header with a title and chips puts the chips on their own line (or drops them) instead of drawing them over the title.

A free or popped-out window opens at the plugin's `ui.pref_w` x `ui.pref_h` (plugin.json) and never gets smaller than `ui.min_w` x `ui.min_h`; set `min_w` to the narrowest width the compact layout still works at.

The player sets the menu size (Auto, 60, 70, 80 or 90 % of the screen; Auto is 80 % at 1440p and higher, 85 % below) and the content scale (80-125 %; Auto is 90 % at 1440p and higher, 100 % below) in General -> Display, or changes the content scale with Ctrl + mouse wheel while the menu is open. A plugin page with a wide layout (columns, tables) picks fewer columns from the room it has, for example `const int columns = room > 1100.0f * scale ? 3 : room > 700.0f * scale ? 2 : 1;`.

The pointer stays valid for the whole start; the values change between frames. `offsetof(HorizonHostApi, ui_metrics)` equals the size of the 1.3 table (unit test `static_assert`), so a plugin built against 1.3 never sees it, and one built against 1.4 checks `host->abi_minor >= 4` (or `host->size`) first.

## ABI 1.5: `suspend_widgets`, `HORIZON_PANEL_NO_BACKGROUND`

```c
HorizonResult (HORIZON_CALL* suspend_widgets)(HorizonPluginHandle plugin, const char* plugin_id, const char* key_prefix, int32_t on);
```

Hides other plugins' widgets while this plugin needs the screen. The Legacy Randomizer shows the map with its picture and the medals itself, so during a run it hides the Clock and Map Info script widgets, which would only cover its panels:

```c
if (host->abi_minor >= 5 && host->suspend_widgets) {
    host->suspend_widgets(host->plugin, "scripts", "w-clock-clock-", 1);       /* run starts */
    host->suspend_widgets(host->plugin, "scripts", "w-map-info-map-info-", 1);
}
/* ... run ends: the same calls with on = 0 */
```

- `plugin_id`: whose widgets; `key_prefix`: panel keys that start with it (`""` or NULL = every widget of that plugin). Script widgets have the key `w-<script id>-<widget id>-<hash>`.
- Nothing is saved. A request counts only while the plugin that made it runs and is on: switched off, crashed or unloaded, the widgets are back at once; at the next start nothing is hidden. Calls of several plugins add up; the same call twice is one request.
- A hidden widget is not drawn, not listed in layout mode and not used for snapping; its tab says "Hidden while <module> runs." The player's own settings (shown, place, size) stay as they are.
- Game thread only (`HORIZON_ERR_WRONG_STATE` otherwise). `HORIZON_ERR_INVALID_ARG`: no `plugin_id`, or the plugin's own id.

`offsetof(HorizonHostApi, suspend_widgets)` equals the size of the 1.4 table (unit test `static_assert`). A plugin checks `host->abi_minor >= 5` (or `host->size`) first.

The panel flag `HORIZON_PANEL_NO_BACKGROUND` (see "Panels") came with the same minor.

## Loading while the game runs

A module that was off at game start is loaded the moment the player switches it on (menu switch, free window "On" box): `LoadLibrary`, `HorizonPlugin_Info`, `HorizonPlugin_Init`, then its page and panels appear. For plugins this means:

- `Init` can run in the middle of a game: the device exists, a map may be running. `set_device_callbacks` calls `device_created` at once with the existing device (`present_params` is NULL then); `before_reset` / `after_reset` follow as usual. `adjust_present_params` only takes effect at the next `Reset`.
- Right after `Init` the host sends the plugin a `GAME_STATE` event with the current state and, if a map runs, a `MAP_LOADED` event (only to that plugin, only if it subscribed).
- Switching off: a plugin with `HORIZON_FLAG_LIVE_TOGGLE` gets `SetEnabled(0)` as before. The DLL stays loaded until the game ends (threads and hooks of a plugin may still point into it); switching on again is `SetEnabled(1)`, no second `Init`. A running plugin without `LIVE_TOGGLE` still turns off only after a restart.
- A module that cannot work this way sets `"startup_only": true` in its `plugin.json`; the switch's tooltip then says "Needs a restart: this module hooks the game at start".

### Opening the plugin's page: `open_page`

`open_page(plugin, panel_key)` opens the Horizon menu on the plugin's page, for an "open my page" hotkey. `panel_key` `NULL` or `""` opens the Settings tab (or the plain page), otherwise that panel's tab; a key the plugin has no panel for returns `HORIZON_ERR_INVALID_ARG` and the page still opens on Settings. Called while the menu already shows this page, it closes the menu (toggle, like the menu key). Game thread only (`HORIZON_ERR_WRONG_STATE` outside it), and `HORIZON_ERR_WRONG_STATE` while a capture (`HorizonHost_SetCaptureActive`) is active. TMVS registers a hotkey `menu` (default F8) that calls it. Background: `docs/spec-open-page.md`.

### Toasts: `notify`

A toast is a short note in the corner of the screen ("Map saved"). The host draws it in the TM Horizon look.

```c
HorizonToast t = {0};
t.size = sizeof(t);
t.kind = HORIZON_TOAST_OK;
t.title = "Map saved";            /* required, one short line */
t.text = "A01-Race";              /* optional second line, NULL or "" for none */
host->notify(host->plugin, &t);
```

| Field | Meaning |
|---|---|
| `size` | `sizeof(HorizonToast)`. The host ignores the call if the struct is shorter than the one it knows up to `flags`. |
| `kind` | `HORIZON_TOAST_INFO` (0), `_OK` (1), `_WARN` (2), `_ERROR` (3). Anything else counts as info. Sets the colour and the default icon. |
| `title` | Required. An empty or NULL title drops the toast. Cut at 120 characters. |
| `text` | Optional. Cut at 240 characters. |
| `icon` | An `HZ_ICON_*` codepoint, `0` = the icon of the kind. |
| `duration_ms` | `0` = 5000 ms, at most 30000 ms. Fade in and out (250 ms each) come on top. |
| `flags` | `HORIZON_TOAST_IMPORTANT` (0x01): shown even with "Do not disturb" and while driving. Use it rarely. |

Rules the host applies:

- The call is copied, can come from any thread and returns at once. Toasts of a plugin that is switched off, failed, or gated online are dropped.
- At most 3 toasts are on screen. Up to 24 wait; when more come, the oldest waiting one is dropped (it goes to the list as missed).
- "Do not disturb" (General page): no toast is shown except important ones. The others only go to the "Recent notifications" list, marked as missed.
- "Quiet while driving" (General page, on by default): while a run is going (solo or network race) toasts that are not important wait until the run ends.
- While a photo or video is being taken (`HORIZON_HOST_FLAG_CAPTURE`, `HorizonHost_SetCaptureActive`) nothing is drawn and the timers stand still.
- The last 20 toasts (shown or missed) are listed on the General page under "Recent notifications", with the plugin's name.
- Every toast is also written to the host log (`toast: title - text`).
- The notifications from TM Horizon (upload finished, mod updates, server events) use the same queue under the name "TM Horizon". See `docs/client-bridge.md`.

### The TM Horizon bridge: `bridge_request` and `bridge_state`

The host itself never goes online. A plugin that needs data from the internet (a TMX search, a map file, a record replay) can ask TM Horizon through the host (WR Hunter does); a plugin may also talk to a site itself (TMX Browser since 0.2.0 calls the TMX API with its own HTTP client, so it works without TM Horizon). The protocol, the paths and the pairing are in `docs/client-bridge.md`; this section is only the plugin side.

```c
static void HORIZON_CALL onAnswer(void* user, int32_t status, const char* content_type,
                                  const void* body, uint32_t body_len) {
    if (status == 200) {
        /* body is valid only during this call: copy what you keep */
    } else if (status == HORIZON_BRIDGE_ERR_NOT_CONNECTED) {
        /* TM Horizon is not running or not paired */
    }
}

if (host->abi_minor >= 3 && host->bridge_state() == HORIZON_BRIDGE_CONNECTED) {
    HorizonResult r = host->bridge_request(host->plugin, "GET",
                          "/v1/tmx/search?game=tmnf&name=A01", NULL, 0, onAnswer, NULL);
    if (r != HORIZON_OK) { /* HORIZON_ERR_BUSY, _NOT_SUPPORTED, _INVALID_ARG */ }
}
```

`bridge_request(plugin, method, path, body, body_len, callback, user)`:

- `method` is `"GET"` or `"POST"`. `path` starts with `/v1/`, uses printable ASCII without spaces, at most 1 KB, and must not start with `/v1/client/` (those paths belong to the host: ping, pairing, events). `body` is for POST (JSON, at most 1 MB), else `NULL` and `0`.
- Return value: `HORIZON_OK` when the request is queued (or answered at once with a rate-limit error through the callback), `HORIZON_ERR_INVALID_ARG` (bad method, path or callback, or a body that is NULL with a length), `HORIZON_ERR_BUSY` (this plugin already has 8 open requests), `HORIZON_ERR_NOT_SUPPORTED` (the bridge is switched off in the host), `HORIZON_ERR_WRONG_STATE` (not called on the game thread).
- The callback always comes later, on the game thread at the end of a frame, never inside `bridge_request`. Errors come the same way.
- `status` is the launcher's HTTP status (200, 404, 429 ...), or one of these:

| `status` | Constant | Meaning |
|---|---|---|
| -1 | `HORIZON_BRIDGE_ERR_NOT_CONNECTED` | not paired, or TM Horizon is not running |
| -2 | `HORIZON_BRIDGE_ERR_TIMEOUT` | no answer within 45 s |
| -3 | `HORIZON_BRIDGE_ERR_RATE_LIMIT` | this plugin sent too many requests, try again later |
| -4 | `HORIZON_BRIDGE_ERR_TOO_LARGE` | the answer is larger than the host takes (16 MB) |
| -5 | `HORIZON_BRIDGE_ERR_CANCELLED` | the plugin was switched off (or gated online) before the answer came |

- `content_type` is the answer's `Content-Type` without parameters (`application/json`, `image/jpeg` ...), `""` for the negative codes. `body` and `body_len` are valid only during the call; for the negative codes they are `NULL` and `0`.
- A 401 from the launcher makes the host forget its key and pair again; the plugin sees the 401.
- A callback that crashes switches the plugin off (like any plugin callback).

`bridge_state()` returns `HORIZON_BRIDGE_OFF` (0, switched off in the host's General page, "TM Horizon connection"), `_SEARCHING` (1, TM Horizon not found yet), `_PAIRING` (2, waiting for Allow in TM Horizon), `_CONNECTED` (3) or `_DENIED` (4, the player pressed No; the host asks again after a while). It is a cheap read; call it every frame if needed. Requests sent in any other state than connected fail with `HORIZON_BRIDGE_ERR_NOT_CONNECTED`.

Limits:

| Limit | Value |
|---|---|
| open requests per plugin | 8 (`HORIZON_ERR_BUSY` above that) |
| request rate per plugin | 4 per second, burst 8 (above that the callback gets `HORIZON_BRIDGE_ERR_RATE_LIMIT`) |
| request body | 1 MB |
| answer | 16 MB |
| time per request | 45 s |

TM Horizon applies its own limits on top (one TMX request per second at most, caches), see `docs/client-bridge.md`.

Threading: call `bridge_request` on the game thread (`Init`, `OnEvent`, a panel's draw function, `DrawOverlay`). The host sends the request on its own thread and hands the answer back on the game thread, so the callback can touch plugin state without a lock. When a plugin is switched off, its waiting requests are dropped and the ones already answered get `HORIZON_BRIDGE_ERR_CANCELLED`. After `Shutdown` no callback arrives. If the callback's `user` pointer points to something, make sure it is still there when the answer comes (a request id looked up in a table is safer than a raw pointer).

The plugin that uses this: WR Hunter (record replay, `docs/plugins/wr-hunter.md`). TMX Browser used it up to 0.1.0.

## For mods that are not plugins (TMVS and others)

`host.dll` (TMLoader: `HorizonHost.dll`) exports three plain C functions (`__cdecl`, stable, no ABI
struct needed). Find the module with `GetModuleHandleW(L"host.dll")` (or `L"HorizonHost.dll"`) and
`GetProcAddress`; if it is not there, Horizon is not running.

| Export | What it does |
|---|---|
| `uint32_t HorizonHost_GetUiState(void)` | What of Horizon is on screen: `1` menu open, `2` bar visible, `4` a detached panel window visible (OR-ed). `0`: nothing (also while a capture is active). |
| `void HorizonHost_SetCaptureActive(int active)` | `active != 0`: the host draws nothing (menu, bar, panels, plugin overlays) and takes no keyboard or mouse input until it is called with `0`. Last call wins (no counting per caller). Heartbeat: 10 s after the last call the capture ends by itself, so a crashed capture cannot hide Horizon for good. Only captures that read the image at Present need it: they call it with 1 every frame while they run and once with 0 afterwards. TMVS copies photos and Clips recordings at the game's frame end (before Horizon draws), so Horizon stays usable while they run; it calls this only for its fallback captures that read the image at Present (e.g. an F12 screenshot without the frame-end copy). Plugins' hotkeys keep working. |
| `uint32_t HorizonHost_Version(void)` | `(major << 16) \| (minor << 8) \| patch`, e.g. `0x000200` for 0.2.0. |

Plugins use `set_host_flag(HORIZON_HOST_FLAG_CAPTURE)` instead (ABI minor 2), which hides only the bar and the
windows with "Hide in photos and videos".

## Standalone builds

Rule: every Horizon module that makes sense on its own must also work without Horizon, as DLLs in a GitHub release. A player who does not want the host can still use the mod. A standalone build has no menu and logs to a file next to the exe.

| Module | Standalone build |
|---|---|
| `eswc-autosave` (Autosave & Login-Fix, module 2.1.0: ESWC, Sunrise and Original) | own repo `cheatoskar/eswc-autosave` (not built here): `dinput8.dll` loader + `eswc_autosave.dll` (1.2: ESWC, Sunrise and Original) |
| `controller-hotplug` (Controller fix) | built here: `dist\standalone\controller-hotplug\`, zip `standalone\Controller-Hotplug-<version>-standalone.zip` made by `tools\package.py` (loader `dinput8.dll` + `controller_hotplug.dll`, own install script) |

`tools\package.py` zips every folder `dist\standalone\<id>\` into `dist\release\standalone\<Name>-<version>-standalone.zip` (version from the module's `plugin.json`) and lists it under `standalone` in `manifest.json`. The module's build script fills the folder from `plugins\<id>\standalone\`.

How they live together with the Legacy Loader:

- Both use `dinput8.dll`. The Horizon install renames a foreign `dinput8.dll` to `dinput8.chain.dll`, and the Legacy Loader loads it first. So the standalone starts before the host.
- The standalone loader chains the same way: if `dinput8.chain.dll` lies next to it and is not the loader itself, it passes DirectInput on to that file, else to the system `dinput8.dll`. Its install script handles all three cases (nothing there, Horizon there, another mod there) and never overwrites or deletes a foreign `dinput8.dll`.
- The module checks whether the standalone is there (its DLL is loaded; for `eswc-autosave` also `eswc_autosave.dll` together with `dinput8.chain.dll` in the folder). If so it stays passive: no thread, no hook, no game call. It shows the toast "Eigenständiges … gefunden – Modul pausiert" (Controller fix: "Controller fix paused: standalone active") and the same text on its page. It never touches the standalone's files.
- Same code, one copy running: with both installed, only the standalone does the work.

## ABI versioning

- Every struct starts with its own size (or the ABI number). The host's `HorizonHostApi.size` and `abi_minor` say how much of the table exists.
- Changes are additive only: new fields go at the end, new events get new numbers, nothing is reordered or removed.
- `HORIZON_ABI_MINOR` is 5 today: 1 = textures and `save_replay`, 2 = panels and host flags, 3 = `open_page`, toasts (`notify`), the TM Horizon bridge (`bridge_request`, `bridge_state`) and `HorizonDeviceCallbacks.post_present`, see "ABI 1.3" above, 4 = `ui_metrics`, see "ABI 1.4", 5 = `suspend_widgets` and `HORIZON_PANEL_NO_BACKGROUND`, see "ABI 1.5". It counts additive changes. A plugin built against a newer minor still loads in an older host. Before using a function added later I check `host->size` (or `abi_minor`) and fall back if it is missing.
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
build\test\fake_host.exe <plugin dll> [--game eswc|sunrise|original|tmnf] [--expect-init <n>]
```

The fake host stubs the whole `HorizonHostApi`, keeps settings in memory, uses `build\test\data\<id>\` as data folder, runs a headless ImGui frame, sends a short event sequence, presses the hotkeys, toggles `SetEnabled`, calls `Shutdown` and unloads the DLL. `engine_pointer` returns NULL, so a plugin with game addresses must log and skip instead of calling game code.
