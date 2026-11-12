# modules.json (0.3.1)

A file next to `host.dll` (TMLoader: next to `HorizonHost.dll` in the version folder), shipped from
`packaging/modules.json` with its icons (`packaging/icons/`). It holds what is not a single module's business:
module groups and first-start defaults. `tools/package.py` copies it into every package and repeats `groups`
and `defaults` in `dist/manifest.json`, so TM Horizon can show the same list.

```json
{
  "format": 1,
  "groups": [
    {
      "id": "legacy-essentials",
      "name": "Legacy Essentials",
      "description": "Autosave, controller fix, login fix and game settings for ESWC, Sunrise and Original.",
      "icon": "icons/legacy-essentials.png",
      "members": [
        { "id": "eswc-autosave", "label": "Autosave" },
        { "id": "controller-hotplug", "label": "Controller fix" },
        { "host": "login-fix", "label": "Login fix" },
        { "id": "game-settings", "label": "Game settings" }
      ]
    }
  ],
  "defaults": { "legacy-randomizer": false },
  "previous_defaults": { "scripts": true, "tmx-browser": false, "tmx-upload": false, "legacy-randomizer": true }
}
```

## Groups

Several modules shown as one entry. The DLLs, their `plugin.json`, settings and hotkeys stay separate; only the
menu changes:

- One row in MODS with the group's icon and name, one switch: off switches every feature off, on switches every
  feature on. The row says "2 of 4 on" when only some are.
- The group's page: a switch per feature (the label from `members`), then the members' own pages as tabs.
- A member module is not listed alone. Opening a member's page (`open_page`, the bar) opens the group on its tab.
- `"host": "login-fix"` is the host's login fix (`OnlineFix` in host.json, ESWC and Original, applies on the next
  start). It is shown in the group instead of General.
- Only the features that apply to the running game are shown; a group without any (TMNF/TMUF for Legacy
  Essentials) is left out.
- A module belongs to one group; ids are checked like plugin ids; `icon` is a PNG path relative to the file,
  without `..`.

## First-start defaults

Without `profile.json` (TM Horizon writes one when it manages the modules) a module that was never switched is on
or off by its `plugin.json` `"enabled_by_default"`: `true` (or missing), `false`, or `"paired"` (on once this game
is paired with TM Horizon, used by TMX Upload). `defaults` in modules.json overrides that per module id, also for
modules that are not ours (the Legacy Randomizer).

`previous_defaults`: what 0.3.0 used for modules whose default changed. On the first 0.3.1 start of an install
that already had `settings\host.json`, these values are written as the player's switches (only where the player
never switched, never with a profile.json), so nothing changes for existing players. `setup.defaults` in
host.json marks that this ran.

| Module | New install | Why |
| --- | --- | --- |
| Legacy Essentials (Autosave, Controller fix, Login fix, Game settings) | on | the basics for the old games |
| TMX Browser | on | |
| TMX Upload | on once paired with TM Horizon | needs TM Horizon |
| Horizon Scripts | off | Clock and Map Info are also hidden until shown |
| Input Display, WR Hunter, Performance | off | |
| Legacy Randomizer | off | installed separately |
| Node Explorer | not shipped | developer tool (`package.py --dev`) |

Code: `src/common/modules.h` (parser, unit test `testModulesConfig`), `src/host/plugins.cpp` (defaults),
`src/host/overlay.cpp` + `src/host/ui/ui_menu.cpp` (group row and page), `src/host/textures.cpp` (group icons).
