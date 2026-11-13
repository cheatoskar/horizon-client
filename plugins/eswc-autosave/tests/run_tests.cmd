@echo off
rem Runs the Sunrise/Original fake host test against the built module (build.cmd or tests\build_fake_host.cmd first).
setlocal
cd /d "%~dp0..\..\.."
set "DLL=dist\legacy\horizon\plugins\eswc-autosave\eswc-autosave.dll"
set "EXE=build\plugins\eswc-autosave\test\legacy_fakehost.exe"
if not exist "%DLL%" (echo %DLL% missing, build first & exit /b 1)
if not exist "%EXE%" (echo %EXE% missing, build first & exit /b 1)
"%EXE%" "%DLL%" sunrise original
exit /b %ERRORLEVEL%
