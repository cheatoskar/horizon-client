# TMX Browser

Plugin id `tmx-browser`. ESWC, Sunrise, Original, TMNF and TMUF, online-ok, live toggle, off by default (switch it on in the menu; it loads right away). It searches the TrackMania Exchange from inside the game, shows map cards with thumbnails, and downloads or starts a map. It needs **TM Horizon** running on the same PC: the game never goes online, TM Horizon talks to TMX (`docs/client-bridge.md`).

It needs a host with ABI 1.3 (panels and the bridge). On an older host `Init` fails and the plugin shows as failed.

## What it does

Two tabs on the plugin's page, no settings page:

- **Browse**: a status line at the top ("Connected to TM Horizon", "Start TM Horizon to search TMX", "Press Allow in TM Horizon", "Denied in TM Horizon, it will ask again soon", "The TM Horizon bridge is off"), then the search form and the result cards.
  - Form: "Map name", "Author", "Style: ..." (a pop-up with the TMX tags, "Clear"), "Any difficulty" (Beginner, Intermediate, Expert, Lunatic), "Any length" (< 30 s, 30-60 s, 1-2 min, > 2 min, by author time), "Awards: N+" (minimum number of awards), the buttons "Search" and "Random" ("A random map that fits the filters."), and "More" for the next page (20 maps per page).
  - Cards: thumbnail, name, author, awards, difficulty, tags. Cards can be small or large ("Small cards" / "Large cards").
  - A click on a card opens its detail: Author, Awards, Difficulty, Author time, Environment, Uploaded, Map ID, tags, and the buttons "Play", "Save" ("Download without playing."), "Favourite" / "Unfavourite", "Back". "Open on TMX" shows the map's web address with a "Copy" button.
- **Favourites**: the maps you starred, per game, with the same cards and detail. "No favourites yet. Click the star on a map." when empty.

The form (name, author, tags, difficulty, length, awards) and the card size are remembered between starts.

## Play and Save

- **Save** downloads the `.Challenge.Gbx` through TM Horizon and writes it to `<maps folder>\Downloaded\_Horizon\<name> (<id>).Challenge.Gbx`. The maps folder is `Tracks\Challenges` in the user folder on Forever, `GameData\Tracks\Challenges` in the game folder on ESWC, Sunrise and Original (site code `tmo`, original.tm-exchange.com). Toast: "Map saved".
- **Play** does the same and then starts the map with the host's `play_map_file` (game control, `HORIZON_CAP_GAME_CONTROL`). Toasts: "Map started", "Could not start the map".
- Play is greyed out with a message when it is not allowed: "Playing is off while online", "Playing is off during a run" (the host's `actions_allowed` is 0, which includes a run under the Competition Patch), "Go back to the main menu to play". So it plays only offline and only from the main menu. If the game state changed while the map downloaded, the map is saved and the toast "Map saved" says why it did not start.
- One map at a time ("Busy, one map at a time").
- Search, thumbnails, favourites and Save also work online (the plugin is online-ok); only Play is blocked. Nothing is changed in the running game. The plugin does not touch input, timing or replays.

## What it reads and writes in the game

Nothing is read from game memory, except the game state, the online flag, and the game and user folders the host reports. It writes only the downloaded map file. No hooks, no code patches, no writes to game memory. `play_map_file` is the same host function other plugins use to start a map file.

## Filters and TM Horizon

Search goes to TMX's search API through TM Horizon, which calls TMX with its own HTTP client, at most one TMX request per second, with caches. TM Horizon leaves out Unlimiter maps. The awards filter is applied by TM Horizon, not by TMX: TMX has no such filter, so TM Horizon drops the maps with fewer awards from each page. A page can therefore hold fewer than 20 maps.

Errors shown on the card or status line: "TM Horizon is not connected", "TM Horizon did not answer in time", "Too many requests, wait a moment", "The answer was too large", "Press Allow in TM Horizon", "Not found on TMX", "TMX did not answer (N)", "No map matches these filters", "No maps found.".

## Where data is stored

In the plugin's data folder `horizon\data\tmx-browser\`:

- `favourites.json`: the favourites (with the map data shown on the card, the game and the date added). An unreadable file is ignored and starts empty.

Downloaded maps are not in the data folder: they go to the game's `Tracks\Challenges\Downloaded\_Horizon\` folder (see above) so the game lists them. Thumbnails are not stored; at most 48 are kept in memory as textures and released when the plugin is switched off. Search settings and card size are in `horizon\settings\tmx-browser.json`.

## Known limits

- The logic and the panels are tested with a fake host and fake TM Horizon answers (`plugins/tmx-browser/tests`), not against the real TM Horizon.
- `play_map_file` on Forever (TMNF, TMUF, also on TMLoader's relocated exe) is not confirmed in the game yet; on ESWC, Sunrise and Original it is the host's game control (main menu only).
- The thumbnail decoder reads what stb_image reads (JPEG, PNG) and shrinks it to 256 px; a thumbnail that fails shows "No preview".
- The map file is checked only for the `GBX` header, not for being a valid map of the running game.
