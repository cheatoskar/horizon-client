# Horizon Performance: smoother frames on the old games

Goal: TrackMania Original, Sunrise, Nations ESWC (2006 engine) and Nations/United Forever (`TmForever.exe`) run
noticeably smoother, with OS-level and Present-side changes only (no game logic, no simulation timing, no input
changes; Competition Patch on TMNF/TMUF must stay happy). TM2020 is out of scope.

Status 2026-11-13: static analysis done, switches and the shader cache (2006 engine) built and tested without
the game. **No number in this file comes from the game yet**; the measurement plan is at the end.

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
| 2006 engine: D3DX is linked into the exe (no `d3dx9_*.dll` import; D3DX error strings, HLSL compiler messages). The shader's compile method (ESWC 0x00581220, Sunrise 0x00703620, Original 0x007028a0; called only through two shader vtables, via the thunk ESWC 0x0051aa50; logs "Compilation of EntryPoint ... failed") builds the defines (the shader's own plus `DGbxVersion`) and an include handler, then tries the profiles from the highest the device supports down (vs_1_1..vs_3_0 / ps_1_1..ps_3_0), one `D3DXCompileShader` call per profile until one compiles. On success it copies the bytecode into the shader (ESWC +0xb4 dword count, +0xb8 data), keeps the constant table and releases the error buffer unread. ESWC 0x00580a40 (named the compile function in an earlier version of this file) runs after it: samplers, semantics and constants read from the bytecode; it calls `D3DXGetShaderConstantTable` itself when the shader has no table. | verified (ESWC decompiled; Sunrise and Original compared instruction by instruction: same code, other field offsets) |
| `D3DXCompileShader` (static D3DX, one call site in each exe): ESWC 0x0087aa6b, Sunrise 0x008e3292, Original 0x008dfd8f. `D3DXCreateBuffer`: 0x008a110e / 0x00915f23 / 0x00912a24. `D3DXGetShaderConstantTable`: 0x0087b669 / 0x008e3e90 / 0x008e098d. The three functions are byte-identical across the exes apart from call distances and start with the 5-byte hot-patch prologue `8b ff 55 8b ec`. The compiler makes its constant table with `D3DXGetShaderConstantTable` on its own output buffer. | verified |
| Include handler (ESWC vtable 0x00ae3ea8: Open 0x005803f0, Close 0x00580740; same code in Sunrise 0x00702780 and Original 0x00701a00): system includes (`<Common.VHlsl.txt>`, `<Common.PHlsl.txt>`) return a copy of text the engine holds in memory; local includes (`"..."`) load a file through the engine's file system and add it to the shader's dependency list. | verified |
| The Dx9 viewport installs three callbacks the compile method calls (ESWC writes at 0x007d2f40): before the compile 0x007e9050 (compile flags; returning 0 skips the compile), the profile check 0x007e92d0, after the compile 0x007e93a0 (creates the D3D shader). | verified (writes, call sites), inferred (roles) |
| Textures: `CDx9TextureKeeper::Undirty` -> `UpLoadTexture` uploads on first use; `TextureMemoryMipFree` drops big mip levels when the video-memory budget is exceeded and they come back later. | verified (names), inferred (that this hitches in a race) |
| Map load: `CSystemArchiveNod::LoadResource`, lightmap precalc and resource loading run synchronously on the game thread; scene resources are loaded when a corpus enters the scene (`OnEnterScene` -> `LoadResourceCorpus`) and zones every `RenderFrameBegin` (`LoadResourceZone`). | verified (call sites), inferred (order) |

### What this means for smoothing

1. **Timer**: ESWC and Original run without the 1 ms timer request the other games make. A process-wide
   `timeBeginPeriod(1)` from Horizon restores what the game meant to do. On TMNF/TMUF/Sunrise it changes nothing.
2. **Scheduling**: everything that matters runs on one thread. Pinning that thread to performance cores and
   giving it MMCSS "Games" scheduling targets exactly the code that makes the frame.
3. **GPU wait**: the engine already limits render-ahead (GpuSync); Horizon does not add one.
4. **First-use hitches**: shaders compile when first bound. On ESWC, Sunrise and Original the compile result
   can be kept on disk (section 2, "Shader cache"), so a map compiles nothing the second time. A warm-up at map
   load is not built (it needs engine calls with a valid render state). TMNF/TMUF: nothing (the compile is an
   import Horizon may not hook while the Competition Patch rules apply).
5. Sunrise's measured stutters with Horizon idle (about 700 per 30 s at 75-100 fps) do not come from a sleep in
   the loop (Sunrise has none while the window is in front). Candidates: the GPU wait after `Present`, the
   repack's own `d3d9.dll` (dgVoodoo) present path, and scheduling (the game thread moved between cores or
   parked on an efficiency core). The "game" and "Present" columns of the A/B runs in section 4 separate them:
   long "Present" points at the driver/wrapper or the GPU wait, long "game" at the CPU side.

## 2. Smoothing switches (Horizon Performance)

A section "Smoothing" on the module page, under the Smooth / Low latency / Custom choice. Each switch is
independent of that choice (no preset turns one on), **off by default**, saved in `performance.json`, and
writes one line to the Horizon log when it is applied or removed (`smoothing: ...`). The three scheduling switches
touch no game code, game memory, Direct3D or input: only Windows scheduling and timer calls, so the Competition
Patch rules (TMNF/TMUF) and online play are not affected. The shader cache (2006 engine only) hooks one D3DX
function in the exe; see "Shader cache" below.

| Switch (key) | What it does | Games | Log line |
|---|---|---|---|
| **Pin thread** (`thread_cores`) | `SetThreadSelectedCpuSets` for the game thread (the one calling `Present`) with the CPU sets of the highest `EfficiencyClass` (`GetSystemCpuSetInformation`). A soft pin: Windows prefers these cores but may still use others under load. CPUs with one efficiency class: nothing changes. Off clears the thread's CPU sets. | all | `game thread <id> on performance cores: 8 of 20 CPU sets` / `no hybrid CPU: unchanged` |
| **Timer 1 ms** (`timer_1ms`) | `timeBeginPeriod(1)` while on, `timeEndPeriod(1)` when off or at exit; also switches power throttling off for the process (EcoQoS and Windows 11's "ignore the timer request of a hidden window", the same call as "No power throttling"). | ESWC and Original (they never ask, section 1); on TMNF/TMUF/Sunrise it repeats what the game did | `timer 1 ms on, power throttling off (timer now 1.00 ms)` |
| **MMCSS** (`mmcss`) | `AvSetMmThreadCharacteristicsW(L"Games")` on the game thread (applied from Horizon's `Present` callback, because only the thread itself can join and leave), `AvRevertMmThreadCharacteristics` when off. | all | `MMCSS on for the game thread, task Games` |
| **Shader cache** (`shader_cache`) | Compiled shaders on disk, so a map compiles nothing the second time (below). Not a benchmark-series option: a filled cache cannot be switched off for one window. | ESWC, Sunrise, Original (not shown on TMNF/TMUF) | `shader cache: on for ...`, then per map `shader cache: <map>: N hits, M misses ...` |

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

### Shader cache (2006 engine)

Switch **Shader cache** (`shader_cache`, off by default) in the Smoothing section, only on ESWC, Sunrise and
Original. Files: `horizon\data\performance\shadercache\<eswc|sunrise|original>\<sha256>.bin`, one per compile
result (a few KB each). Deleting the folder clears the cache.

How it works (`plugins/performance/shadercache.cpp`):

1. When the switch is first on, the plugin picks the exe's address table by PE timestamp and compares the start of
   `D3DXCompileShader`, `D3DXCreateBuffer` and `D3DXGetShaderConstantTable` (each up to its first relative call,
   23 to 32 bytes) with the expected bytes. Any difference (another build, or another mod hooked the function):
   one log line `shader cache: off, D3DXCompileShader at 0x0087aa6b has other bytes ...` and nothing is written.
   Then the 5-byte hot-patch prologue of `D3DXCompileShader` becomes a jump; the trampoline runs the same 5 bytes
   and jumps back. The plugin DLL is pinned while the hook exists; switching off makes the hook a pass-through,
   quitting the game puts the 5 bytes back.
2. Every call: key = SHA-256 over the format version, the exe build (game and PE timestamp), the source text, the
   defines (name and value, in order), entry point, profile and flags.
3. **Hit** (file there and valid): the bytecode goes into a buffer from the exe's own `D3DXCreateBuffer`, the
   constant table comes from the exe's own `D3DXGetShaderConstantTable` on that buffer (what the compiler does with
   its output), no error buffer, the stored result code. The engine gets the bytes its compiler made.
4. **Miss**: the normal compile, with the include handler wrapped to see what it opens. Stored when the result is
   valid bytecode (version token matching vs/ps, end token), or a compile error (`E_FAIL`: the profile does not
   fit the shader and the engine tries the next one; the error text is stored and handed back the same way).

Safety checks; on any failure the normal compile runs and the file is written again:

- File: magic, format version (another version is a miss), the key inside equals the key asked for, sizes, a
  SHA-256 of the payload, bytecode tokens and the shader type matching the profile.
- System includes (`<Common.PHlsl.txt>` ...) are opened again through the engine's handler at every hit and
  compared by SHA-256: a mod that changes them makes the entry unusable.
- A compile that opened a local include (`"..."`) is never stored: the engine records that file as a dependency
  of the shader, and a hit would skip it.
- A D3DX call on the hit path fails: nothing is handed out, the normal compile runs.
- Files are written under a temporary name and renamed: no half-written file after a crash.

Log: one line per map, written when the next map loads or the game is back in the menu, e.g.
`shader cache: A01-Race: 0 hits, N misses (N stored), compile T ms, cache 0 ms`; the second run of the same map
should read `N hits, 0 misses (0 stored)` and a cache time far below T. Shaders compiled in the menu and while loading are counted
under `menu and loading`. At most 8 lines per start about single files (`shader cache: 1a2b3c4d5e6f not used (an
include changed), compiled again`). The page shows the current count ("Now: N from cache, 0 compiled").

Tests without the game (`tests\run_perf_tests.cmd`, unit part): keys (every input changes the key), files (round
trip, truncated, damaged byte, wrong magic, other format version, wrong key, not bytecode), and the hook itself on
stand-ins with the same prologue: miss, hit with identical bytes and a new constant table, compile errors, a changed
system include, a local include, damaged and old files, pass-through, the bytes restored, no buffer leaked. Refused
on wrong bytes and on an unmapped address. **Not run in the game yet.**

Not covered: texture uploads on first use, and the driver's own work in `CreateVertexShader`/`CreatePixelShader`
(the bytecode still goes to the driver; usually small).

**Warm-up at map load: not built.** The compile runs when the engine binds a material the first time (the shader
keeper's `Undirty`). Compiling earlier means walking the scene's materials during the loading screen and calling the
engine's bind path with a valid render state; a wrong state can crash the device. With the disk cache the second run
has nothing left to warm up, so the risk buys only the first run of a new map. TMNF/TMUF: nothing of this while the
Competition Patch rules apply (no hooks in the exe or its imports).

### Large address aware (status line, launcher plan)

The games are 32-bit. With the flag `IMAGE_FILE_LARGE_ADDRESS_AWARE` (0x0020 in the PE file header) a 32-bit
process gets 4 GB of address space on 64-bit Windows instead of 2 GB, which avoids out-of-memory crashes with big
texture packs or TMVS. The module page shows **"Large address aware: yes/no"** under "Engine and memory" for the
running exe (read from its PE header in memory), with a one-line tooltip; the address space line next to it shows
how close the game is to the limit (total 2048 MB without the flag, 4096 MB with it). **The plugin never patches the
exe** (it is running, and its hash is what Horizon pins).

The exes (PE headers read 2026-11-13):

| Exe | SHA-256 (start) | `Characteristics` | LAA | PE checksum |
|---|---|---|---|---|
| Nations ESWC | `983bd127` | 0x030f | **no** | 0 (not set) |
| Sunrise | `e59dfc7e` | 0x030f | **no** | 0 (not set) |
| Original | `75e129d7` | 0x030f | **no** | 0 (not set) |
| TMNF / TMUF | `3847cf9f` / `4b6a7b31` | 0x0102 | no | set (0x009f4a9e / 0x009f5c5c) |

0x030f includes `RELOCS_STRIPPED`: the 2006 exes always load at 0x00400000, with or without the flag.

**How the launcher could offer "Enable large address aware"** (ESWC, Sunrise, Original only; per game, off by
default):

1. Only for an exe whose SHA-256 is one of the three above, and only while the game is not running (no process with
   that exe, the file opens for exclusive writing).
2. Backup: copy the exe to `<exe>.horizon-backup` (only if none exists yet) and write a journal entry with both
   hashes, like the launcher's DXVK journal.
3. Patch one bit: `Characteristics |= 0x0020` (in all three exes file offset 0x56, byte 0x0f becomes 0x2f; take the
   offset from `e_lfanew` + 0x16, not from this table). The PE checksum of these exes is 0, which the Windows loader
   does not check for an exe (it checks drivers and a few system DLLs only), so it stays 0. If an exe ever has a
   non-zero checksum, recompute it with `CheckSumMappedFile` (imagehlp) and write it at optional header + 0x40.
4. Verify: read the file back; its SHA-256 must be the patched hash, computed from the known exes:
   ESWC `f73bb0444bca8f72517a4be5a9e0fcf235266c3306ad05e58f223c93e60068b4`, Sunrise
   `9318380da7120c9eea55bb05ddae3c7125b1e554edade75c903f304d791484f5`, Original
   `cf494e57bf5c3db43075e0edb4322c12c86e78a0a5c1e2d5cb1213d5ec3ac068`. Otherwise restore the backup.
5. Undo: put the backup back (its hash must be the original one) and drop the journal entry. Clearing the bit again
   gives the identical original file too (only that byte changed).

Needed before this ships: Horizon's host pins exes by SHA-256 (`src/host/exe_known.h`), so a patched exe would be an
"unknown exe" and Horizon would stay off. The three patched hashes go into that table (same PE timestamp, size and
code). gamectl and the shader cache pick their addresses by PE timestamp and need nothing. Not checked yet: that the
community builds do not verify their own file (no such check seen, not searched for in depth), and a real session
with the flag set.

**TMNF/TMUF are excluded**: TMLoader starts its own `TmForever.exe` copies and the Competition Patch expects known
exes; Horizon does not change those files.

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

**First-lap hitches and the shader cache** (ESWC, Sunrise, Original): switch "Shader cache" on, delete
`horizon\data\performance\shadercache\<game>\`, start the game, play the replay and "Save last 60 s" after the first
60 s; back to the menu (the log gets `shader cache: <map>: 0 hits, N misses`), play the same replay again and save
again (the log should get `N hits, 0 misses`). Then a fresh game start with the cache filled: first pass already
`N hits`. Spikes in the first file that are gone in the later ones = shader compiles; spikes left in every file =
texture uploads or something else.

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
