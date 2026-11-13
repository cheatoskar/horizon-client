# profile.json

TM Horizon writes `profile.json` before each start of a game profile. The host reads it once at start. Without the launcher nothing changes: **a missing profile.json is not an error**, the host then loads every plugin it finds (see "Without profile.json").

## Where

The file lies next to `host.dll`:

| Game | Path |
|---|---|
| ESWC, Sunrise, Forever without TMLoader | `<game folder>\horizon\profile.json` |
| Forever with TMLoader | `%LOCALAPPDATA%\TMLoader\database\TmForever\products\Horizon Host\<version>\profile.json` |

Encoding UTF-8, with or without BOM.

## Format 1

```json
{
  "format": 1,
  "profile": "My ESWC",
  "game": "eswc",
  "safe_mode": false,
  "menu_key": "Insert",
  "bar": true,
  "plugins": [
    { "id": "eswc-autosave", "version": "2.0.0", "enabled": true },
    { "id": "legacy-randomizer", "enabled": false }
  ],
  "written_by": "TM Horizon 0.3.0",
  "written_at": "2026-11-11T19:00:00Z"
}
```

| Key | Type | Required | Meaning |
|---|---|---|---|
| `format` | number | yes | Must be `1`. Any other value: the host loads **no** plugins and shows the error in the menu (About). |
| `profile` | string | no | Profile name shown in the menu. |
| `game` | string | no | `eswc`, `sunrise`, `original`, `tmnf`, `tmuf`. Informational only (the host identifies the exe itself by SHA-256). |
| `safe_mode` | bool | no (false) | `true` = "Start without mods": the host runs, no plugin is loaded. Same as holding Shift at start. |
| `menu_key` | string | no ("Insert") | Default key of the Horizon menu, e.g. `"Insert"`, `"Ctrl+Tab"`, `"F2"`. A key rebound in the menu's Hotkeys page wins over this value (stored in `settings\host.json`). |
| `bar` | bool | no (true) | `false` hides the Horizon bar at the top screen edge. |
| `stats_dir` | string | no | A folder TM Horizon reads for its statistics page. The host appends one JSON line per map load, start, respawn and finish to `<stats_dir>\<yyyy-mm>.jsonl` (see below). Missing: nothing is written. Safe mode writes nothing. |
| `plugins` | array | no ([]) | The plugins this profile enables, see below. Missing or empty array: no plugin is loaded. |
| `written_by`, `written_at` | string | no | For the log. |

Unknown keys are ignored (the launcher can add fields without breaking older hosts).

### Play events (`stats_dir`)

One line per event, UTF-8, `\n` at the end, file opened for appending (a half-written last line after a crash is skipped by TM Horizon). `v` is the format (1), `t` Unix time in ms, `uid` the map uid.

| `ev` | When | Other fields |
|---|---|---|
| `map` | MAP_LOADED | `name`, `author` and `env` when known |
| `start` | RACE_START (each start and restart) | |
| `respawn` | RESPAWN (ESWC/Sunrise; Forever has no respawn event) | `respawns`: count in this run |
| `finish` | FINISH | `time_ms`, `respawns`, `local`, `pb`, `mode` (`solo`, `net`, `unknown`) |

```
{"v":1,"t":1794326446230,"ev":"finish","uid":"7GK45NEyMOif7S9jicImvQNz7gf","time_ms":45230,"respawns":0,"local":true,"pb":true,"mode":"solo"}
```

Replays and the editor's test drives write nothing. The uid comes from the map file when the host started the map; for a map picked in the game's menus it is read from the engine's id table (plain reads, no game code is called). If neither works the line has no `uid` and TM Horizon skips it. The writes run on a worker thread; the host never reads, rewrites or deletes these files.

### `plugins` entries

Either an object or the plugin id as a plain string (`"eswc-autosave"` = `{"id": "eswc-autosave"}`).

| Key | Type | Required | Meaning |
|---|---|---|---|
| `id` | string | yes | Folder name under `horizon\plugins\` and the plugin's own id: `[a-z0-9._-]`, 1..64 characters, no `..`. Invalid ids are skipped with a warning in the log. |
| `version` | string | no | The version the launcher installed. If the installed plugin reports another version, the host loads it anyway and logs a warning. |
| `enabled` | bool | no (true) | `false` = installed but off in this profile. |

A plugin is loaded when **all** of these hold:

1. its folder `horizon\plugins\<id>\` exists with `<id>.dll` (or the `dll` named in its `plugin.json`),
2. profile.json lists it with `enabled` not false (or there is no profile.json),
3. it was not switched off in the in-game menu (`settings\host.json`, key `plugin.<id>.enabled`),
4. its `games` (plugin.json and `HorizonPlugin_Info`) include the running game,
5. no safe mode (Shift held, Ctrl+Shift, or `safe_mode: true`).

Duplicate ids: the first entry counts. Ids listed but not installed: a warning in the log.

## Without profile.json

Every folder in `horizon\plugins\` with a valid id is a plugin candidate and loads, unless

- its `plugin.json` says `"enabled_by_default": false` (then it is off until switched on in the menu), or
- it was switched off in the menu (stored in `settings\host.json`).

Without a profile only the quiet modules start: the menu itself, Horizon Scripts (clock, map info), Autosave & Login-Fix and Controller hot-plug. WR Hunter, Input Display, Node Explorer, TMX Browser, TMX Upload and Horizon Performance ship with `"enabled_by_default": false`. A switch the player already set in the menu (`plugin.<id>.enabled` in `settings\host.json`) stays as it is.

Switching a module on in the menu loads it right away (DLL, `Init`, the game's Direct3D device handed over through `set_device_callbacks`, its settings page appears). Only a module whose `plugin.json` says `"startup_only": true` (it must hook the game at start) still needs a restart; the menu says so on its switch.

`eswc-autosave` ("Autosave & Login-Fix") is on by default and checks for the standalone ESWC Autosave itself (`eswc_autosave.dll` loaded, or `eswc_autosave.dll` and `dinput8.chain.dll` in the game folder). If it is there, the module loads but stays passive (no hook, no event, no hotkey) and shows "Eigenständiges ESWC Autosave gefunden – Modul pausiert" as a toast and on its page; the standalone files are never touched. The ESWC login fix (error 12157) is part of the host, not of the plugin: switch `OnlineFix` in settings\host.json (menu: General, "Login-Fix (Fehler 12157)", default on).

`controller-hotplug` ("Controller fix") is on by default as well. It pauses itself the same way when the standalone `controller_hotplug.dll` is loaded in the game (toast "Controller fix paused: standalone active").

The switches in the menu persist in `settings\host.json` and work the same with or without a profile.

## What the host writes (never profile.json)

| Path (legacy games: under `<game>\horizon\`; TMLoader: the product folder `...\products\Horizon Host\`) | Content |
|---|---|
| `logs\<yyyy-mm-dd_hh-mm-ss>.log` | one log per start, the last 20 are kept |
| `settings\host.json` | menu switches (`plugin.<id>.enabled`), hotkeys (`hotkey.<owner>.<action>`), `bar`, `OnlineFix`, menu size `ui.menu_size` (0 = auto, else 60/70/80/90 % of the screen), content scale `ui.content_scale` (0 = auto, else 80-125 %), widget layout `panel.<plugin>.<key>`, free windows `pwin.<plugin>` |
| `settings\<plugin id>.json` | each plugin's own settings |
| `data\<plugin id>\` | each plugin's data folder |

The launcher may read `settings\host.json` to show the in-game switches, but must not rely on it.
