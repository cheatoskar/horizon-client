# Horizon Performance: smoother frames on the old games

Goal: TrackMania Original, Sunrise, Nations ESWC (2006 engine) and Nations/United Forever (`TmForever.exe`) run
noticeably smoother, with OS-level and Present-side changes only (no game logic, no simulation timing, no input
changes; Competition Patch on TMNF/TMUF must stay happy). TM2020 is out of scope.

Status 2026-11-13: static analysis done, switches built and tested without the game. **No number in this file
comes from the game yet**; the measurement plan is at the end.

## 1. What the games do (static analysis)

Exes (read only; identify yours with `trackmania-internals/scripts/re/pe_info.py`):

| Game | Exe | PE timestamp | SHA-256 (start) |
|---|---|---|---|
| TMNF | `TmForever.exe` 2.11.26 | `4d431494` | `3847cf9f` (my TMNF-test copy; TMLoader's exe has the same `.text`) |
| Nations ESWC | `TmNationsESWC.exe` (unofficial 1.80 community build) | `5ce7272f` | `983bd127` |
| Sunrise | `TmSunrise.exe` (Win10 community repack) | `5cc8f0cf` | `e59dfc7e` |
| Original | `TMORIGINAL.EXE` (Online Patch v2) | `5d337477` | `75e129d7` |

Method: Ghidra 12.1.2 (headless, TMNF with the public `TmForever.map` names) for TMNF; capstone sweeps (import
call sites, string references, vtable-slot calls) for the 2006 exes, matched against the TMNF structure.
"Verified" = seen in the code of that exe; "inferred" = same structure as the verified TMNF code, or a reasoned
consequence. Addresses are link-time VAs (image base 0x00400000). StarForce/DRM code was not looked at.

### Main loop and threads

| Finding | TMNF | ESWC | Sunrise | Original | Status |
|---|---|---|---|---|---|
| One thread runs window messages, game logic, physics and rendering: `WinMain` loops `PeekMessage(PM_REMOVE)`; with no message it calls `CGbxApp::MainLoop` | 0x00529c00 / MainLoop 0x0052a8b0 | loop 0x00426390, MainLoop 0x00425320 | loop 0x0043dff6, MainLoop 0x00411630 | loop 0x0043e1d0, MainLoop 0x00411660 | verified (TMNF decompiled; 2006 loops disassembled, MainLoop has the same call shape) |
| Sleep in the loop | `Sleep(7)` when minimised, `Sleep(20)` when the window is not focused, none in front | **`Sleep(0)` after every frame, no background sleep** (loop rewritten by the community build: the `PeekMessage` call moved into a stub at 0x00426448) | as TMNF (`Sleep(7)` / `Sleep(20)`) | **no sleep at all** (rewritten like ESWC, without the `Sleep(0)`) | verified |
| `CMwCmdBufferCore::Run` (TMNF 0x00922db0): ticks the timer, runs the fixed-period "scheme" command buffers in a catch-up loop (up to 1000 steps per frame; the 100 Hz simulation is one of them), then the "async" buffers once per frame (rendering) | yes | same shape | same shape | same shape | verified TMNF, inferred 2006 |
| "High frequency" commands (input, audio, video) are **not a thread**: `HighFrequencyRun` is called from inside long work on the same thread (mip-map generation, texture upload, lightmap precalc, the GPU wait), throttled by QPC | yes | — | — | — | verified TMNF |
| Other threads | network check, sea scene, Bink video, OpenAL | network, Bink | network | network | verified (CreateThread sites) |

So the thread that calls `Present` is the game thread: render, physics and game logic. Thread-level options apply
to it (Horizon's `Present` callback runs on it).

### Timer

| Finding | Status |
|---|---|
| Game clock `CMwTimer`: QueryPerformanceCounter (`GetCurrentCounter0` via `CMwProfiler::GetTimeStamp`), falling back to `timeGetTime` only if a 90 s calibration (QPC vs `timeGetTime` vs `GetTickCount`, 5 % tolerance) fails (`CMwTimer::Tick` 0x0093a670, `CMwTimer_CalibrateEnd_ShouldSwitchOff` 0x00434100). Same calibration code in ESWC (0x0062a490). | verified TMNF, ESWC |
| `CMwTimerAdapter::Resync` (0x0093a840) pulls the "human" time towards `timeGetTime()` every frame (half the difference). Its granularity is the system timer resolution. | verified TMNF; effect on smoothness inferred |
| `MwSystemTimerInit`: `timeGetDevCaps` + `timeBeginPeriod(wPeriodMin)` (1 ms). **TMNF 0x00939d60 and Sunrise 0x00486d70 call it. ESWC 0x0049bd80 and Original 0x00486dc0 do not**: in both community builds the `push ecx; call [timeBeginPeriod]` bytes are replaced by `xor eax,eax; mov ecx,<IAT slot>` (same for `timeEndPeriod`), so the game believes it succeeded and never raises the timer resolution. | verified |
| Consequence on ESWC/Original: unless some other program asks for a finer timer, `timeGetTime` steps in ~15.6 ms and the adapter's resync jitters with it; `Sleep(n)` rounds up to 15.6 ms. | inferred |

### Present and the GPU

| Finding | TMNF | ESWC | Sunrise | Original | Status |
|---|---|---|---|---|---|
| `RenderFrameEnd`: `EndScene`, then `Present(0,0,0,0)` on the device | 0x009a4704 (`call edx`, slot 17) | 0x007e4632 | 0x006cf4d2 | 0x006ce822 | verified |
| 2006 engine times `Present` with QPC and logs `[Dx9] D3dDevice->Present() has taken N ms !` above 50 ms | — | yes | yes | yes | verified |
| **GPU sync after Present**: `SyncGpuIssueQuery` puts an event query (or, without queries, a `ColorFill` + lock of a 16x16 surface) into a ring; `SyncGpuWaitQuery` polls the query from N frames ago with `GetData(FLUSH)`, `SleepEx(0)`/`Sleep(0)` and a high-frequency run, logging `[Dx9] Cpu/Gpu Sync not responding after N ms` after 200 ms. Ring size = the `GpuSync` frame count, the wait itself is switched by the viewport flag (`CSystemConfigDisplay.GpuSync`). | 0x0098b780 / 0x0098b890 | 0x007f1ec0 / 0x007f1fd0 | 0x006dca20 / 0x006dcb30 | 0x006dbdb0 / 0x006dbec0 | verified (TMNF decompiled; ESWC and Sunrise disassembled; Original by the same call order after `Present`) |
| Effect: with GpuSync on, the CPU never runs more than N frames ahead of the GPU (lower input delay, less CPU/GPU overlap). A second render-ahead limiter would duplicate it. | | | | | inferred |
| Device: mixed vertex processing, not multithreaded (TMNF, TMVS notes). | | | | | verified TMNF |

### Shaders and textures created during a race (first-use hitches)

| Finding | Status |
|---|---|
| TMNF compiles HLSL at run time with `d3dx9_30!D3DXCompileShader` (one call site, `CompileInternal` 0x008deaf0). `CPlugGpuCompileCache::CacheOrCompileHR` (0x008deff0) is no cache in this build: it always compiles. | verified |
| The compile runs when a shader is **bound the first time**: `CDx9ShaderKeeper::Undirty` (virtual, 0x00970a60) and `CDx9PixelShader::UndirtyAndSetPixelShader` call `CDx9PixelShader::Compile`. Nothing walks all materials at map load to compile them. | verified (call chain); hitch size inferred |
| 2006 engine: D3DX is linked into the exe (D3DX error strings, HLSL compiler messages, shader profile table at ESWC 0x0073cac8); the compile function (ESWC 0x00580a40, log "Compilation of EntryPoint ...") is reached from a large virtual function next to the Dx9 viewport code (0x007e8ce0), the same shape as TMNF's `Undirty`. | verified (strings, call), inferred (that it is the bind-time path) |
| Textures: `CDx9TextureKeeper::Undirty` -> `UpLoadTexture` uploads on first use; `TextureMemoryMipFree` drops big mip levels when the video-memory budget is exceeded and they come back later. | verified (names), inferred (that this hitches in a race) |
| Map load: `CSystemArchiveNod::LoadResource`, lightmap precalc and resource loading run synchronously on the game thread; scene resources are loaded when a corpus enters the scene (`OnEnterScene` -> `LoadResourceCorpus`) and zones every `RenderFrameBegin` (`LoadResourceZone`). | verified (call sites), inferred (order) |

### What this means for smoothing

1. **Timer**: ESWC and Original run without the 1 ms timer request the other games make. A process-wide
   `timeBeginPeriod(1)` from Horizon restores what the game meant to do. On TMNF/TMUF/Sunrise it changes nothing.
2. **Scheduling**: everything that matters runs on one thread. Pinning that thread to performance cores and
   giving it MMCSS "Games" scheduling targets exactly the code that makes the frame.
3. **GPU wait**: the engine already limits render-ahead (GpuSync); Horizon does not add one.
4. **First-use hitches**: shaders compile when first bound. A safe warm-up from outside the engine is not
   possible: the compile is engine code (and on TMNF an import Horizon may not hook while the Competition Patch
   rules apply). Only a plan, see section 2, "Shader and texture warm-up".
5. Sunrise's measured stutters with Horizon idle (about 700 per 30 s at 75-100 fps) do not come from a sleep in
   the loop (Sunrise has none while the window is in front). Candidates: the GPU wait after `Present`, the
   repack's own `d3d9.dll` (dgVoodoo) present path, and scheduling (the game thread moved between cores or
   parked on an efficiency core). The "game" and "Present" columns of the A/B runs in section 4 separate them:
   long "Present" points at the driver/wrapper or the GPU wait, long "game" at the CPU side.

## 2. Smoothing switches (Horizon Performance)

A section "Smoothing" on the module page, under the Smooth / Low latency / Custom choice. Each switch is
independent of that choice (no preset turns one on), **off by default**, saved in `performance.json`, and
writes one line to the Horizon log when it is applied or removed (`smoothing: ...`). Nothing of it touches game
code, game memory, Direct3D or input: only Windows scheduling and timer calls, so the Competition Patch rules
(TMNF/TMUF) and online play are not affected.

| Switch (key) | What it does | Games | Log line |
|---|---|---|---|
| **Pin thread** (`thread_cores`) | `SetThreadSelectedCpuSets` for the game thread (the one calling `Present`) with the CPU sets of the highest `EfficiencyClass` (`GetSystemCpuSetInformation`). A soft pin: Windows prefers these cores but may still use others under load. CPUs with one efficiency class: nothing changes. Off clears the thread's CPU sets. | all | `game thread <id> on performance cores: 8 of 20 CPU sets` / `no hybrid CPU: unchanged` |
| **Timer 1 ms** (`timer_1ms`) | `timeBeginPeriod(1)` while on, `timeEndPeriod(1)` when off or at exit; also switches power throttling off for the process (EcoQoS and Windows 11's "ignore the timer request of a hidden window", the same call as "No power throttling"). | ESWC and Original (they never ask, section 1); on TMNF/TMUF/Sunrise it repeats what the game did | `timer 1 ms on, power throttling off (timer now 1.00 ms)` |
| **MMCSS** (`mmcss`) | `AvSetMmThreadCharacteristicsW(L"Games")` on the game thread (applied from Horizon's `Present` callback, because only the thread itself can join and leave), `AvRevertMmThreadCharacteristics` when off. | all | `MMCSS on for the game thread, task Games` |

Notes:
- **Pin thread vs. "CPU cores: P-cores"** (Custom): the older option sets the affinity of the whole process (hard,
  all threads incl. audio and network). Pin thread moves only the game thread and leaves the rest free. Both on:
  the affinity mask wins where they disagree.
- **MMCSS vs. Priority**: no conflict. MMCSS raises only the game thread, inside the MMCSS band, and Windows keeps
  a share of CPU time for everything else (`SystemResponsiveness`, 20 % by default). The process priority then
  only matters for the game's other threads. MMCSS "Games" also asks for a 1 ms timer while the thread runs
  (registry `Clock Rate`, inferred from the Windows defaults, not measured).
- **Turning the module off** takes all three back. If that happens on another thread than the game thread (not the
  case for the menu, which runs inside `Present`), MMCSS stays on the game thread until it ends; the log says so.
- Benchmark series: the switches are options of a set (`threadCores`, `timer1ms`, `mmcss`), so the series can
  measure them A/B; the result files list them under `options`.
- Tests without the game: unit tests (request parsing, presets keep the switches), and the real host with the
  plugin and all three switches on (`tests\run_perf_tests.cmd`, "smoothing switches": the three log lines). My
  test PC has no hybrid CPU, so the pin itself has not run on one yet.

### Shader and texture warm-up (plan only, not built)

Why not now: the stall is in the engine's own compile (`D3DXCompileShader` on TMNF, the D3DX compiler linked into
the 2006 exes), which runs when a material is bound the first time. Nothing at the Direct3D level can do that
work earlier: `CreatePixelShader` only receives the finished bytecode. Warming up therefore needs engine calls or
a hook in the compile path:

1. **2006 engine (ESWC/Sunrise/Original, no Competition Patch)**: hook the exe's compile function (ESWC
   0x00580a40) and keep a disk cache `horizon\data\performance\shaders\<sha1 of source+defines+target>.bin` of
   the compiled bytecode and constant table. Second start onward: no compile at all. Risk: medium (inline hook
   per exe build, constant-table object must be rebuilt from the cache). Gain: only the first-bind hitches, which
   have not been measured yet.
2. **Warm-up at map load**: during the loading screen, walk the scene's materials (`CHmsViewport` corpus list,
   `CDx9ShaderKeeper`) and call the keeper's `Undirty` once each. Needs engine calls with the right render state;
   a wrong state can crash the device. Not before (1) and a measurement that shows the hitches.
3. **TMNF/TMUF**: none of this while the Competition Patch rules apply (no hooks in the exe or its imports).

First step instead: measure. The hang log and the frame-time CSV show first-lap spikes; the plan in section 4
compares lap 1 with lap 2 of the same route. If lap 2 is clean and lap 1 is not, (1) is worth building.

## 3. DXVK (feasibility, doc only)

DXVK's 32-bit `d3d9.dll` (from the official release archive, `x32\d3d9.dll`) translates Direct3D 9 to Vulkan. The
games import `d3d9.dll` by name and it is not a KnownDLL, so a `d3d9.dll` next to the exe is loaded instead of the
system one. Horizon's host hooks the factory of whatever `d3d9.dll` the game uses (`src/host/d3dhook.cpp`), so
Horizon and this module work on top of DXVK (offline test with DXVK 3.1.1, `docs/performance.md`, "DXVK"). Latest
release when I checked: v3.1.1 (2026-09-15). DXVK 2.x and newer need Vulkan 1.3; older GPUs would need DXVK 1.10.3.

### Load order per game

| Game | Today's chain | With DXVK, no TMVS | With DXVK and TMVS standalone (`d3d9.dll`) | With TMVS via TMLoader |
|---|---|---|---|---|
| ESWC | exe -> system `d3d9.dll` (my test folder has no wrapper) | DXVK as `d3d9.dll` in the game folder | TMVS as `d3d9.dll`, DXVK renamed to **`d3d9.chain.dll`**: TMVS loads `d3d9.chain.dll` from its own folder when it exports `Direct3DCreate9` and is not a TMVS copy (TMVS `src/proxy.cpp` `loadChain`, branch `feat/legacy-games`) | — |
| Sunrise (repack) | exe -> the repack's own `d3d9.dll` proxy | DXVK **replaces** the repack's proxy (kept as a backup; whatever it fixes is lost) | TMVS `d3d9.dll` -> `d3d9.chain.dll`: today the repack's proxy sits there; DXVK would take its place | — |
| Original (Online Patch v2) | exe -> dgVoodoo 2.8.2 `d3d9.dll` (+ `DDraw.dll`, `dgVoodoo.conf`) | DXVK replaces dgVoodoo's `d3d9.dll`; `DDraw.dll` stays dgVoodoo | as Sunrise: DXVK as `d3d9.chain.dll` instead of dgVoodoo | — |
| TMNF / TMUF | exe -> system `d3d9.dll` | DXVK as `d3d9.dll` next to the `TmForever.exe` that actually starts (with TMLoader its own copy, not the game install) | TMVS standalone `d3d9.dll` + DXVK `d3d9.chain.dll` | TMVS hooks whatever `d3d9.dll` the game loads: DXVK as `d3d9.dll`, TMVS as a TMLoader mod |

Horizon is the same in every row: the host's factory hook sits in front of the first `d3d9.dll` (game -> Horizon
hooks -> TMVS -> DXVK). Keep `d3dx9_30.dll` (TMNF/TMUF): it is a separate helper DLL the game needs for its shader
compiler.

### Known issues (DXVK tracker, read 2026-11-13; no TrackMania entry in DXVK's built-in app profiles)

| Issue | Game | What it says |
|---|---|---|
| doitsujin/dxvk#5500 (closed 2026-04) | Nations ESWC | Broken menu UI and a crash at the start of the race with DXVK. The maintainer replayed the trace on the native NVIDIA D3D9 driver and saw the same picture, so the `d3d9.dll` bundled with that ESWC build contains a workaround DXVK does not have. Expect the same where a community build ships a wrapper (Sunrise repack, Original's dgVoodoo). |
| doitsujin/dxvk#5985 (closed 2026-10) | TMNF | Black menu/tracks with Shader Quality PC3High: caused by deleting `d3dx9_30.dll` (Wine's fallback compiler), not by DXVK. |
| doitsujin/dxvk#1910 (closed 2021) | TMNF | Very low FPS under Wine: the game creates a **mixed vertex processing** device and DXVK emulates software vertex processing; the `d3d9.swvp*Count` workaround then broke ghost colours. This module's call counter shows "software VP draws" per frame. |
| doitsujin/dxvk#3283 (closed 2023) | TMUF | Performance regression, fixed. #1278, #1375, #1376, #1377, #1640, #1642 (2019-2020, TMUF/TMNF crashes and rendering bugs) are closed. |

Assessment: on Windows with a good NVIDIA/AMD D3D9 driver expect little or nothing; possible gains on Intel Arc
(D3D9 runs through D3D9On12) and for frame pacing, against first-time pipeline-compile stutter. For Sunrise and
Original the fair comparison is DXVK vs. dgVoodoo/the repack proxy, and dgVoodoo's own settings (output API,
frame latency in `dgVoodoo.conf`) are a cheaper first knob than swapping it out. Competition Patch acceptance of
DXVK on TMNF/TMUF is unknown (CP 2.0 checks runs server-side; nothing public about renderers): test offline first.

### What the launcher needs

The launcher already has an experimental per-game DXVK switch (`tm-hub-mono` `apps/launcher/docs/optimize.md`,
"DXVK (Vulkan)": DXVK 3.1.1 pinned, archive SHA-256 checked, only `x32/d3d9.dll` extracted, `dxvk.conf` with
`d3d9.maxFrameLatency = 1`, a journal, exact removal). What changes with this analysis:

1. **Chaining instead of refusing.** It refuses when a `d3d9.dll` is already there because "TMVS loads the system
   d3d9 by full path". TMVS now loads `d3d9.chain.dll` first, so: TMVS standalone present -> write DXVK as
   `d3d9.chain.dll` (if that name is free). Sunrise/Original: the slot is taken by the repack proxy/dgVoodoo ->
   offer "replace" with the old file kept under another name in the journal, never deleted.
2. **Per game, default off**, labelled experimental; ESWC with a warning (issue #5500).
3. **Download only from the official GitHub release** (`https://github.com/doitsujin/dxvk/releases/download/v<ver>/dxvk-<ver>.tar.gz`),
   version and archive SHA-256 pinned in the launcher, no mirrors or file hosters; nothing written when the hash
   differs.
4. `dxvk.conf` next to the exe: `d3d9.maxFrameLatency = 1` keeps the engine's own GPU sync meaningful; nothing else
   until measured.
5. TMNF/TMUF: put the files next to the exe TMLoader starts, and show "Competition Patch: unknown" next to the
   switch.

## 4. Measurement plan (for my tests)

Nothing above is worth keeping without numbers. One scene per game, same camera, 60 s windows, every switch
against the same baseline in the same game start.

**Scene:** a replay of one lap, watched with the same camera (replays give the same route every time; in map mode
the car only stands at the start). One map per game:

| Game | Map | Replay |
|---|---|---|
| TMNF | A01-Race | my own driving, at least 120 s (the series needs twice the window length) |
| TMUF | StarStadiumB3 | same |
| ESWC | A01 | same |
| Sunrise | first Island map | same |
| Original | first Desert map | same |

**Smoothing switches:** the module's benchmark series (one game start, no clicking):
`tools/bench_series.ps1 -GamePath <game> -Smooth -Seconds 60` (ESWC, Sunrise, Original) or
`tools/bench_series.ps1 -Forever -Smooth -Seconds 60` (TMNF/TMUF), then open the replay; press Ctrl+Shift+B if it
does not start by itself. Sets: `baseline`, `pin-thread`, `timer-1ms`, `mmcss`, `all-three`, 1 warm-up + 3 runs,
interleaved order (about 21 min). Result: `python tools/bench_report.py <stamp>_series.json`.

**DXVK** cannot switch at run time: two kinds of start, same replay, each a manual 60 s benchmark (Ctrl+Shift+B,
label `native` / `dxvk`), three starts each, alternating (native, dxvk, native, ...). Optional: PresentMon (not
installed here; official console build from GameTechDev/PresentMon) for the present-to-display view the module
cannot see: `PresentMon --process_name <exe> --timed 60 --output_file <label>.csv`.

**First-lap hitches** (decides whether the shader cache plan is worth building): baseline, fresh game start, play
the replay and "Save last 60 s" after the first 60 s and again after the second pass of the same 60 s. Spikes only
in the first file = first-use compiles/uploads.

**Before each series:** game window in front, same resolution and game settings, nothing else running (a browser
asks for a 1 ms timer itself and hides the timer effect), laptop on mains power. Note CPU (hybrid or not), GPU,
driver.

**Table** (one per game; FPS, lows and stutters from `bench_report.py` with the difference to baseline in brackets;
"Present ms" and "game ms" are `presentMsAvg` and `cpuMsAvg` of the run files, mean of the runs):

| Set | avg FPS | median ms | 1 % low | 0.1 % low | stutters/min | Present ms | game ms | note |
|---|---|---|---|---|---|---|---|---|
| baseline | | | | | | | | |
| pin-thread | | | | | | | | hybrid CPU? |
| timer-1ms | | | | | | | | timer before: |
| mmcss | | | | | | | | |
| all-three | | | | | | | | |
| native (manual) | | | | | | | | |
| dxvk (manual) | | | | | | | | DXVK version |

A switch counts as a win only if its difference is larger than the baseline's own spread (the report's hint
column) in 1 % low or stutters/min, on two different days.
