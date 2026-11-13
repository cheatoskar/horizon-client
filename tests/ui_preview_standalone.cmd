@echo off
rem Builds and runs the UI preview (no game needed): build\test\ui_preview.exe writes screens\ui-<scene>.png.
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
if not exist "%VCVARS%" goto novc
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0.."
for %%d in (build build\test build\test\imgui build\test\ui screens) do if not exist %%d mkdir %%d

set "DEFS=/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"

rem Dear ImGui (vendored: /W3, no /WX), only rebuilt when missing
if not exist build\test\imgui.lib (
  cl /nologo /c /O2 /MT /W3 /EHsc /std:c++17 /utf-8 %DEFS% /Ithird_party\imgui /Fo"build\test\imgui\\" third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_dx9.cpp third_party\imgui\backends\imgui_impl_win32.cpp || exit /b 1
  lib /nologo /OUT:build\test\imgui.lib build\test\imgui\*.obj || exit /b 1
)

cl /nologo /c /O2 /MT /W4 /WX /EHsc /std:c++17 /utf-8 %DEFS% /Iinclude /Isrc /Ithird_party\imgui /Fo"build\test\ui\\" src\host\ui\ui_style.cpp src\host\ui\ui_menu.cpp src\host\ui\ui_bar.cpp tests\ui_preview.cpp || exit /b 1
link /nologo /MACHINE:X86 /OUT:build\test\ui_preview.exe build\test\ui\*.obj build\test\imgui.lib d3d9.lib user32.lib gdi32.lib ole32.lib windowscodecs.lib || exit /b 1
build\test\ui_preview.exe
exit /b %errorlevel%

:novc
echo Visual Studio Build Tools not found.
exit /b 1
