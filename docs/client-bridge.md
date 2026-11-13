# TM Horizon client bridge

The host never goes online. When a plugin needs something from the internet (a TMX search, a
map file, a record replay), the Horizon Host asks the TM Horizon launcher on the same PC, and the
launcher asks TMX with its own HTTP client (user agent `TM-Hub/<version>`, its caches and limits). The
same connection carries notifications from the launcher to the game (upload finished, mod updates,
server events), shown as Horizon toasts.

```
plugin --bridge_request--> host (one connection, one key) --HTTP 127.0.0.1--> TM Horizon --HTTPS--> TMX
host  <--/v1/client/events (long poll)-- TM Horizon (upload done, mod update, server event)
```

Plugins never open sockets for this. They call `bridge_request` (ABI 1.3, `include/horizon_plugin.h`);
the host queues the request, sends it on its bridge thread and hands the answer back on the game thread.

## Transport

- HTTP/1.1 on `127.0.0.1` only, the first free port of **27331-27335** (TM Horizon binds it, the host
  probes them in order with `GET /v1/client/ping`). `Connection: close`, no CORS headers.
- A request whose `Host` header is not `127.0.0.1` or `localhost` gets 403 (DNS rebinding).
- Every path except `ping` and `pair` needs `X-Horizon-Key: <key>`; a wrong or missing key gets 401.
- JSON is UTF-8. Errors: `{"ok":false,"error":"<short text>"}` with a 4xx/5xx status.
- Limits: requests from one key are rate limited (token bucket, 8 per second, burst 24): 429 with
  `Retry-After` when exceeded. Calls that reach TMX are serialised in TM Horizon with at least one
  second between two TMX requests, and searches/thumbnails are cached. Answers are at most 16 MB.

## Pairing (like the 100% TMX mod bridge, roles swapped)

The side that holds the data asks the player. Here that is TM Horizon:

1. The host sends `POST /v1/client/pair` with
   `{"client":"horizon-host","version":"0.2.0","game":"tmnf","gameName":"TrackMania Nations Forever","instance":"<16 hex>"}`.
2. TM Horizon shows "Horizon in TrackMania Nations Forever wants to connect. Allow / No" and answers
   `{"ok":false,"status":"pending"}` while the question stands (180 s after the last ask; the host asks
   every 2 s), `{"ok":false,"status":"denied"}` for 60 s after No, and `{"ok":true,"key":"<64 hex>"}`
   once the player pressed Allow.
3. The host keeps the key in `horizon\settings\host.json` (`bridge.key`). A 401 later drops it and pairs again.
   "Forget games" in TM Horizon deletes all keys.

## Paths

| Request | Answer |
|---|---|
| `GET /v1/client/ping` | `{"ok":true,"app":"tm-horizon","version":"0.3.0","api":1}` |
| `POST /v1/client/pair` | see above |
| `GET /v1/client/hello` | `{"ok":true,"app":"tm-horizon","version":"..."}` |
| `GET /v1/client/events?since=N&wait=W&caps=C` | long poll, W up to 25 s: `{"ok":true,"last":M,"events":[{"id":..,"kind":"upload","level":"ok","title":"..","text":"..","at":<ms>}]}`. Without `since` the answer has no events, only `last` (start after it). `kind`: `upload`, `mod`, `server`, `info`, `play_map`; `level`: `info`, `ok`, `warn`, `error`. `caps` (optional, comma-separated): what this host can do, see "Play a map". |
| `GET /v1/tmx/search?game=&name=&author=&tags=&difficulty=&lengthmax=&lengthmin=&awards=&page=` | `{"ok":true,"site":"tmnf.exchange","page":1,"more":true,"results":[Track]}` (20 per page) |
| `GET /v1/tmx/random?game=&tags=&difficulty=&lengthmax=&lengthmin=&awards=` | `{"ok":true,"result":Track}` or `{"ok":true,"result":null}` |
| `GET /v1/tmx/thumb?game=&id=` | the map's thumbnail (`image/jpeg` or `image/png`, at most 1 MB), 404 none |
| `GET /v1/tmx/map?game=&id=` | the `.Challenge.Gbx` (`application/octet-stream`, at most 8 MB), header `X-File-Name` |
| `GET /v1/tmx/record?game=&uid=` | `{"ok":true,"found":true,"site":"..","trackId":..,"replayId":..,"time":..,"player":".."}` or `{"ok":true,"found":false}` |
| `GET /v1/tmx/replay?game=&id=` | the record's `.Replay.Gbx` bytes (at most 8 MB) |

`game` is the host's site code: `tmn` (Nations ESWC, nations.tm-exchange.com), `tms` (Sunrise,
sunrise.tm-exchange.com), `tmo` (Original), `tmnf` (tmnf.exchange), `tmuf` (tmuf.exchange). For
`tmnf` the record lookup also tries tmuf.exchange (a Stadium map can be on either).

Track:

```json
{"id":2233,"name":"A01-Race","author":"Nadeo","awards":357,"tags":["Race"],"difficulty":"Beginner",
 "authorTime":24540,"environment":"Stadium","uploaded":"2008-04-12T00:00:00Z","thumbnail":true,"uid":"..."}
```

Search filters: `name`, `author` (text), `tags` (comma-separated TMX tag ids: 0 Race, 1 Stunt, 2 Maze,
3 Offroad, 4 Multilap, 5 FullSpeed, 6 LOL, 7 Tech, 8 SpeedTech, 9 RPG, 10 PressForward, 11 Trial,
12 Grass, 13 Story, 14 Nascar, 15 Speedfun, 16 Endurance, 17 Altered Nadeo, 18 Transitional),
`difficulty` (0 Beginner, 1 Intermediate, 2 Expert, 3 Lunatic), `lengthmin`/`lengthmax` (author time
in ms, TMX `authortimemin/max`), `awards` (at least this many; TMX has no such filter, so TM Horizon
drops the rest of each page). Unlimiter maps (`inunlimiter=0`) are always left out.

## Host side

- The bridge runs on one thread of the host, only while the bridge switch in Horizon's General page is
  on (default on). Without TM Horizon it probes the ports every 10 s, nothing else.
- Per plugin at most 8 open requests and 4 per second (the host answers `HORIZON_BRIDGE_ERR_RATE_LIMIT`
  without sending). Answers of a plugin that was switched off are dropped (`HORIZON_BRIDGE_ERR_CANCELLED`
  is not delivered after Shutdown).
- Events from `/v1/client/events` become toasts. While a race runs and "Quiet while driving" is on they wait until the run ends; with "Do not disturb" they are only listed on the
  General page.

## Notifications from TM Horizon

TM Horizon adds an event when a replay upload finished (ok or failed), when updates for installed mods
were found, and when the launcher's frontend calls `client_notify` (server events). Events are kept
for 10 minutes (at most 50).

## Play a map (`play_map`)

TM Horizon can ask a running game to open a map file that is already in the game's Tracks folder.

- Capability: a host that can do it adds `caps=play_map` to every events long poll, the first call too
  (`/v1/client/events?caps=play_map`, then `/v1/client/events?since=N&wait=20&caps=play_map`). Hosts
  0.2.0 and older send no `caps`; launchers that do not know it ignore it. Only the ESWC, Sunrise and
  Original hosts send it. Forever (TMNF/TMUF) never does and ignores `play_map` (log line only).
- Event: `{"id":..,"kind":"play_map","level":"info","title":"<map name>","text":"","at":<ms>,"game":"tmn","path":"Challenges/Downloaded/12345.Challenge.Gbx"}`
  - `game`: the site code of the target game (`tmn`, `tms`, ...). Several games can be paired at once;
    a host ignores events for another code.
  - `path`: the map file relative to the game's Tracks folder, `/` or `\`. The host refuses it when it
    is empty or longer than 400 bytes, starts with `/` or `\` (absolute, UNC), has a `:` (drive, stream),
    a `.`, `..` or empty part, a part ending in `.` or space, a device name (`CON`, `NUL`, `COM1`, ...),
    a control character or one of `*?"<>|`, or does not end in `.Challenge.Gbx` (any case). The file must
    exist under the Tracks folder after the path is resolved.
  - Tracks folder: `<game folder>\GameData\Tracks\` for ESWC, Sunrise and Original (the folder of the exe; TM
    Horizon saves downloads to `GameData\Tracks\Challenges\Downloaded\`).
  - `title`: shown in the result toast (plain text; strip TM format codes). May be empty.
  - `at`: a request older than 120 s (by this PC's clock) is dropped, e.g. one still queued when the
    host reconnects.
- On the game thread the host does what `play_map_file` does: refused while online or in a network
  race ("Leave the online game first."), only from the main menu ("Go to the main menu first." during a
  race, in the editor or while loading). The game gets a copy of the file in
  `Tracks\Challenges\Downloaded\_Horizon\` and starts it in solo. Several requests in one poll: only the
  newest runs.
- Result: a toast with the map name ("Loading map." or the reason) and a log line. Nothing goes back
  to TM Horizon.
