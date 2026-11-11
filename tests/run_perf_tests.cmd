@echo off
rem Horizon Performance tests without a game (run build.cmd and tests\build_fake_host.cmd first):
rem   unit tests; the real host with the plugin on a mixed-VP D3D9 device (counting on), the same with the
rem   state filter (the scene must be pixel-identical), the 60 fps limiter, the hang log on a 2.5 s stall,
rem   and the fake host on all four games. Screens go to screens\ (not committed).
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
if not exist "%VCVARS%" (echo MSVC vcvarsamd64_x86.bat not found & exit /b 1)
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0.."
if not exist build\test\perf mkdir build\test\perf
if not exist screens mkdir screens
set "DEFS=/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"
cl /nologo /c /O2 /MT /GS /Zi /W4 /WX /EHsc /std:c++17 /utf-8 %DEFS% /Iinclude /Isrc /Fo"build\test\perf\\" /Fd"build\test\perf\t.pdb" tests\perf_test.cpp plugins\performance\perf_core.cpp plugins\performance\series.cpp || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:build\test\perf_test.exe build\test\perf\perf_test.obj build\test\perf\perf_core.obj build\test\perf\series.obj user32.lib kernel32.lib ole32.lib windowscodecs.lib || exit /b 1
set FAIL=0
set "P=build\test\perf"

echo === unit
build\test\perf_test.exe unit || set FAIL=1

call :setup count
> %P%\count\horizon\settings\performance.json echo {"count": 1, "overlay": 1, "hang_log": 1}
echo === host + plugin, counting on (screens\perf-menu.png)
build\test\perf_test.exe host "%CD%\%P%\count" "%CD%\%P%\count.txt" "%CD%\screens\perf-menu.png" || set FAIL=1

call :setup filter
> %P%\filter\horizon\settings\performance.json echo {"count": 1, "filter": 1, "overlay": 1}
echo === host + plugin, state filter on
build\test\perf_test.exe host "%CD%\%P%\filter" "%CD%\%P%\filter.txt" || set FAIL=1
build\test\perf_test.exe compare "%CD%\%P%\count.txt" "%CD%\%P%\filter.txt" || set FAIL=1

call :setup limit
> %P%\limit\horizon\settings\performance.json echo {"limit_fps": 60, "overlay": 0}
echo === frame limiter 60 fps
build\test\perf_test.exe host "%CD%\%P%\limit" "%CD%\%P%\limit.txt" || set FAIL=1
build\test\perf_test.exe limit "%CD%\%P%\limit.txt" 60 || set FAIL=1

call :setup stall
> %P%\stall\horizon\settings\performance.json echo {"hang_log": 1, "hang_ms": 1000, "overlay": 0}
echo === hang log: the main thread sleeps 2.5 s
build\test\perf_test.exe host "%CD%\%P%\stall" "%CD%\%P%\stall.txt" stall || set FAIL=1

call :setup series
mkdir %P%\series\horizon\data\performance
> %P%\series\horizon\settings\performance.json echo {"overlay": 0, "hang_log": 0}
> %P%\series\horizon\data\performance\bench-request.json echo {"version": 1, "series": {"mode": "now", "startAfter": 1, "seconds": 1, "settle": 0.3, "runs": 2, "warmup": 1, "sets": [{"label": "baseline"}, {"label": "prio", "options": {"priority": 2}}, {"label": "filt", "options": {"filter": 1}}, {"label": "vs", "options": {"vsync": 1}}]}}
echo === benchmark series: 4 sets (one needs a restart), warm-up + 2 runs, options restored
build\test\perf_test.exe host "%CD%\%P%\series" "%CD%\%P%\series.txt" series || set FAIL=1

echo === bench_report.py (synthetic series)
where python >nul 2>nul && (python tools\test_bench_report.py >nul 2>&1 && echo PASS  bench_report || (echo FAIL  bench_report & set FAIL=1)) || echo SKIP  python not found

echo === fake host, four games
for %%g in (eswc sunrise tmnf tmuf) do (
  build\test\fake_host.exe dist\legacy\horizon\plugins\performance\performance.dll --game %%g >nul 2>&1 && echo PASS  fake host %%g || (echo FAIL  fake host %%g & set FAIL=1)
)
if %FAIL%==0 (echo ALL PERF TESTS PASSED) else (echo SOME PERF TESTS FAILED)
exit /b %FAIL%

:setup
set "T=%P%\%~1"
if exist %T% rmdir /s /q %T%
mkdir %T%\horizon\plugins
mkdir %T%\horizon\settings
copy /y dist\legacy\horizon\host.dll %T%\horizon\ >nul
xcopy /e /i /q /y dist\legacy\horizon\plugins\performance %T%\horizon\plugins\performance >nul
> %T%\horizon\profile.json echo {"format": 1, "profile": "Test", "plugins": [{"id": "performance", "enabled": true}]}
exit /b 0
