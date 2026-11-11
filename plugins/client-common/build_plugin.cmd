@echo off
rem Fragment called by build.cmd (vcvars x86 set, cwd = repo root, %CXX% and %DEFS% set, build\imgui_core.lib exists).
rem Client mods, shared part: build\client\ckcommon.lib (game reads by reflection name, map UID, replay files,
rem the vendored reflect module) and the test host. The plugins (wr-hunter, input-display, tmx-browser) link it
rem and package into dist-client\ only (never dist\).
set "CK=plugins\client-common"
set "OD=build\client"
for %%d in ("%OD%" "%OD%\obj" "%OD%\test" dist-client dist-client\plugins) do if not exist %%d mkdir %%d
if exist "%OD%\obj\*.obj" del /q "%OD%\obj\*.obj"
%CXX% /I%CK% /Fo"%OD%\obj\\" /Fd"%OD%\ck.pdb" %CK%\gameread.cpp %CK%\mapident.cpp %CK%\replayfile.cpp || exit /b 1
rem vendored reflect module (not ours: /W3 like in build.cmd)
cl /nologo /c /O2 /MT /GS /Zi /W3 /EHsc /std:c++17 %DEFS% /Isrc /Fo"%OD%\obj\reflect.obj" /Fd"%OD%\ck.pdb" src\reflect\reflect.cpp || exit /b 1
lib /nologo /OUT:"%OD%\ckcommon.lib" "%OD%\obj\*.obj" || exit /b 1
rem the test host (fake world + fake bridge) as a library for the plugins' tests
if exist "%OD%\test\*.obj" del /q "%OD%\test\*.obj"
%CXX% /I%CK% /I%CK%\tests /Fo"%OD%\test\\" /Fd"%OD%\test\t.pdb" %CK%\tests\client_host.cpp || exit /b 1
lib /nologo /OUT:"%OD%\clienthost.lib" "%OD%\test\client_host.obj" || exit /b 1
exit /b 0
