# WR Hunter

Plugin id `wr-hunter`. ESWC, Sunrise, TMNF and TMUF, online-ok, live toggle, off by default (switch it on in the menu; it loads right away). A LiveSplit-style split display: at every checkpoint it shows your time against your personal best and against a target, with gold sectors, a sum of best sectors and a history per map.

## What it does

- Shows the split at each checkpoint and the delta to your PB and to the chosen target. Colours as in TrackMania: blue = faster, red = slower (lighter when the gap shrinks or grows back, like LiveSplit), gold when you beat your best time for that sector. The compact view has a line with the latest checkpoint deltas, each in its colour (against the target if there is one, else the PB). Lines have a fixed width and long map names are cut, so the widget no longer grows across the screen at the finish.
- Keeps per map: personal best (with its splits), gold sectors (the best time of each sector over all your finished runs), sum of best, runs, resets, finishes, and the last 100 finished runs.
- A run with another number of splits than your PB (another route, laps) goes into the history only. It does not change PB or golds.
- After a new personal best (not the first finish) it can show a toast, "New personal best" with the time and the difference.
- A reset is counted when a new run starts while the previous one was already running (more than 0.5 s or with a checkpoint).

## Panels and settings

Panels (see `plugin-abi.md`, Panels):

- **Splits**: a HUD window, starts popped out and kept open. Compact or detailed.
- **History**: a tab on the plugin's page. The current map (PB, sum of best, runs, resets, finishes, gold sectors, the run list with time, "vs PB", respawns and date) and an "All maps" table (PB, runs, finishes).
- **Settings** (the plugin's own page, before the tabs):

| Label | Key | Default | What it does |
|---|---|---|---|
| Layout: Compact / Detailed | `Layout` (0/1) | Compact | One-line delta or a table of all splits with PB, target and sector columns |
| Sum of best | `ShowSob` | on | Shows the sum of your gold sectors |
| Runs and resets | `ShowResets` | on | Shows "Runs / Resets / Finishes" |
| PB note | `PbToast` | on | A short note in the corner after a new personal best |
| Compare with: Personal best only / TMX record / Ghost in the race / Replay file | `Target` (0-3) | TMX record | What the second delta is measured against |
| Reload | | | TMX record only: deletes the cached record replay and loads it again |
| Find replays | `TargetFile` | | Replay file only: lists your replays of this map (time and driver), click one to use it |
| Reset map stats, Delete / Keep | | | Deletes PB, golds and history of the loaded map after a second click |

"Ghost in the race" is only listed on TMNF and TMUF.

## Where the times come from

| Game | Splits and finish | Map UID |
|---|---|---|
| ESWC, Sunrise | the host's `CHECKPOINT`, `FINISH`, `RESPAWN` and `RACE_START` events | the event's UID when the map was started by the host; otherwise read from the engine's name table (see below) |
| TMNF, TMUF | read once a frame from the local player's checkpoint list (race state, race time, `CurCheckpoint`, the game's own list of checkpoint times); the `FINISH` event ends the run | same read, first guess at offset 0xdc of the challenge |

Members are looked up by class and member name through the host's reflection (`CTrackManiaRace`, `CGamePlayer`, `CTrackManiaPlayerInfo`, and on ESWC/Sunrise `CConstructionRace`, `CConstructionPlayer`). A member that has no reflection name uses a fixed default offset (the checkpoint list on Forever is one of them) and every value is checked for plausibility before it is used. Where the list cannot be read, the splits are taken from the checkpoint counter when it moves.

The map UID is found without calling game code: the plugin finds `CMwId::GetString` in the exe by its byte pattern, takes the address of the engine's id name table from its instructions and reads the three ids (UID, environment, author) of the challenge by plain reads. If that fails, the map is kept under a local key (a hash of name and author time) and the settings page says "kept by name (UID unknown)". Without a UID there is no TMX record and replay files cannot be matched.

## What it never does

- No writes to game memory, no game function calls from the plugin, no hooks, no code patches, no change to your input. On Forever this is also the Competition Patch rule: nothing in the game's code is touched.
- No checkpoint positions, no line, nothing drawn in the world. Split times only. Allowed online.
- It does not go online itself. The TMX record comes through TM Horizon.

## The TMX record

With "TMX record" selected the plugin asks TM Horizon (`/v1/tmx/record` with the map UID, then `/v1/tmx/replay`), reads the checkpoint times out of the record replay and uses them as the target. TM Horizon must be running on the same PC and paired, see `docs/client-bridge.md`. Messages shown instead of a target: "Start TM Horizon for the TMX record.", "This map has no record on TMX.", "Map UID unknown: no TMX record.", "Record replay not available.", "This Horizon Host has no TM Horizon bridge." (host older than ABI 1.3), "TM Horizon connection is off.". Without TM Horizon the plugin still works against your PB.

## Where data is stored

In the plugin's data folder `horizon\data\wr-hunter\`:

- `maps\<uid>.json`: stats of one map (PB, golds, runs). Maps without a UID use `maps\local-<hash>.json`.
- `records\<uid>.Replay.Gbx`: the downloaded TMX record replay, used as a cache. "Reload" deletes it.

Settings are in `horizon\settings\wr-hunter.json`. The replay picker reads your replay folder (`Tracks\Replays` in the user folder on Forever, `GameData\Tracks\Replays` in the game folder otherwise) with a worker thread, at most 4000 files.

## Known limits

- The reading of the local player's checkpoint list on TMNF/TMUF is written from the reflection tables and tested against fake game memory only. I have not confirmed it in the real game yet.
- The ESWC and Sunrise map UID offset (the three ids in the challenge) was found by scanning, not from a reflection member, and is not confirmed in the game yet. If it is wrong, maps are kept by name.
- "Ghost in the race" reads the ghost list of the race with fixed offsets on TMNF/TMUF; also not confirmed in the game.
- A finish without a start the plugin saw (switched on during a run) is counted as a run but not as a reset.
