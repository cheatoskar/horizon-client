@echo off
rem Renders WR Hunter, Input Display, TMX Browser and the toasts with the client test host (no game):
rem screens\client-<scene>.png (screens\ is not committed). Run build.cmd first.
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
if not exist "%VCVARS%" (echo MSVC vcvarsamd64_x86.bat not found & exit /b 1)
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0..\..\.."
if not exist build\imgui.lib (echo run build.cmd first & exit /b 1)
if not exist build\client\clienthost.lib (echo run build.cmd first & exit /b 1)
set "TD=build\test\client-preview"
for %%d in ("%TD%" screens) do if not exist %%d mkdir %%d
set "DEFS=/DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"
cl /nologo /c /O2 /MT /W4 /WX /EHsc /std:c++17 /utf-8 %DEFS% /Iinclude /Isrc /Ithird_party\imgui /Iplugins\client-common\tests /Fo"%TD%\\" src\host\ui\ui_style.cpp src\host\ui\ui_menu.cpp src\host\ui\ui_bar.cpp src\host\ui\ui_toasts.cpp plugins\client-common\tests\client_preview.cpp || exit /b 1
link /nologo /MACHINE:X86 /OUT:"%TD%\client_preview.exe" "%TD%\*.obj" build\client\clienthost.lib build\imgui.lib d3d9.lib user32.lib gdi32.lib ole32.lib windowscodecs.lib || exit /b 1
"%TD%\client_preview.exe" build\plugins
exit /b %errorlevel%
