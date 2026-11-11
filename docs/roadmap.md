# Open points and roadmap

State of 0.1.0 (2026-11-11). Additive ABI changes only (`HORIZON_ABI_MINOR`), see `include/horizon_plugin.h`.

## Next ABI minors (planned, not built)

- **Panels** (`register_panel(plugin, const HorizonPanelDef*)`): `HorizonPanelDef { key (stable, for saving), title, icon, flags (MENU_PAGE | HUD_PANEL), draw(void* user), user, default rect }`. The host owns the tab list, pop-out into detached windows (TMVS's detach-windows model, `tm shaders` branch detach-windows, commit 34dc1f7), minimize, close back to the menu, "keep open when the menu closes", opacity, edit/locked layout, click-through, hidden during photo/video, persistence per plugin + key, layout presets. The Legacy Randomizer does its own panels until then.
- **Shared icon font** in the host's atlas (Lucide, ISC licence) with the codepoints in `horizon_imgui.h`: plugins can't add fonts to the shared atlas themselves.
- `save_replay` (ABI 1.1 entry exists, returns NOT_SUPPORTED): wire to the ESWC save path (the Autosave plugin's code moves into the host's ESWC adapter).
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
