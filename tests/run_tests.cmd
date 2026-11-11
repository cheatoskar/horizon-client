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
build\test\host_test.exe loader "%CD%\%T%" || set FAIL=1

echo === host + sample plugin (no profile.json: every plugin loads) + D3D9 device + menu
set "T=build\test\run-host"
if exist %T% rmdir /s /q %T%
mkdir %T%\horizon\plugins\sample
copy /y dist\legacy\horizon\host.dll %T%\horizon\ >nul
copy /y build\plugins\sample\sample.dll %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\plugin.json %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\icon.png %T%\horizon\plugins\sample\ >nul
xcopy /e /i /q /y dist\legacy\horizon\plugins\eswc-autosave %T%\horizon\plugins\eswc-autosave >nul
build\test\host_test.exe host "%CD%\%T%" expect-plugin "%CD%\screens\host-test-menu.png" || set FAIL=1

echo === safe mode: no plugins
set "T=build\test\run-safe"
if exist %T% rmdir /s /q %T%
mkdir %T%\horizon\plugins\sample
copy /y dist\legacy\horizon\host.dll %T%\horizon\ >nul
copy /y build\plugins\sample\sample.dll %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\plugin.json %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\icon.png %T%\horizon\plugins\sample\ >nul
build\test\host_test.exe host "%CD%\%T%" safe expect-no-plugin || set FAIL=1

echo === profile.json without the sample: it stays off
set "T=build\test\run-profile"
if exist %T% rmdir /s /q %T%
mkdir %T%\horizon\plugins\sample
copy /y dist\legacy\horizon\host.dll %T%\horizon\ >nul
copy /y build\plugins\sample\sample.dll %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\plugin.json %T%\horizon\plugins\sample\ >nul
copy /y build\plugins\sample\icon.png %T%\horizon\plugins\sample\ >nul
> %T%\horizon\profile.json echo {"format": 1, "profile": "Test", "plugins": [{"id": "eswc-autosave", "enabled": true}]}
build\test\host_test.exe host "%CD%\%T%" expect-no-plugin || set FAIL=1

echo === plugins against the fake host
call tests\build_fake_host.cmd >nul 2>&1 && echo PASS  fake host (sample + autosave + tmx-upload) || (echo FAIL  fake host & set FAIL=1)

if %FAIL%==0 (echo ALL TESTS PASSED) else (echo SOME TESTS FAILED)
exit /b %FAIL%
