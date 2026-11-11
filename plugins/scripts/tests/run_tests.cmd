@echo off
rem Runs the Horizon Scripts tests (after build.cmd): unit + engine tests, then scripts.dll with both
rem example scripts in a stub host for ESWC, Sunrise, TMNF and TMUF.
setlocal
cd /d "%~dp0..\..\.."
set FAIL=0
echo === scripts: unit and engine tests
build\test\scripts\scripts_test.exe || set FAIL=1
echo === scripts: plugin + example scripts in the fake host
build\test\scripts\scripts_fakehost.exe build\plugins\scripts\scripts.dll plugins\scripts\examples || set FAIL=1
exit /b %FAIL%
