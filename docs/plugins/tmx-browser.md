# TMX Browser

Plugin id `tmx-browser`, version 0.2.0. ESWC, Sunrise, Original, TMNF and TMUF, online-ok, live toggle, off by default (switch it on in the menu; it loads right away). It searches the TrackMania Exchange of the running game from inside the game, shows map cards with pictures, and saves or plays a map. It talks to TMX itself; TM Horizon is not needed.

It needs a host with ABI 1.3 (panels, toasts). On an older host `Init` fails and the plugin shows as failed.

## What it does

Two tabs on the plugin's page, no settings page:

- **Browse**: the exchange at the top (`tmnf.exchange`, `original.tm-exchange.com` ...), then the search form and the result cards.
  - Form: "Map name", "Author", "Style: ..." (a pop-up with the TMX tags, "Clear"), "Any difficulty" (Beginner, Intermediate, Expert, Lunatic), "Any length" (< 30 s, 30-60 s, 1-2 min, > 2 min, by author time), "Awards: N+", the buttons "Search" and "Random", and "More" for the next page (20 maps per page).
  - Cards: picture, name, author, awards, difficulty, tags. "Small cards" / "Large cards".
  - A click on a card opens its detail: Author, Awards, Difficulty, Author time, Environment, Uploaded, Map ID, tags, and the buttons "Play" (or "Play later"), "Save", "Favourite" / "Unfavourite", "Back". "Open on TMX" shows the map's web address with a "Copy" button.
- **Favourites**: the maps you starred, per game, with the same cards and detail. "No favourites" when empty.

The form (name, author, tags, difficulty, length, awards) and the card size are remembered between starts.

## Play and Save

- **Save** downloads the map from TMX (`https://<exchange>/trackgbx/<id>`) and writes it to `<maps folder>\Downloaded\_Horizon\<name> (<id>).Challenge.Gbx`. The maps folder is `Documents\TrackMania\Tracks\Challenges` on Forever (also under TMLoader; the game folder's `GameData\Tracks\Challenges` with `/useexedir`), `GameData\Tracks\Challenges` in the game folder on ESWC, Sunrise and Original. The file is checked before it is written: at most 8 MB, a GBX file of the challenge class (`0x03043000`, older games `0x24003000`); anything else is refused ("Not a map file", "Map file too large"). Status "Saved", toast "Map saved".
- **Play on ESWC, Sunrise and Original**: Save, then the host's `play_map_file` (game control, the same path TM Horizon's play request uses), two frames after "Loading map..." is on screen, because the game rescans its map list while it opens the map. Toasts "Map started" / "Map not started".
- **Play on TMNF and TMUF**: Forever stays read-only, so the plugin neither writes the map nor calls into the game. It asks TMX for the map's play link (`https://tmnf.exchange/trackplay/<id>` answers `302` with a one-time `tmtp:///:tmx:download:tmnf?token=...`; on tmuf.exchange `...:download:tmu?...`) and hands that link to the game the way Windows does when you click a tmtp link while the game runs: the game window gets the game's own single-instance message (`WM_COPYDATA`, `dwData` `'LRUC'`, what `TmForever.exe /singleinst /url=<link>` sends). The game then downloads and opens the map itself. Status "Opened in the game". (The host's `play_map_file` on Forever hands a local file to the game the same way, with the game's file message; TMX Browser does not need it.)
- Play is refused at once, with no request and no game call, when it is not allowed: "Leave the server first" (online or a network race), "Leave the race first" (solo race, end screen), "Run in progress" (the host's `actions_allowed` is 0, which includes a Competition Patch run), "Main menu only" (editor, replays, loading). The button then reads **Play later**: the map starts by itself once you are back in the main menu, offline and not in a run ("Plays in the main menu", "Cancel"). On the old games the map is downloaded right away, on Forever the play link is asked for when it starts (the token is single-use).
- If the game state changed while the map downloaded, the map is saved and not started ("Saved. Leave the race first").
- One map at a time ("Busy").
- Search, pictures, favourites and Save also work online (the plugin is online-ok). The plugin does not touch input, timing or replays.

## TMX requests

- The exchange of the running game: `nations.tm-exchange.com` (ESWC), `sunrise.tm-exchange.com`, `original.tm-exchange.com`, `tmnf.exchange`, `tmuf.exchange`. Only https to these five hosts, also after redirects. User agent `TM-Horizon/<host version> (+https://tm-horizon.com)`.
- TMX API v2: `/api/tracks?fields=...&count=20&inunlimiter=0` with the filters (`name`, `author`, `tag`, `difficulty`, `authortimemin`, `authortimemax`), the next page with `after=<last map id>`. "Random" asks for 100 maps with the filters (no name or author) and picks one. TMX has no awards filter: the plugin drops maps with fewer awards from each page, so a page can hold fewer than 20 maps. Unlimiter maps are left out.
- Rate: search, random, map files and play links go one at a time, at least 1 s apart. Pictures go three at a time, starts at least 250 ms apart, at most six waiting.
- Pictures: Forever maps carry their own thumbnail (`HasThumbnail`, `/trackshow/<id>/thumbnail`, a few KB). On Nations, Sunrise and Original TMX never sets that flag; the only pictures there are uploaded screenshots (`Images`, `/trackshow/<id>/image/1`), and a map without a screenshot shows "No preview". (Up to 0.1.0 the plugin asked for a picture only when `HasThumbnail` was set, so Original, Sunrise and Nations showed none.) TMX's grey "no picture" logo is recognised and not shown. Pictures are decoded on the worker threads (stb_image, shrunk to 256 px wide) and cached on disk; the game thread only creates the texture.
- Errors on the card or status line: "No connection to TMX", "TMX timed out", "Too many requests", "Not found on TMX", "TMX error N", "Bad answer from TMX", "No map found", "No maps found", "No play link from TMX", "Game did not answer".

## What it reads and writes in the game

Nothing is read from game memory, except the game state, the online flag, and the game and user folders the host reports. It writes the downloaded map file (not for Forever's Play) and its own data files. No hooks, no code patches, no writes to game memory. On Forever the only contact with the game is the tmtp link message above.

## Where data is stored

In the plugin's data folder `horizon\data\tmx-browser\`:

- `favourites.json`: the favourites (with the map data shown on the card, the picture kind, the game and the date added). An unreadable file is ignored and starts empty.
- `thumbs\<site>-<id>.img`: the picture cache (the bytes TMX sent), at most 600 files; the oldest are deleted at start.

Downloaded maps go to the game's `Tracks\Challenges\Downloaded\_Horizon\` folder (see above) so the game lists them. At most 48 pictures are kept as textures and released when the plugin is switched off. Search settings and card size are in `horizon\settings\tmx-browser.json`.

## Tests and known limits

- `plugins/tmx-browser/tests`: unit tests (URLs, TMX answers, picture kinds, map file check, tmtp link check, cache prune) and a fake-host test on all five games with a fake TMX (`tests/fake_tmx.h`, no network): requests, pictures and the disk cache, paging, Play (legacy download + `play_map_file`, Forever link), the online and race gates, Play later, the map check, favourites, texture release.
- The Forever link message is the game's own single-instance path (checked statically in TMLoader 2.12.0's `TmForever.exe`: `/singleinst` sends `WM_COPYDATA` 'LRUC' with the `/url` text to the running game's window, whose window procedure hands it to the game's URL handler, the one `/url=tmtp://...` uses at start). Not run in a game yet. What the game does with a TMX link while it sits on a server was not tried, so the plugin never sends it there.
- ESWC, Sunrise and Original Play is the host's game control (main menu only); not run in game on every exe yet.
