@echo off
rem Standalone: builds Dear ImGui (if needed), the plugins below and build\test\fake_host.exe, then runs the checks.
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
if not exist "%VCVARS%" (echo MSVC vcvarsamd64_x86.bat not found & exit /b 1)
call "%VCVARS%" >nul 2>nul || exit /b 1
rem shipped DLLs name only their PDB file, not the build folder (no user paths in release files)
set "LINK=/PDBALTPATH:%%_PDB%%"
cd /d "%~dp0.."
for %%d in (build build\imgui build\test build\plugins) do if not exist %%d mkdir %%d

rem same defines as build.cmd: the plugins below are rebuilt into dist\legacy, which is what gets packaged
set "DEFS=/DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"
set "CXX=cl /nologo /c /O2 /MT /GS /Zi /W4 /WX /EHsc /std:c++17 /utf-8 %DEFS% /Iinclude /Isrc /Ithird_party\imgui"

if not exist build\imgui.lib (
  cl /nologo /c /O2 /MT /GS /Zi /W3 /EHsc /std:c++17 /utf-8 %DEFS% /Ithird_party\imgui /Fo"build\imgui\\" /Fd"build\imgui\imgui.pdb" third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_dx9.cpp third_party\imgui\backends\imgui_impl_win32.cpp || exit /b 1
  lib /nologo /OUT:build\imgui.lib build\imgui\*.obj || exit /b 1
)
rem the plugins link only the ImGui core; it must exist before the first plugin (a fresh folder has none)
if not exist build\imgui_core.lib (
  lib /nologo /OUT:build\imgui_core.lib build\imgui\imgui.obj build\imgui\imgui_draw.obj build\imgui\imgui_tables.obj build\imgui\imgui_widgets.obj || exit /b 1
)

call plugins\eswc-autosave\build_plugin.cmd || exit /b 1
call plugins\sample\build_plugin.cmd || exit /b 1
call plugins\tmx-upload\build_plugin.cmd || exit /b 1
call plugins\controller-hotplug\build_plugin.cmd || exit /b 1

%CXX% /Fo"build\test\fake_host.obj" /Fd"build\test\fake_host.pdb" tests\fake_host.cpp || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:build\test\fake_host.exe build\test\fake_host.obj build\imgui.lib ws2_32.lib user32.lib kernel32.lib gdi32.lib shell32.lib || exit /b 1

set "AUTOSAVE=dist\legacy\horizon\plugins\eswc-autosave\eswc-autosave.dll"
set "SAMPLE=build\plugins\sample\sample.dll"
set "TMXUP=dist\legacy\horizon\plugins\tmx-upload\tmx-upload.dll"
set "HOTPLUG=dist\legacy\horizon\plugins\controller-hotplug\controller-hotplug.dll"
set "FH=build\test\fake_host.exe"
set "FAIL=0"
echo.
echo ===== sample, game eswc
%FH% %SAMPLE% --game eswc || set "FAIL=1"
echo ===== sample, game sunrise
%FH% %SAMPLE% --game sunrise || set "FAIL=1"
echo ===== sample, game original
%FH% %SAMPLE% --game original || set "FAIL=1"
echo ===== sample, game tmnf
%FH% %SAMPLE% --game tmnf || set "FAIL=1"
echo ===== sample, game tmuf
%FH% %SAMPLE% --game tmuf || set "FAIL=1"
echo ===== eswc-autosave, game eswc (inits, no game pointer)
%FH% %AUTOSAVE% --game eswc --standalone-dummy %SAMPLE% || set "FAIL=1"
echo ===== eswc-autosave, game sunrise with an unknown exe (refuses with 2)
%FH% %AUTOSAVE% --game sunrise --expect-init 2 || set "FAIL=1"
echo ===== eswc-autosave, Sunrise and Original save flow (own fake host with a fake game image)
build\plugins\eswc-autosave\test\legacy_fakehost.exe %AUTOSAVE% sunrise original || set "FAIL=1"
echo ===== tmx-upload, game eswc (real HTTP on 127.0.0.1)
%FH% %TMXUP% --game eswc || set "FAIL=1"
echo ===== eswc-autosave, game original (real sha, no PE timestamp here: refuses with 2)
%FH% %AUTOSAVE% --game original --expect-init 2 || set "FAIL=1"
echo ===== tmx-upload, game original (site tmo)
%FH% %TMXUP% --game original || set "FAIL=1"
echo ===== tmx-upload, game tmnf
%FH% %TMXUP% --game tmnf || set "FAIL=1"
echo ===== controller-hotplug, game eswc (no game here: stays off; test mode: plug, off/on, replug, standalone)
%FH% %HOTPLUG% --game eswc --standalone-dummy %SAMPLE% || set "FAIL=1"
echo ===== controller-hotplug, game sunrise with an unknown exe (refuses with 2)
%FH% %HOTPLUG% --game sunrise --expect-init 2 || set "FAIL=1"
echo ===== controller-hotplug, game original (the real exe's sha, no game code here: inits, stays off)
%FH% %HOTPLUG% --game original || set "FAIL=1"
echo ===== controller-hotplug, game tmnf (refuses with 2)
%FH% %HOTPLUG% --game tmnf --expect-init 2 || set "FAIL=1"
echo.
if "%FAIL%"=="1" (echo fake host tests FAILED & exit /b 1)
echo fake host tests passed
exit /b 0
