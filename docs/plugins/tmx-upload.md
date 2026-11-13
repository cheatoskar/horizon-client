# TMX Upload

Plugin id `tmx-upload`. All games, online-ok, live toggle, **off by default**. It offers the replays you drive to an uploader on the same PC: TM Horizon, or the browser extension of the 100% TMX mod. It does not talk to TMX itself and never sends anything on its own.

## What it does

1. Listens for `REPLAY_SAVED` (the game or another plugin such as ESWC Autosave wrote a replay file).
2. Queues the file if it passes the rules below (newest first, the last 20 are kept).
3. Serves the queue over a local HTTP bridge. The uploader pairs once (you press Allow in the game), takes the file and reports the result back. The page in the Horizon menu lists the last 10 results.

## The page

One status line, then the buttons and the results:

- **Uploader: waiting (port 27311)**: the bridge runs, no uploader has called yet. **Uploader: connected (TM Horizon)**: an uploader called this start (TM Horizon, the browser extension or another app). **Uploader: other mod (port N)**: another mod already runs the bridge on that port (checked again every 30 s). **Uploader: no free port**: 27311-27315 are all taken. **Uploader: off**: the module is switched off.
- **Upload last**, **Forget apps** (see below), **Queue: n** (replays waiting for an uploader), **Results** (OK / Failed, time, map, detail and link).

## Rules for a queued replay

- The file name ends in `.Replay.Gbx` and the file exists.
- The local player finished the map that is loaded now (a `FINISH` with `is_local_player` since the last `MAP_LOADED` or `RACE_START`). Replays saved while watching a replay are never queued.
- `verified` in the offer is true when the map UID of the loaded map is found in the first 64 KB of the file. The launcher skips unverified offers. When the host cannot read the UID (empty string), nothing is verified.
- Two files for one finish (ESWC Autosave writes the full multi-ghost replay and then `<name> TMX.Replay.Gbx` with only your ghost): the `TMX` copy wins, the other one is dropped from the queue.
- "Upload last" offers the newest queued replay again under a new id. If your latest finish is not in the queue yet it first asks the host to save the replay (`save_replay`); where the host returns "not supported" it offers the newest queued one instead.

## Pairing

`POST /v1/pair` shows "TM Horizon wants to upload replays. Allow / No" on the plugin page, and as a small window at the top of the screen while the menu is closed. The question stands 180 s after the last request (the launcher asks every 2 s, so it stays while the launcher waits). After No the same ask is answered `denied` for 60 s. After Allow, the next `/v1/pair` returns a key. Keys are kept in the plugin settings (`Tokens`, up to 8), so a restart does not ask again. "Forget apps" deletes them; the apps then get 401 and ask again.

## Protocol (same as `bridge.cpp` of the 100% TMX mod)

HTTP/1.1 on `127.0.0.1` only, the first free port of 27311-27315, `Connection: close`, no CORS headers. A request whose `Host` is not `127.0.0.1` or `localhost` gets 403 (DNS rebinding). Authenticated requests carry `X-TMX-Key`.

| Request | Answer |
|---|---|
| `GET /v1/ping` | `{"ok":true,"mod":"horizon-tmx-upload 0.1.0","bridge":"horizon-host"}` |
| `POST /v1/pair` | `{"ok":false,"status":"pending"}` / `{"ok":false,"status":"denied"}` / `{"ok":true,"key":"..."}` |
| `GET /v1/hello` | `{"ok":true,"mod":"...","game":"<site code>","linked":true}` |
| `GET /v1/next?wait=N` | long poll, N up to 55 s: `{"ok":true,"upload":{id,site,trackId,mapName,fileName,timeMs,verified}}` or `"upload":null`. Newest queued replay first; it is offered again until `/v1/result`. |
| `GET /v1/file?id=` | the replay bytes (`application/octet-stream`), 404 for an unknown id |
| `POST /v1/result` | body `{"id","ok","detail"}` (optional `link`): removes the job, 404 for an unknown id |

Site codes in `site` and `game`: `tmn` (ESWC), `tms` (Sunrise), `tmo` (Original), `tmnf`, `tmuf`.

Differences from the mod: `trackId` is always 0 (the plugin does not know the TMX id; TMX finds the map from the file), `linked` is always true, the answer to a refused pairing is `denied` instead of `pending` for 60 s, ids are `<n>-<random>`, and the key is one per paired app (not one for the install).

## Coexistence

When the 100% TMX mod (Forever) already runs its bridge, a second server would split the uploaders. Before binding, the plugin sends `GET /v1/ping` to 27311-27315. If something answers `{"ok":true,"mod":...}`, it stays passive ("Another mod runs the upload bridge") and checks again every 30 s. It starts its own server when the other one is gone. The server only runs while the plugin is switched on; switching off or leaving the game stops it and joins its threads within a fraction of a second.

## Threads

The server runs on its own thread (plus a short-lived thread per client). It shares state with the game thread under one mutex, never calls the host (log lines are queued, settings are written through `run_on_game_thread`).

## Not covered

- Forever: the host sends no game events there yet, so nothing is queued automatically; "Upload last" has nothing to offer.
- Sunrise: no finish events yet, same effect.
- A replay saved by hand after a finish in the same map is queued too (it is a replay of this map by you).
- Test hook `HorizonPlugin_TestHook` (used by `tests\fake_host.cpp`) answers the pairing prompt and reads the state; the game never calls it.
