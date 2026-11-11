@echo off
rem Client mods (WR Hunter, Input Display, TMX Browser): unit tests and each DLL in the client test host
rem for ESWC, Sunrise, Original, TMNF and TMUF. Run build.cmd first.
setlocal
cd /d "%~dp0..\..\.."
set FAIL=0
build\client\test\wr_hunter_test.exe || set FAIL=1
build\client\test\wr_hunter_fakehost.exe build\plugins\wr-hunter\wr-hunter.dll || set FAIL=1
build\client\test\input_display_fakehost.exe build\plugins\input-display\input-display.dll || set FAIL=1
call plugins\tmx-browser\tests\run_tests.cmd || set FAIL=1
if %FAIL%==0 (echo client mods: all passed) else (echo client mods: FAILED)
exit /b %FAIL%
