@echo off
rem Runs the Node Explorer tests (after build.cmd): unit tests, fake engine graph, the DLL in a fake host.
setlocal
cd /d "%~dp0..\..\.."
build\test\node-explorer\explorer_test.exe build\plugins\node-explorer\node-explorer.dll
exit /b %errorlevel%
