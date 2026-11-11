# Open points and roadmap

State of 0.1.0 (2026-11-11). Additive ABI changes only (`HORIZON_ABI_MINOR`), see `include/horizon_plugin.h`.

## Next ABI minors (planned, not built)

- ~~**Panels**~~ done in 0.2.0 (ABI 1.2, see plugin-abi.md "Panels"); still open from the plan: minimize, layout presets. Original plan: (`register_panel(plugin, const HorizonPanelDef*)`): `HorizonPanelDef { key (stable, for saving), title, icon, flags (MENU_PAGE | HUD_PANEL), draw(void* user), user, default rect }`. The host owns the tab list, pop-out into detached windows (TMVS's detach-windows model, `tm shaders` branch detach-windows, commit 34dc1f7), minimize, close back to the menu, "keep open when the menu closes", opacity, edit/locked layout, click-through, hidden during photo/video, persistence per plugin + key, layout presets. The Legacy Randomizer does its own panels until then.
- ~~**Shared icon font**~~ done: 53 Lucide icons (ISC licence) merged into every host font, codepoints in `horizon_imgui.h` (`HZ_ICON_*`, ABI), used in the sidebar and the bar. Plugins can't add fonts to the shared atlas themselves, so new icons go through `tools/make_icon_font.py`.
- ~~`save_replay`~~ done on ESWC in 0.2.0 (the Autosave plugin's save code, now also in the host). Open: Sunrise (replay field of the app not known yet), Forever; ESWC Autosave could use `save_replay` instead of its own copy of the code.
- `unsubscribe`/hotkey query: not needed so far.

## Game adapters

- ESWC: map uid/author/path in MAP_LOADED only when gamectl started the map (gbx header). For maps started from the game's own menu they stay "" (no reflection field for the uid; the challenge's file is reachable through its fid, offsets not mapped yet).
- Sunrise: finish/checkpoint/respawn/race-end events need the verified OnFinishLine (vtable slot, prologue check) and the solo race end; the randomizer session puts them into tm-modding-knowledge games/tm-sunrise.
- Forever: no game events yet (candidates in tm-modding-knowledge modding/game-control.md: OnFinishLine TMNF 0x480820 / TMUF 0x480610, Race1P 0x472300 / 0x472080, to be prologue-checked and kept read-only for Competition Patch). gamectl's Forever specs need image base 0x400000: under TMLoader 2.12.0 (relocated) state/online are unknown, so solo-only plugins stay paused there.
- Original: not supported (no pinned exe).

## Host

- TMVS together with the host: both draw at Present; the host's vtable hook runs before TMVS's inline Present hook, so TMVS's menu lands on top of the Horizon menu, and the Horizon menu could end up in TMVS clips/video export when both are open. Later: TMVS as a `_DEVICE` plugin (host owns the hook, calls TMVS's pre-present first).
- Our extra BeginScene/EndScene at Present is visible to other hookers (TMVS counts scenes).
- Font atlas is built once (scale from the monitor height); a later resolution change keeps the scale.
- The ESWC/Sunrise device must be created after the host starts (the game's first DirectInput8Create comes ~0.4 s before CreateDevice in ESWC; measured in Sunrise too). A late-attach path (device made before the host) is not built.
- Unload of plugins: never (DLLs stay until the process ends); Shutdown runs when the game window is destroyed.
