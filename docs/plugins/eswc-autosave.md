# Autosave & Login-Fix

Plugin id `eswc-autosave`, menu name "Autosave & Login-Fix", version 2.1.0. ESWC, Sunrise and Original, online-ok, live toggle, on by default. Saves your replay after a finish, the same file "Save replay" writes, without the dialogs.

## What it does

| Game | What gets saved |
|---|---|
| ESWC, unofficial 1.80 (0.1.7.9) | solo: the race you just finished; LAN/online: your previous run ("Save previous replay"), plus the TMX copy with only your ghost |
| Sunrise, Win10 community repack | solo only: the race you just finished |
| Original, Online Patch v2 exe | solo only: the race you just finished |

With "New best only" (the default) a replay is saved only when the run is a new personal best on the map since the game started. Files go to `GameData\Tracks\Replays\My Replays\` as `<map>(mm'ss''cc).Replay.Gbx` (stunts: `<map>(score).Replay.Gbx`), the name the game suggests itself. An existing file is never overwritten (`… (2).Replay.Gbx`). The host and the other plugins get a "replay saved" event with the path.

The module calls the game's own save code (the calls `CTrackMania::SaveReplayRecord` makes after you press OK), so the replay and its `validable` flag are what the game would write. It never writes into the replay or the game's other objects.

On Sunrise and Original it reacts only to the host's FINISH of the local player in a solo race and the solo race end that follows (end-race screen). LAN, online and network races are never saved there; the save key does nothing online either.

## Settings

| Setting | Default | |
|---|---|---|
| Save: New best only / Every finish | New best only | |
| Refresh replay list | on | rescans the game's replay list after a save, as the game does |
| TMX copy | on | ESWC only |
| Save now (key F9) | | saves the last run now (Sunrise/Original: the finished solo run) |

## Login fix

The login fix (error 12157: TLS 1.0 and no revocation check, for the game process only) is part of the host, not of this module (host setting "Login-Fix"). It covers ESWC and, with the host's Original support, Original. Sunrise: none (not known to need one).

## Games and state

| Game | State |
|---|---|
| ESWC | as 2.0: tested in game |
| Sunrise | static: every address checked two ways (same calls in the game's save code as ESWC's verified code, plus reflection offsets, strings, vtable slots); fake host test passes; not run in game yet. Needs the host's Sunrise race hooks (FINISH, solo RACE_END) |
| Original | as Sunrise. Needs a host with Original support (branch `tmo`); with a host without it the module is never loaded on Original |

Addresses: `autosave_plugin.cpp` (`LegacyGame`) and trackmania-internals `games/tm-sunrise/replay-save.md`, `games/tm-original/addresses.md`.

## Tests

`plugins\eswc-autosave\tests\run_tests.cmd` (after a build) runs `legacy_fakehost.exe`: the module in a fake host on Sunrise and Original, with a fake game image that has the game's save functions as stubs at their addresses. The ESWC flows run in `tests\fake_host.cpp` (`tests\build_fake_host.cmd`).

## Standalone build

The same autosave exists without Horizon: eswc-autosave 1.2 (`dinput8.dll` + `eswc_autosave.dll` + `eswc_autosave.ini`), for ESWC, Sunrise and Original. If it is active in the game (`eswc_autosave.dll` loaded, or next to a `dinput8.chain.dll`), this module stays passive and shows "Eigenständiges ESWC Autosave gefunden – Modul pausiert".
