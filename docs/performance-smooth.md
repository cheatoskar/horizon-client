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
   rules apply). Only a plan, see section 2.4.
5. Sunrise's measured stutters with Horizon idle (about 700 per 30 s at 75-100 fps) do not come from a sleep in
   the loop (Sunrise has none while the window is in front). Candidates: the GPU wait after `Present`, the
   repack's own `d3d9.dll` (dgVoodoo) present path, and scheduling (the game thread moved between cores or
   parked on an efficiency core). The "game" and "Present" columns of the A/B runs in section 4 separate them:
   long "Present" points at the driver/wrapper or the GPU wait, long "game" at the CPU side.
