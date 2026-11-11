# Open points and roadmap

State of 0.1.0 (2026-11-11). Additive ABI changes only (`HORIZON_ABI_MINOR`), see `include/horizon_plugin.h`.

## Next ABI minors (planned, not built)

- ~~**Panels**~~ done in 0.2.0 (ABI 1.2, see plugin-abi.md "Panels"); still open from the plan: minimize, layout presets. Original plan: (`register_panel(plugin, const HorizonPanelDef*)`): `HorizonPanelDef { key (stable, for saving), title, icon, flags (MENU_PAGE | HUD_PANEL), draw(void* user), user, default rect }`. The host owns the tab list, pop-out into detached windows (TMVS's detach-windows model, `tm shaders` branch detach-windows, commit 34dc1f7), minimize, close back to the menu, "keep open when the menu closes", opacity, edit/locked layout, click-through, hidden during photo/video, persistence per plugin + key, layout presets. The Legacy Randomizer does its own panels until then.
- ~~**Toasts and the TM Horizon bridge**~~ done in 0.2.0 (ABI 1.3: `notify`, `bridge_request`, `bridge_state`, see plugin-abi.md "Notifications and the TM Horizon bridge" and client-bridge.md). First users: the client mods WR Hunter, Input Display and TMX Browser (docs/plugins/). Open: real-game test of the Forever car offsets and of the ESWC/Sunrise map UID offset; speed, gear and RPM on ESWC/Sunrise (engine getters only).
- ~~**Shared icon font**~~ done: 53 Lucide icons (ISC licence) merged into every host font, codepoints in `horizon_imgui.h` (`HZ_ICON_*`, ABI), used in the sidebar and the bar. Plugins can't add fonts to the shared atlas themselves, so new icons go through `tools/make_icon_font.py`.
- ~~`save_replay`~~ done on ESWC in 0.2.0 (the Autosave plugin's save code, now also in the host). Open: Sunrise (replay field of the app not known yet), Forever; ESWC Autosave could use `save_replay` instead of its own copy of the code.
- `unsubscribe`/hotkey query: not needed so far.

## Phase 2: scripts (Horizon Scripts plugin)

Built (branch `scripts`, plugin 0.1.0, not yet run in a game): AngelScript 2.38.0 as a plugin (`plugins/scripts`), no host changes. Scripts in `<game>\horizon\scripts\<id>\` with `info.toml`/`info.json`, hot reload, per-script errors with file:line in the Scripts tab, settings page from `[Setting]` globals, UI (ImGui subset), Time, Game (state, map, PlayMapFile/ReturnToMenu behind `game-control`), read-only Reflection over the Node Explorer's read layer, IO for the script's own data folder, Log. Time budget per callback, online gate, Competition Patch rule. Guide: `docs/scripting.md`.

Next, in this order:

- **Write access behind permissions**: reflection writes and parameterless method calls through the engine's own member accessors, a new permission (`write-game`), solo only, never with Competition Patch loaded, never on members that touch physics, timing, input or replays (allow list per class, not a block list).
- **Net**: HTTP GET/POST to an allow list declared in `info.toml` (TMX API first), off the game thread, results delivered in a callback; never while a CP run is validated.
- **Openplanet-compat layer** for simple UI plugins (`UI::`, `Render`/`RenderMenu`, `[Setting]`, `print`): only after asking Openplanet's author whether that is welcome, and without copying Openplanet code or docs.
- Smaller: `yield()`-style coroutines (Main() that waits for a frame), `UI::Begin` with a close button (needs unsafe references or a handle type), vec4 colour settings (`[Setting color]` is parsed but ignored today), `Game::Map` thumbnail, Sunrise/Forever events as the host gains them (scripts get them without changes).
- Before a release: run both examples in ESWC, Sunrise, TMNF and TMUF (TESTPLAN), a TMX-validated run with CP 2.0 and the plugin loaded, and ask donadigo how CP treats AngelScript in another DLL.

## Game adapters

- ESWC: map uid/author/path in MAP_LOADED only when gamectl started the map (gbx header). For maps started from the game's own menu they stay "" (no reflection field for the uid; the challenge's file is reachable through its fid, offsets not mapped yet).
- Sunrise: finish/checkpoint/respawn/race-end events need the verified OnFinishLine (vtable slot, prologue check) and the solo race end; the randomizer session puts them into tm-modding-knowledge games/tm-sunrise.
- Forever: events by polling only (no hook, because Competition Patch 2.0 may treat changed code bytes as tampering): MAP_LOADED (medal times from the challenge, uid/author/path only for maps gamectl started), RACE_START (also each restart), FINISH (time, PB), game state incl. END_RACE. Written statically, **not yet run in game**. Limits: no CHECKPOINT/RESPAWN/RACE_END, uid of maps picked in the game's own menu stays "" (an id index that only a game call resolves), editor/replay split by class name unverified. gamectl now moves its link addresses by (image base - 0x400000), so TMLoader 2.12.0 (relocated) is covered like 2.12.0-compat; play_map_file/return_to_menu on Forever are still untested in game. The same patch belongs into legacy-randomizer's gamectl/reflect (vendored copies diverge until then).
- Original: not supported (no pinned exe).

## Host

- TMVS together with the host: both draw at Present; the host's vtable hook runs before TMVS's inline Present hook, so TMVS's menu lands on top of the Horizon menu, and the Horizon menu could end up in TMVS clips/video export when both are open. Later: TMVS as a `_DEVICE` plugin (host owns the hook, calls TMVS's pre-present first).
- Our extra BeginScene/EndScene at Present is visible to other hookers (TMVS counts scenes).
- Font atlas is built once (scale from the monitor height); a later resolution change keeps the scale.
- The ESWC/Sunrise device must be created after the host starts (the game's first DirectInput8Create comes ~0.4 s before CreateDevice in ESWC; measured in Sunrise too). A late-attach path (device made before the host) is not built.
- Unload of plugins: never (DLLs stay until the process ends); Shutdown runs when the game window is destroyed.
