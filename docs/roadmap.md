# Open points and roadmap

State of 0.1.0 (2026-11-11). Additive ABI changes only (`HORIZON_ABI_MINOR`), see `include/horizon_plugin.h`.

## Next ABI minors (planned, not built)

- **Panels** (`register_panel(plugin, const HorizonPanelDef*)`): `HorizonPanelDef { key (stable, for saving), title, icon, flags (MENU_PAGE | HUD_PANEL), draw(void* user), user, default rect }`. The host owns the tab list, pop-out into detached windows (TMVS's detach-windows model, `tm shaders` branch detach-windows, commit 34dc1f7), minimize, close back to the menu, "keep open when the menu closes", opacity, edit/locked layout, click-through, hidden during photo/video, persistence per plugin + key, layout presets. The Legacy Randomizer does its own panels until then.
- ~~**Shared icon font**~~ done: 53 Lucide icons (ISC licence) merged into every host font, codepoints in `horizon_imgui.h` (`HZ_ICON_*`, ABI), used in the sidebar and the bar. Plugins can't add fonts to the shared atlas themselves, so new icons go through `tools/make_icon_font.py`.
- `save_replay` (ABI 1.1 entry exists, returns NOT_SUPPORTED): wire to the ESWC save path (the Autosave plugin's code moves into the host's ESWC adapter).
- `unsubscribe`/hotkey query: not needed so far.

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
