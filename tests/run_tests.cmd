@echo off
rem Runs every test without a game (after build.cmd). Screens go to screens\ (not committed).
setlocal
cd /d "%~dp0.."
set FAIL=0
if not exist screens mkdir screens

echo === unit tests
build\test\unit_tests.exe || set FAIL=1

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

echo === host + sample plugin (no profile.json: every plugin loads) + D3D9 device + menu
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
rem ABI layout: a plugin built against the 1.2 header loads in this 1.3 host
mkdir %T%\horizon\plugins\abi12
copy /y build\test\abi12\abi12.dll %T%\horizon\plugins\abi12\ >nul
copy /y build\test\abi12\plugin.json %T%\horizon\plugins\abi12\ >nul
build\test\host_test.exe host "%CD%\%T%" expect-plugin expect-abi12 "%CD%\screens\host-test-menu.png" || set FAIL=1

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
call tests\build_fake_host.cmd >nul 2>&1 && echo PASS  fake host (sample + autosave + tmx-upload) || (echo FAIL  fake host & set FAIL=1)

echo === node explorer: unit tests, fake engine graph, DLL in a fake host
call plugins\node-explorer\tests\run_tests.cmd >nul 2>&1 && echo PASS  node explorer || (echo FAIL  node explorer & set FAIL=1)

echo === horizon scripts: unit and engine tests, both example scripts in a fake host (ESWC, Sunrise, TMNF, TMUF)
call plugins\scripts\tests\run_tests.cmd >nul 2>&1 && echo PASS  horizon scripts || (echo FAIL  horizon scripts & set FAIL=1)

echo === client mods: WR Hunter, Input Display, TMX Browser (ESWC, Sunrise, TMNF, TMUF)
call plugins\client-common\tests\run_tests.cmd >nul 2>&1 && echo PASS  client mods || (echo FAIL  client mods & set FAIL=1)

if %FAIL%==0 (echo ALL TESTS PASSED) else (echo SOME TESTS FAILED)
exit /b %FAIL%
