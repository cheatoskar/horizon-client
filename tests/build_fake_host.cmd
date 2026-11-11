@echo off
rem Standalone: builds Dear ImGui (if needed), both plugins and build\test\fake_host.exe, then runs the checks.
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
if not exist "%VCVARS%" (echo MSVC vcvarsamd64_x86.bat not found & exit /b 1)
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0.."
for %%d in (build build\imgui build\test build\plugins) do if not exist %%d mkdir %%d

set "DEFS=/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"
set "CXX=cl /nologo /c /O2 /MT /GS /Zi /W4 /WX /EHsc /std:c++17 /utf-8 %DEFS% /Iinclude /Isrc /Ithird_party\imgui"

if not exist build\imgui.lib (
  cl /nologo /c /O2 /MT /GS /Zi /W3 /EHsc /std:c++17 /utf-8 %DEFS% /Ithird_party\imgui /Fo"build\imgui\\" /Fd"build\imgui\imgui.pdb" third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_dx9.cpp third_party\imgui\backends\imgui_impl_win32.cpp || exit /b 1
  lib /nologo /OUT:build\imgui.lib build\imgui\*.obj || exit /b 1
)

call plugins\eswc-autosave\build_plugin.cmd || exit /b 1
call plugins\sample\build_plugin.cmd || exit /b 1

%CXX% /Fo"build\test\fake_host.obj" /Fd"build\test\fake_host.pdb" tests\fake_host.cpp || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:build\test\fake_host.exe build\test\fake_host.obj build\imgui.lib user32.lib kernel32.lib gdi32.lib shell32.lib || exit /b 1

set "AUTOSAVE=dist\legacy\horizon\plugins\eswc-autosave\eswc-autosave.dll"
set "SAMPLE=build\plugins\sample\sample.dll"
set "FH=build\test\fake_host.exe"
set "FAIL=0"
echo.
echo ===== sample, game eswc
%FH% %SAMPLE% --game eswc || set "FAIL=1"
echo ===== sample, game sunrise
%FH% %SAMPLE% --game sunrise || set "FAIL=1"
echo ===== sample, game tmnf
%FH% %SAMPLE% --game tmnf || set "FAIL=1"
echo ===== eswc-autosave, game eswc (inits, no game pointer)
%FH% %AUTOSAVE% --game eswc || set "FAIL=1"
echo ===== eswc-autosave, game sunrise (refuses with 2)
%FH% %AUTOSAVE% --game sunrise --expect-init 2 || set "FAIL=1"
echo.
if "%FAIL%"=="1" (echo fake host tests FAILED & exit /b 1)
echo fake host tests passed
exit /b 0
