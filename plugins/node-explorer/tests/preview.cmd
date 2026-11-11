@echo off
rem Renders the Node Explorer page in the Horizon menu over the fake engine graph (no game):
rem screens\node-explorer-<scene>.png (screens\ is not committed). Run build.cmd first.
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
if not exist "%VCVARS%" (echo MSVC vcvarsamd64_x86.bat not found & exit /b 1)
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0..\..\.."
if not exist build\imgui.lib (echo run build.cmd first & exit /b 1)
set "PD=plugins\node-explorer"
set "TD=build\test\node-explorer-preview"
for %%d in ("%TD%" screens) do if not exist %%d mkdir %%d
set "DEFS=/DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"
cl /nologo /c /O2 /MT /W4 /WX /EHsc /std:c++17 /utf-8 %DEFS% /Iinclude /Isrc /Ithird_party\imgui /I%PD%\src /I%PD%\tests /Fo"%TD%\\" src\host\ui\ui_style.cpp src\host\ui\ui_menu.cpp src\host\ui\ui_bar.cpp %PD%\tests\explorer_preview.cpp %PD%\tests\fake_graph.cpp %PD%\tests\stub_host.cpp || exit /b 1
link /nologo /MACHINE:X86 /OUT:"%TD%\explorer_preview.exe" "%TD%\*.obj" build\imgui.lib d3d9.lib user32.lib gdi32.lib ole32.lib windowscodecs.lib || exit /b 1
"%TD%\explorer_preview.exe" build\plugins\node-explorer\node-explorer.dll
exit /b %errorlevel%
