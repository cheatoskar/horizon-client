@echo off
rem Runs every test without a game (after build.cmd). Screens go to screens\ (not committed).
setlocal
cd /d "%~dp0.."
set FAIL=0
if not exist screens mkdir screens

echo === unit tests
build\test\unit_tests.exe || set FAIL=1

echo === menu input (dim layer, display change)
build\test\ui_input_test.exe || set FAIL=1

echo === controller hot-plug: tracker and game side against a fake input port
build\test\hp_tests.exe || set FAIL=1

echo === game settings: volumes and window mode against a fake game, the Windows audio session
build\test\gs_tests.exe || set FAIL=1

echo === loader + chain + unknown exe
set "T=build\test\run-loader"
if exist %T% rmdir /s /q %T%
mkdir %T%\horizon
copy /y dist\legacy\dinput8.dll %T%\ >nul
copy /y build\test\dinput8.chain.dll %T%\ >nul
copy /y dist\legacy\horizon\host.dll %T%\horizon\ >nul
rem no TM Horizon bridge in tests (it would ask a running TM Horizon on this PC to pair)
mkdir %T%\horizon\settings
> %T%\horizon\settings\host.json echo {"bridge.enabled": 0}
build\test\host_test.exe loader "%CD%\%T%" || set FAIL=1

echo === host + sample plugin (no profile.json: every plugin loads) + D3D9 device + menu + keys typed into the menu
set "T=build\test\run-host"
if exist %T% rmdir /s /q %T%
mkdir %T%\horizon\plugins\sample
copy /y dist\legacy\horizon\host.dll %T%\horizon\ >nul
rem no TM Horizon bridge in tests (it would ask a running TM Horizon on this PC to pair)
mkdir %T%\horizon\settings
> %T%\horizon\settings\host.json echo {"bridge.enabled": 0}
copy /y build\plugins\sample\sample.dll %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\plugin.json %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\icon.png %T%\horizon\plugins\sample\ >nul
xcopy /e /i /q /y dist\legacy\horizon\plugins\eswc-autosave %T%\horizon\plugins\eswc-autosave >nul
rem ABI layout: a plugin built against the 1.2 header loads in this 1.4 host
mkdir %T%\horizon\plugins\abi12
copy /y build\test\abi12\abi12.dll %T%\horizon\plugins\abi12\ >nul
copy /y build\test\abi12\plugin.json %T%\horizon\plugins\abi12\ >nul
build\test\host_test.exe host "%CD%\%T%" expect-plugin expect-abi12 keys "cp=%CD%\build\test\cp\TMCompPatch.dll" "%CD%\screens\host-test-menu.png" || set FAIL=1

echo === modules switched on while the game runs (sample: loaded late, off/on cycles, Reset; abi12: startup_only)
set "T=build\test\run-late"
if exist %T% rmdir /s /q %T%
mkdir %T%\horizon\plugins\sample
copy /y dist\legacy\horizon\host.dll %T%\horizon\ >nul
mkdir %T%\horizon\settings
> %T%\horizon\settings\host.json echo {"bridge.enabled": 0, "plugin.sample.enabled": 0, "plugin.abi12.enabled": 0}
copy /y build\plugins\sample\sample.dll %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\plugin.json %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\icon.png %T%\horizon\plugins\sample\ >nul
mkdir %T%\horizon\plugins\abi12
copy /y build\test\abi12\abi12.dll %T%\horizon\plugins\abi12\ >nul
> %T%\horizon\plugins\abi12\plugin.json echo {"id": "abi12", "name": "ABI 1.2 test", "version": "1.0.0", "dll": "abi12.dll", "online": "online-ok", "abi": 1, "startup_only": true}
build\test\host_test.exe host "%CD%\%T%" runtime-load "%CD%\screens\host-test-late.png" || set FAIL=1

echo === modules that throw while drawing: only they stop, the menu and the game input stay usable
set "T=build\test\run-crash"
if exist %T% rmdir /s /q %T%
mkdir %T%\horizon\plugins\sample
copy /y dist\legacy\horizon\host.dll %T%\horizon\ >nul
mkdir %T%\horizon\settings
> %T%\horizon\settings\host.json echo {"bridge.enabled": 0}
copy /y build\plugins\sample\sample.dll %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\plugin.json %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\icon.png %T%\horizon\plugins\sample\ >nul
for %%M in (menu overlay) do (
  mkdir %T%\horizon\plugins\crash%%M
  copy /y build\test\crash%%M\crash%%M.dll %T%\horizon\plugins\crash%%M\ >nul
  > %T%\horizon\plugins\crash%%M\plugin.json echo {"id": "crash%%M", "name": "Crash %%M", "version": "1.0.0", "dll": "crash%%M.dll", "games": ["eswc", "sunrise", "original", "tmnf", "tmuf"], "online": "online-ok", "abi": 1}
)
build\test\host_test.exe host "%CD%\%T%" expect-plugin crash page=plugin:crashmenu || set FAIL=1

echo === widgets stay on screen and in place: map loads, race, finish, next map, menus, Resets, a smaller window
set "T=build\test\run-widgets"
if exist %T% rmdir /s /q %T%
mkdir %T%\horizon\plugins\sample
copy /y dist\legacy\horizon\host.dll %T%\horizon\ >nul
mkdir %T%\horizon\settings
rem the Clock widget where a 0.3.0 host saved it (pixels, no anchor)
> %T%\horizon\settings\host.json echo {"bridge.enabled": 0, "panel.sample.clock": "1,1,0,0,1,100,1300,40,0,0,0,1.00"}
copy /y build\plugins\sample\sample.dll %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\plugin.json %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\icon.png %T%\horizon\plugins\sample\ >nul
build\test\host_test.exe host "%CD%\%T%" expect-plugin widgets || set FAIL=1

echo === shared setup: the widget from the shared folder (anchored to the right edge), TM Horizon switch per game
set "T=build\test\run-shared"
if exist %T% rmdir /s /q %T%
mkdir %T%\horizon\plugins\sample
mkdir %T%\appdata\shared
copy /y dist\legacy\horizon\host.dll %T%\horizon\ >nul
mkdir %T%\horizon\settings
> %T%\horizon\settings\host.json echo {"bridge.enabled": 0, "setup.shared": 1, "panel.sample.clock": "1,1,0,0,1,100,20,600,0,0,0,1.00"}
> %T%\appdata\shared\host.json echo {"bridge.enabled": 1, "widgets.opacity": 40, "panel.sample.clock": "1,1,0,0,1,100,1300,40,0,0,0,1.00,2,0,0.10000,0.04444"}
copy /y build\plugins\sample\sample.dll %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\plugin.json %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\icon.png %T%\horizon\plugins\sample\ >nul
set "HORIZON_SETUP_DIR=%CD%\%T%\appdata"
build\test\host_test.exe host "%CD%\%T%" expect-plugin widgets-anchored || set FAIL=1
set "HORIZON_SETUP_DIR="

echo === safe mode: no plugins
set "T=build\test\run-safe"
if exist %T% rmdir /s /q %T%
mkdir %T%\horizon\plugins\sample
copy /y dist\legacy\horizon\host.dll %T%\horizon\ >nul
rem no TM Horizon bridge in tests (it would ask a running TM Horizon on this PC to pair)
mkdir %T%\horizon\settings
> %T%\horizon\settings\host.json echo {"bridge.enabled": 0}
copy /y build\plugins\sample\sample.dll %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\plugin.json %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\icon.png %T%\horizon\plugins\sample\ >nul
build\test\host_test.exe host "%CD%\%T%" safe expect-no-plugin || set FAIL=1

echo === profile.json without the sample: it stays off
set "T=build\test\run-profile"
if exist %T% rmdir /s /q %T%
mkdir %T%\horizon\plugins\sample
copy /y dist\legacy\horizon\host.dll %T%\horizon\ >nul
rem no TM Horizon bridge in tests (it would ask a running TM Horizon on this PC to pair)
mkdir %T%\horizon\settings
> %T%\horizon\settings\host.json echo {"bridge.enabled": 0}
copy /y build\plugins\sample\sample.dll %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\plugin.json %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\icon.png %T%\horizon\plugins\sample\ >nul
set "SD=%CD:\=/%/%T:\=/%/stats"
> %T%\horizon\profile.json echo {"format": 1, "profile": "Test", "stats_dir": "%SD%", "plugins": [{"id": "eswc-autosave", "enabled": true}]}
build\test\host_test.exe host "%CD%\%T%" expect-no-plugin || set FAIL=1
rem stats_dir from the profile: the writer starts and stops with the host (no race here, so no lines)
findstr /c:"stats: play events go to" %T%\horizon\logs\*.log >nul && echo PASS  stats writer started || (echo FAIL  stats writer & set FAIL=1)

echo === plugins against the fake host
call tests\build_fake_host.cmd >nul 2>&1 && echo PASS  fake host (sample + autosave + tmx-upload + controller-hotplug) || (echo FAIL  fake host & set FAIL=1)

echo === node explorer: unit tests, fake engine graph, DLL in a fake host
call plugins\node-explorer\tests\run_tests.cmd >nul 2>&1 && echo PASS  node explorer || (echo FAIL  node explorer & set FAIL=1)

echo === horizon scripts: unit and engine tests, both example scripts in a fake host (ESWC, Sunrise, TMNF, TMUF)
call plugins\scripts\tests\run_tests.cmd >nul 2>&1 && echo PASS  horizon scripts || (echo FAIL  horizon scripts & set FAIL=1)

echo === client mods: WR Hunter, Input Display, TMX Browser (ESWC, Sunrise, TMNF, TMUF)
call plugins\client-common\tests\run_tests.cmd >nul 2>&1 && echo PASS  client mods || (echo FAIL  client mods & set FAIL=1)

if %FAIL%==0 (echo ALL TESTS PASSED) else (echo SOME TESTS FAILED)
exit /b %FAIL%
