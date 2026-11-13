# Shared setup (0.3.1)

One Horizon setup for every game install of a Windows user: menu size, text size, keys, widget places,
module switches and module settings are made once and every game with **Shared setup** on uses them.

## Where it lives

| What | Where |
| --- | --- |
| The shared setup | `%APPDATA%\Horizon Client\shared\host.json` and `shared\<module id>.json` |
| Exported setups | `%APPDATA%\Horizon Client\setups\setup-<yyyy-mm-dd_hh-mm-ss>.json` |
| A game's own setup | `horizon\settings\host.json` and `horizon\settings\<module id>.json` (TMLoader: `products\Horizon Host\settings\`) |

The TMLoader product folder has a space in its name (`Horizon Host`); every path is handled as a whole, nothing
is split at spaces. Tests set `HORIZON_SETUP_DIR` to a temp folder; the test program never touches `%APPDATA%`
without it.

## The switch

General > **Shared setup** (On/Off). The value is `setup.shared` in the game's own `settings\host.json`.

- A new install (no `settings\host.json` yet) starts with it **on**.
- An install that already has a `settings\host.json` keeps its own setup (**off**) until the player switches.
- Switching applies after a restart of the game ("Restart to apply" under the switch).
- The first game that switches on while no shared setup exists yet copies its own setup into it, so nothing is
  lost.
- Switching off brings the game's own setup back: the game's `settings\host.json` keeps its own values the whole
  time; with the shared setup on they are only not read.

## What stays per game

With the shared setup on, these keys are still read from and written to the game's own `settings\host.json`:

| Key | Why |
| --- | --- |
| `bridge.*` | The pairing with TM Horizon (key, instance) belongs to this install |
| `setup.*` | The switch itself |
| `OnlineFix` | Login fix of ESWC and Original only |
| `pwin.<id>` | Free module windows (pixels) |
| `plugin.<id>.*`, `panel.<id>.*`, `hotkey.<id>.*` of a game-specific module | A module whose `plugin.json` lists fewer than all five games (Autosave & Login-Fix, Controller fix, Node Explorer) |

The settings file of a game-specific module also stays in `settings\<id>.json`. Everything else is shared: the
display settings (`ui.*`), widget background (`widgets.*`), notifications (`toasts.*`), the top bar (`bar`),
the Horizon keys, and the switches, widget places, keys and settings files of modules made for every game
(Horizon Scripts and its scripts' settings, WR Hunter, Input Display, TMX Browser, TMX Upload, Performance,
Legacy Randomizer). Module data (`horizon\data\<id>\`, for example WR Hunter's times) and the scripts folder
stay with the game.

Two games running at the same time both write the shared files. A save reads the file again and writes only the
keys this game changed on top of it, so neither loses the other's change.

## Widget places

A widget's place is saved with an anchor (since 0.3.1): the nearest screen edge per axis (left, centre, right;
top, middle, bottom) and the gap to it as a share of the window size. The entry
`panel.<plugin>.<key>` keeps the 0.3.0 fields and appends four:

```
detached,keepOpen,locked,clickThrough,hideInCapture,opacity,x,y,w,h,minimised,scale,ax,ay,ox,oy
```

`ax`/`ay`: 0 left/top, 1 centre, 2 right/bottom; `ox`/`oy`: gap divided by the window width/height (centre:
offset of the widget's centre from the window's centre). A widget at the top right stays at the top right at
1280x960, 1920x1080 or 800x600, in every game. A widget that fits its content keeps its anchored edge when its
content gets wider. Entries from 0.3.0 (pixels only) are used as they are and get their anchor the next time
the widget is moved, or when the setup is copied or exported. Older hosts read the first twelve fields and ignore
the rest.

Only what the player does moves a widget for good: dragging it with the menu open, layout mode, or its first
placement in its slot. When the game window gets smaller (a Reset, a loading screen, Alt+Tab) the widget is kept on
screen, but its saved place stays, and it goes back there when the window is large again.

## Buttons

General > Shared setup:

| Button | What it does |
| --- | --- |
| Copy from this game | This game's own setup replaces the shared one. |
| Reset | The shared setup goes back to the defaults (asks first). |
| Export | Saves the setup in use to `setups\setup-<time>.json` (a toast names the file). |
| Import | Lists the files in `setups\`; the chosen one is applied to the setup in use. "Open folder" opens `setups\` (copy a file in there to import it). |

Copy, Reset and Import apply fully after a restart (modules read their settings at start).

## Export file

```json
{
  "format": "horizon-setup",
  "version": 1,
  "host_version": "0.3.1",
  "game": "TrackMania Nations ESWC",
  "host": { "ui.content_scale": 110, "panel.scripts.w-clock-clock-d7a888": "1,1,0,0,1,100,1110,18,0,0,0,1.00,2,0,0.01406,0.01875" },
  "plugins": { "scripts": { "s.clock.hours": "0" } }
}
```

The TM Horizon pairing (`bridge.*`) and the switch (`setup.*`) are never exported or imported. Import writes each
host key where it belongs (shared or this game's own, as in the table above), replaces the settings of every
module in the file that is installed in this game, and leaves modules not in the file alone. A file from a newer
format is refused.

Code: `src/common/setup.h` (key classes, copy, export/import, anchors; unit test `testSharedSetup`),
`src/host/hostcfg.cpp` (the stores and the buttons), `src/host/panels.cpp` (anchors). Host test:
`tests\run_tests.cmd`, "shared setup".
