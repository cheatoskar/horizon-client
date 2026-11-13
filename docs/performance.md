# Horizon Performance

A Horizon plugin (`plugins/performance`, id `performance`) for TMNF, TMUF, Nations ESWC and Sunrise: measure first, then switch single options on and compare. Plus a hang log for long loading stalls. The launcher side ("Optimieren", benchmark button, replay archive, start recorder, DXVK) lives in `tm-hub-mono/apps/launcher/docs/optimize.md`.

Status 2026-11-11: built and tested without the game (real host + plugin on a Direct3D 9 device made like the game's, fake host on all four games). **No number in this file comes from the game yet.** My test plan for the game is in `TESTPLAN.md`, section "Horizon Performance".

## What it measures

| Value | How |
|---|---|
| Frame time | From one `Present` call to the next (the moment the game hands a finished frame over). |
| Game / Present / limiter | "Present" is the time inside `Present` incl. Horizon's own menu drawing (long = waiting for the GPU or V-Sync); "limiter" is the frame limiter's wait; "game" is the rest: the engine's own work for the frame. Needs host ABI 1.3 (`post_present`); with an older host only the frame time is right. |
| 1 % low / 0.1 % low | 1000 / mean of the slowest 1 % (0.1 %) of frame times in the window. |
| Stutter | A frame longer than 2x the median frame time of the window **and** at least 6 ms longer than the median. (Until 2026-11-12 only the factor: at 400 fps in a menu every 5 ms frame counted, my ESWC log showed "1332 stutters" and "2499 stutters" in 30 s around loading, none of them visible.) |
| Direct3D calls (switch "Count Direct3D calls") | Per frame: draws (and how many with software vertex processing), render/sampler/texture-stage states and how many repeat the value already set, textures, shaders, constant uploads, streams, render targets, locks (and how many without DISCARD/NOOVERWRITE, with their time), readbacks (`GetRenderTargetData`), query polls and the time the CPU waited for the GPU through them. |

Frames longer than 5 s (loading screens) are kept out of the statistics; the hang log covers them. Also left out (the `stats:` line says how many): frames while the game window is in the background or minimised, frames whose `Present` failed (device lost), the first second after a device Reset, after the window comes back to the front, after a game state change or a map load, and frames while the host reports "loading". A benchmark window leaves out the same, except "loading" (see the series).

Everything is in the "Frame times" window (a Horizon HUD panel: FPS, lows, a graph of the last frames with the stutter line, the call counts) and on the plugin's page. Every 30 s one `stats:` line goes into the Horizon log, so two starts can be compared from the logs alone.

Cost of measuring: the frame timer costs nothing measurable. The call counter goes through 36 hooked methods; in my synthetic test (200 draws, 1800 state calls, ~2600 hooked calls per frame) the difference was inside the noise (1.27 ms without, 1.40 ms with, +-0.2 ms between runs). **Benchmarks of other options run with the counter off.**

## Smooth / Low latency / Custom (0.3.1)

The choice at the top of the page; what the module does as soon as it is switched on (it is off by default):

| | Smooth (default) | Low latency | Custom |
|---|---|---|---|
| Skip repeated states | on (its own checks still apply, off with the Competition Patch) | on | the player's |
| Power throttling (EcoQoS) | off for the game | off | the player's |
| Priority | Above normal | Above normal | the player's (High only here: it can starve audio, voice chat and streaming for little gain) |
| Frame limit | refresh rate of the monitor the game window is on | same | the player's |
| V-Sync | as the game has it | off (applies at the next start or Alt+Enter) | the player's |

The refresh rate is read from the monitor of the game window (`EnumDisplaySettings`), again when the device is
created; unknown = no limit. Measuring ("Count Direct3D calls") stays the player's choice. A player who set
options before 0.3.1 starts on Custom. `perf::presetOptions` (series.h), unit test `testPresets`.

## Benchmark

- **By hand:** Seconds and Label on the page, then "Start" (or the hotkey). It records from that moment.
- **Hotkey:** one key, default **Ctrl+Shift+B** ("Benchmark start/stop", rebindable on the Hotkeys page): starts a manual benchmark, starts a waiting request, and stops a running launcher benchmark or series. Ctrl+F10 is deliberately not used because Legacy Randomizer uses it for its gold skip; a key stored in an earlier test (Ctrl+F10) stays until you rebind it on the Hotkeys page.
- **From the launcher ("Benchmark starten"):** the launcher writes `<data>\bench-request.json` and starts the game. Mode `map`: as soon as the main menu is there (3 s settled), the plugin starts the chosen map in solo, waits for the race, warms up (default 5 s) and records `runs` windows of `seconds` one after another while the car stands at the start (a fixed camera on a known map), then goes back to the menu. Mode `replay`: I open a replay myself; recording starts when the game state is "replay". The request is renamed to `bench-request.taken.json` at once, so a crash never repeats it.
- Results: `<data>\bench\<yyyymmdd-hhmmss>_<label>[_<run>].json` (summary) and `.csv` (one line per frame, all counters). `<data>` is `horizon\data\performance\` (ESWC/Sunrise) or `...\TMLoader\...\products\Horizon Host\data\performance\` (Forever). The summary holds every switch's state (`options`), so a result says what was on.

Summary fields: `version, game, label, request (map/replay/manual), run, started, seconds, frames, avgFps, avgMs, medianMs, p99Ms, p999Ms, maxMs, low1Fps, low01Fps, stutters, presentMsAvg, cpuMsAvg, sleepMsAvg, drawsPerFrame, options{}`.

## Benchmark series

One game start, no clicking, a table "option -> effect". The plugin runs the same scene several times and switches one option set per window; `tools/bench_report.py` turns the result into the table. Nothing in it has been run in the game yet (offline: the state machine against a fake host, and the real host with the plugin, `tests\run_perf_tests.cmd`).

**Start without the launcher:** `tools/bench_series.ps1 -GamePath <game folder>` writes `bench-request.json` into `<game>\horizon\data\performance\` (ESWC, Sunrise). `tools/bench_series.ps1 -Forever` (TMNF and TMUF via TMLoader) writes the request into the current folder and prints the one command that copies it to where the Forever host reads it (`%LOCALAPPDATA%\TMLoader\database\TmForever\products\Horizon Host\data\performance\`, the same for TMNF and TMUF); `-Forever -Install` copies it itself. Then start the game (about 10 minutes with the defaults: 7 sets x (1 warm-up + 3 runs) x (20 s + 2 s settle)).

**The module does not have to be on:** Performance is off by default. When the host finds `data\performance\bench-request.json` at the start, it loads the module for that start only (log: "on for this start only: a benchmark request is waiting", toast "Benchmark wartet"); the switch in the menu stays where it was. With a TM Horizon profile that has the module off, the host does not override it and shows the toast "Benchmark wartet: Performance-Modul in TM Horizon einschalten".

**On screen:** while a request waits or runs, a line at the top of the screen (drawn by the module itself, independent of the frame-time window, which may be off or docked in the menu) says what to do and shows the progress: "Warte auf Replay: öffne ein Replay (mind. 40 s)" / "Startet von selbst. Sonst Ctrl+Shift+B drücken, sobald das Replay läuft", then "Benchmark startet in 3 s", "Benchmark 7/21: priority-high, Lauf 2/3 (noch etwa 6 min)", and 20 s after the end "Benchmark fertig: <stamp>_series.json im Ordner bench".

**Start condition (first ESWC run, 2026-11-11: the replay looped, nothing happened):** replay mode waited for the host's game state "replay". ESWC never reported it: a replay watched from the menu runs in the app's clip viewer (`CConstructionGame` +0x128, `CCtnMediaClipViewer` 0x2408a000), not in `CurrentRace` and not in the `Editor` slot, so the host said "Loading" the whole time (log: "game state Loading" after "map loaded", nothing after it). Fixed in the host (ESWC: clip viewer + map = "replay", log line "a replay plays (the app's clip viewer)"); TMNF/TMUF already report replays (a race of another class or the replay editor). Sunrise's slot is unknown: there a watched replay stays "Loading". So in replay mode the series starts **either** when the host reports a replay **or** when I press the benchmark key while the replay plays (any game); after a key start the scene is not checked any more. `"mode": "now"` starts by the key only (`"startAfter": <s>` starts it that many seconds after the game start, for the tests). Pressing the key while the series runs stops it.

**The request** (read once at the start, renamed to `bench-request.taken.json`):

```json
{"version": 1, "series": {
  "replay": "<path or name>", "runs": 3, "warmup": 1, "seconds": 20, "settle": 2,
  "sets": [{"label": "baseline", "options": {}},
           {"label": "limiter+vsync-off", "options": {"limitFps": 120, "vsync": 1}}]}}
```

| Field | Meaning | Default |
|---|---|---|
| `replay` | Path or name of the replay (see below). `map`: path of a `.Challenge.Gbx`, then the plugin starts that map in solo itself. `mode` is `replay`, `map` or `now` (`now`: measure whatever is on screen; for tests). | `replay`, the newest one |
| `runs`, `warmup` | Measured windows and discarded windows per set. | 3, 1 |
| `seconds`, `settle` | Length of one window; wait after switching the options before recording. | 20, 2 |
| `sets` | `label` and `options` (`count`, `filter`, `limitFps`, `vsync`, `priority`, `affinity`, `powerThrottlingOff`, `fineTimer`; same names and values as in the result files, missing = off). | the 7 sets below |

Default sets: `baseline` (all off), `limiter+vsync-off` (limit 120, V-Sync off), `priority-high`, `perf-cores`, `skip-repeated-states`, `fine-timer`, `no-power-throttling`. Every set starts from "everything off" plus its own options, so each one is compared with the baseline, never with the one before it.

**Order:** interleaved by rotation: warm-up round, then the measured rounds, and every round starts one set later (A B C, B C A, C A B ...). Heat and background load then do not favour one set. The state filter (and the call counter, if a set asks for it) needs the Direct3D hooks: they are attached for the whole series, so every window runs with the same hooks, only the mode differs.

**What cannot change at run time:** V-Sync is applied when the device is created, and the plugin does not trigger a device Reset. A set whose only option is V-Sync is not run and shows "needs restart: vsync" in the result; in a set with other options V-Sync is left as the game started with and the run's `note` says so. So `limiter+vsync-off` measures the limiter only. Options that turn out unavailable after the settle time (state filter with a wrapper `d3d9.dll`, Competition Patch) skip that set once with the reason.

**Replay or map:** the host cannot open a replay (ABI 1.3 has `play_map_file` only), so in replay mode I open the replay once myself; the line on screen says which one if the request names it. From then on it runs by itself. A window is `seconds` of frames that show the scene: frames in the background, after a Reset, while the replay is gone or in the `settle` time after it comes back are not counted and do not end the window, so a looping replay just continues (its restart counts as settle). A replay gone for more than 5 s starts the current window over; a map race gone for 5 s stops the series. `-Map` (or `"map"` in the request) is fully unattended, but the car stands at the start. Picking a replay: `"replay": "C:\\path\\x.Replay.Gbx"`, or a name, looked up in `<user folder>\Tracks\Replays\` and `...\Autosaves\` (`.Replay.Gbx` is added). Without `replay`: the newest `*.Replay.Gbx` in `Tracks\Replays\Autosaves`, else in `Tracks\Replays`. The user folder is the game's own (`Documents\TrackMania...`).

**Progress and safety:** the line on screen and the frame-time window show "Benchmark 7/21: priority-high, Lauf 2/3" (warm-ups: "Benchmark Warmup 2/7: label"). The benchmark key (Ctrl+Shift+B) or "Stop" on the page stops the series. At the end, on a stop, when the race or replay ends, when the plugin is switched off or the game quits, my own settings are put back (the settings file is never written during a series, so a crash leaves them untouched too). Priority, affinity, timer and power throttling go back to what the game had.

**Result files** in `<data>\bench\`: `<stamp>_series.json` (everything: sets, status, every run's numbers, GPU, CPU, resolution, game, replay or map, TM Vibrant Shaders yes/no, `completed`) and per window `<stamp>_series_<label>_r<N>.json/.csv` (measured) or `_w<N>` (warm-up), same format as a single benchmark with `"request": "series"`. TMVS is "yes" when `TMVibrantShaders.dll` is loaded or another `d3d9.dll` wraps the device (probably TMVS standalone).

**Analysis:** `python tools/bench_report.py <stamp>_series.json` prints and writes `<stamp>_series.md`: per set average FPS, median ms, 1 % low, 0.1 % low and stutters per minute (mean of the measured runs), each with the difference to the baseline in percent. Hint column: "Unterschied kleiner als die Streuung" when the difference of the means is smaller than max-min of the baseline's own runs (for every metric); otherwise the metrics that are above it. With one baseline run there is no spread and the hint says so. Test: `python tools/test_bench_report.py` (synthetic data, also run by `tests\run_perf_tests.cmd`).

## Where the frame time goes (static analysis, test copies only)

From the exes of my test copies of the games (imports, call sites with capstone, TMNF symbols from `TmForever.map`), and TM Vibrant Shaders' notes. Measured in the game: nothing yet.

| Finding | Meaning |
|---|---|
| TMNF/TMUF create a **mixed vertex processing** device (flags 0x80), not pure, not multithreaded (TMVS GOTCHA; my test device uses the same flags). ESWC/Sunrise: the plugin logs the flags at the first start. | The D3D9 runtime keeps the device state for non-pure devices, so a state filter in front of it saves little; the counter shows how many calls repeat. No `MULTITHREADED` lock cost. |
| The engine compiles its shaders at run time (`d3dx9_30!D3DXCompileShader`, TMNF/TMUF). | A source of hitches when a material is seen first. A shader cache would need an IAT hook of the exe: not built (Competition Patch rule for Forever; unmeasured on ESWC/Sunrise). |
| **GPU sync:** `CVisionViewportDx9::SyncGpuWaitQuery` (TMNF 0x98b890) polls an event query with `D3DGETDATA_FLUSH` and `Sleep(0)` until the GPU is done; the game's own setting `GpuSync` / `GpuSyncTimeOut` (CSystemConfigDisplay) controls it. | The CPU waits for the GPU every frame while it is on: lower input delay, fewer FPS. The counter's "GPU wait" shows exactly this time. A second render-ahead limiter of my own would duplicate it: not built. |
| `MwSystemTimerInit` calls `timeBeginPeriod(<minimum>)` (1 ms) at start on TMNF and Sunrise. **ESWC (`983bd127`) and Original (Online Patch v2) do not**: the call is patched out in both community builds (2026-11-13, `docs/performance-smooth.md`). | TMNF/TMUF/Sunrise: the usual "timer resolution fix" is already done by the game. ESWC/Original: the switch "Timer 1 ms" restores it. |
| `CSystemEngine::CaptureInfoOsCpu` sets REALTIME priority, thread priority 15 and affinity 1 **only around its CPU speed measurement at start**, then restores the old values. | The game does not pin itself to one core and keeps normal priority while playing. |
| Adaptive view distance: `CHmsViewport.TargetFps*` / `CSystemConfigDisplay.ZClip`, `ZClipAuto` (the engine cuts the view distance when FPS drop); `Shadows`, `LightMapQuality`, `TexturesQuality`, `VertexProcess`, `MultiThreadEnable` (TMNF) are game settings. | These are better set in the game's own config (the launcher shows them) than overwritten at run time. The page shows the engine's own FPS and the adaptive view distance read-only. |
| No exe is large address aware (2 GB). | The page shows used address space; I do not patch the exe. |

## Options

All off by default. Each one is a switch with a one-line explanation on the page.

| Option | What it does | Games | Competition Patch | Online | Expected effect | Measured (offline) |
|---|---|---|---|---|---|---|
| FPS limit | Waits after `Present` until the next frame slot (high-resolution waitable timer, last 0.7 ms spinning). No catch-up bursts after a hitch. | all | yes (only Present timing) | yes | Steadier frame times, less heat; lower FPS by design. Physics is a fixed 100 Hz tick, so it does not change driving. | 60 fps target: 16.669 ms mean over 100 frames (target 16.667). |
| V-Sync | `PresentationInterval` at device creation / Reset: Default (as the game sets it), Off, On. | all | yes | yes | Off + limiter: less input delay than V-Sync. | not measured |
| Skip repeated states (experimental) | Does not pass `SetRenderState`/`SetSamplerState`/`SetTextureStageState`/`SetTexture`/`Set*Shader` on when the value is already set. Off while a state block records; forgets everything after `StateBlock::Apply`, Reset and each frame's pointers. Blocked when another mod hooks a slot before mine or a wrapper/non-system `d3d9.dll` is used (TMVS standalone, Sunrise proxy, DXVK). | all | **off** (patches d3d9's method tables) | yes | Small: the runtime already keeps the state. Only worth it if the counter shows many repeats AND the game is CPU-bound. | Image pixel-identical with and without (checksum of the test scene, native d3d9 and DXVK). Frame time difference within noise. |
| Fine timer (0.5 ms) | `NtSetTimerResolution(0.5 ms)` while the game runs; given back at exit. | all | yes | yes | Small: the game already asks for 1 ms. | timer reads back 0.5 ms |
| No power throttling | `SetProcessInformation(ProcessPowerThrottling)`: no EcoQoS, and Windows 11 keeps honouring the timer when the window is hidden. | all | yes | yes | Helps laptops/hybrid CPUs that park the game on efficiency cores. | not measured |
| Priority | Above normal / high (process priority class). | all | yes | yes | Fewer stutters from background load; nothing on an idle PC. | not measured |
| CPU cores | All but CPU 0, or performance cores only (hybrid CPUs, by EfficiencyClass). | all | yes | yes | Hybrid Intel CPUs: may help 1 % lows. Elsewhere: nothing or worse. | not measured |
| Count Direct3D calls | Measurement only (see above). | all | **off** | yes | costs a little | see above |
| Hang log | See below. | all | yes (reads only, never during a race) | yes | none | caught a 2.5 s stall with the right stack |

Not built, and why:
- **Shader cache / texture preload:** needs hooks in the exe (D3DX import) or engine calls; not allowed on Forever with Competition Patch, and without a measured hitch on ESWC/Sunrise it would be a guess.
- **Large Address Aware patch:** changes the exe; I only show the address space.
- **Writing engine values (view distance, shadows) at run time:** the game's own settings do it; the launcher shows them. Forever never gets game-memory writes.
- **Render-ahead limiter:** the engine has its own (`GpuSync`).

## Hang log (startup stall, replay list refresh)

While the game thread sends no frame for longer than "After (ms)" (default 1.5 s) and no race runs, a watcher thread suspends the game thread for a few microseconds, copies its registers and the top 16 KB of its stack, resumes it, and only then writes the return addresses to `<data>\hangs.log` (`module+offset`, export names of system DLLs, TMNF function names from `TmForever.map` in the game folder). At the end of a stall: duration, disk read/write bytes and operations of the process during the stall, and the function most samples were in. Game states and map loads are written with seconds since the process started. Nothing is hooked; it works with or without TMLoader. Offline test: a 2.5 s `Sleep` in the test program was found as `KERNELBASE.dll!SleepEx` -> `perf_test.exe+0x1307b` in 3 of 3 samples.

### Startup stall after choosing the profile: what the code says (TMNF, `TmForever.map`)

Right after the profile is chosen the game runs `CGameCtnApp::InitAfterProfiles` (0x6057e0). In it, ranked by how well they fit "sometimes very long, the 2nd/3rd start is fine, TMNF and TMUF, with and without TMLoader, shared `Documents\TrackMania`":

1. **File cache refresh** `CGameCtnApp::LoadAndRefreshFidCache` (0x5eb690, called from InitAfterProfiles+0x268). It loads `Game.FidCache.Gbx` (`C:\ProgramData\TrackMania`) and `Config\User.FidCache.Gbx` (`Documents\TrackMania`), then `CPlugFileFidCache::InstallAndUpdate`: every folder marked dirty is re-read file by file (`CreateCache_FromFolder`: `UpdateFidProps` + `LoadHeaderUserData` for each file), and the cache is written back. The same function runs at a normal quit (`QuitGameInternal`). So: a crash, a kill or a start after the other game (TMNF and TMUF share `Documents\TrackMania`) leaves dirty folders; with ~3000 replays one changed folder means thousands of header reads at the next start, and the start after that is fast again. My file shows `User.FidCache.Gbx` at 1.8 MB. **Best fit.**
2. **Replay scan** `ScanDiskForReplays` (InitAfterProfiles+0x642) -> `CGameCtnReplayRecordInfo::RefreshFromDisk`: lists every replay under the replay folders recursively (`GetAllChildFidByClassId`, depth -1) and builds an info object per file. Cheap with a warm cache, slow without (point 1) or with a virus scanner checking each `.Gbx` it opens.
3. **Internet check and master server login**: `CGameNetwork::CheckInternetAtStart` and `CGameMasterServer::GetMSConnectionAndGameParams` call `CNetHttpClient::CheckInternetConnection` = `InternetCheckConnectionA` (synchronous WinINet); `CNetSystem::GetHost` / `GetAddressFromName` use blocking `gethostbyname`. A dead route, slow DNS or an IPv6 address that does not answer blocks the game thread until the TCP/DNS timeout (typically 20-30 s). Fits "very long", fits less "2nd start fine" (unless DNS is cached the second time).
4. Profile, scores and lightmap cache reads (`ScanDiskForProfilesAndScores`, `ScanDiskForLightMaps`): same pattern as 1, smaller folders.

What tells them apart: `hangs.log` names the function (FidCache/ReplayRecordInfo vs `wininet`/`ws2_32`), and the disk numbers at the stall end (many read operations = 1/2/4; almost none = 3). The launcher's start recorder (no hooks, no Horizon needed) records file activity per folder, `User.FidCache.Gbx` size/time before and after, and whether the window hangs.

Safe fixes that are mine to make: none in game code. On my side: keep the replay folder small (launcher "Replay-Archiv" moves old replays out of `Documents\TrackMania` entirely, undoable), quit the game normally (the cache is saved at quit), and if it is point 3: check DNS/IPv6. A virus scanner exclusion for `Documents\TrackMania` would help points 1/2 but is my decision, not the launcher's. `/noscancache` (a real switch, `LoadAndRefreshFidCache` returns at once) turns the cache **off**, which makes every start read all headers: useful only as a diagnosis ("is it the cache?"), not as a fix.

### Replay list "Refresh"

`CGameCtnMenus::MenuReplay_OnRefresh` -> `ScanDiskForReplays` -> `RefreshFromDisk`: re-lists the whole replay tree (recursive) and builds every entry again, on the game thread. An archive subfolder inside `Tracks\Replays` would still be scanned, so the launcher's archive lives in `Documents\TM Horizon\Replay-Archiv\<game>\`, outside the game's user folder. A Horizon header cache inside the game would need a hook in `RefreshFromDisk` (ESWC/Sunrise allowed, Forever not with Competition Patch); not built before the hang log shows the time is really there.

## DXVK

DXVK 3.1.1 (`x32\d3d9.dll`) works with the Horizon host and this plugin in my offline test: device created through the host's hooks, all 36 counter slots hooked, frame limiter exact (16.668 ms at 60), filter blocked (non-system d3d9) as intended, hang log works. Chaining:

- **TM Vibrant Shaders as standalone `d3d9.dll`** (ESWC, Forever without TMLoader) loads the system `d3d9.dll` by full path (`src/proxy.cpp`), so DXVK cannot sit behind it without a change in TMVS (proposal for TMVS 2.0: load `d3d9.chain.dll` or `dxvk\d3d9.dll` from the game folder first if present). The launcher refuses DXVK when another `d3d9.dll` is in the game folder.
- **TMVS through TMLoader** (`TMVibrantShaders.dll`) hooks whatever `d3d9.dll` the game uses: DXVK in the game folder + TMVS via TMLoader should work (untested).
- **Sunrise repack** has its own `d3d9.dll` proxy: same refusal.
- Competition Patch acceptance of DXVK is **unknown**. Online: DXVK only changes how the image is drawn, not what the game sends.
- Expected gain: mainly on GPUs whose D3D9 driver is weak (Intel Arc runs D3D9 through D3D9On12, see TMVS GOTCHA) and for frame pacing; on a good NVIDIA/AMD D3D9 driver often none. First-time pipeline compiles can stutter. Untested in the game.

## Tests (no game)

`tests\run_perf_tests.cmd` (after `build.cmd` and `tests\build_fake_host.cmd`): 110 unit checks (statistics, limiter maths, state cache, request parsing, CSV/JSON), the real host with the plugin on a mixed-VP device (counting on, screenshot `screens\perf-menu.png`), the same with the state filter (scene checksum identical), the 60 fps limiter (within 4 %), the hang log on a 2.5 s stall, and a benchmark series in the real host (4 sets, one needing a restart: file contents, per-run files, options restored), the series state machine against a fake host (interleaving, restore, restart-only and unavailable sets, pause, abort), and the fake host on ESWC, Sunrise, TMNF and TMUF.

## Alt+Tab and stutter (first ESWC run, 2026-11-11)

What I saw: after Alt+Tab out of ESWC and back the game "stuttered extremely long". The log and `hangs.log` of that start:

- ESWC (windowed, 1280x960) stops drawing while its window is in the background: no `Present` for the whole time. The hang log took that as a hang ("stall: no frame for 1.5 s ... 38.1 s without a frame") and suspended the game thread to sample it, the second sample inside the driver (`NtGdiDdDDIDestroyAllocation2`, i.e. the device Reset after coming back, log "d3d: Reset" at the same second). Nothing else of Horizon runs while no frame comes: the overlay, hotkeys, game poll and events all run inside our `Present` hook; the only threads are the hang watcher (100 ms sleep), the d3d slot check (500 ms sleep) and the key-release watcher (20 ms sleep, one `GetAsyncKeyState`). No busy loop, no timer resolution change (fine timer off by default), no input hook that spins.
- After coming back: one Reset (the game's own), ImGui only drops and rebuilds its font texture and buffers, our textures are MANAGED (survive the Reset). About 1.5 s from the Reset to the first frame, then normal frames ("307 fps, 1 % low 167, 5 stutters").
- The huge stutter numbers ("1332 stutters", "2499 stutters") were 30 s windows with menu and map loading at ~380 fps: frames of 5-10 ms counted as stutters because the rule was only "2x the median".

So Horizon did not cause the stall; the hang log made it worse in the one way it could (suspending the game thread during the Reset after Alt+Tab), and the stutter counter made it look far worse than it was. Changed:

1. Hang log: no stall while the game window is minimised **or in the background**, and no sampling while it is (also when it goes to the background during a stall) or while the device is in a Reset (`before_reset` .. `after_reset`).
2. Stutter counter: frames in the background, with a failed `Present`, in the first second after a Reset / coming back to the front / a state change or map load, and while loading are left out of the live numbers, the log line and benchmark windows; a stutter needs 2x the median and +6 ms.
3. Forever (TMNF/TMUF): the same code; the Forever host only reads game memory in the frame poll (no patches, Competition Patch rule), nothing extra runs in the background. If a game keeps drawing in the background (not checked for TMNF yet), those frames are marked "background" and left out.

Not measured in the game yet: whether the long stutter after coming back is the game's own Reset (likely: 1.5 s Reset to first frame in the log) or something else. Next test: Alt+Tab with the Performance module on, then `hangs.log` (should show no stall for the background time) and the `stats:` lines ("N frames left out").
