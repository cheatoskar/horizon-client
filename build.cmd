@echo off
rem Builds Horizon Host (32-bit, MSVC Build Tools):
rem   dist\legacy\dinput8.dll                        the Legacy Loader (no C runtime, a few KB)
rem   dist\legacy\horizon\host.dll                   the host
rem   dist\legacy\horizon\plugins\<id>\<id>.dll      the plugins (eswc-autosave; sample only with "build.cmd sample")
rem   build\test\*.exe                               unit tests, D3D9 smoke test, UI preview
rem Then tools\package.py makes the TMLoader product and the manifest (build.cmd package).
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
if not exist "%VCVARS%" goto novc
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0"
for %%d in (build build\loader build\host build\imgui build\common build\test build\plugins dist dist\legacy dist\legacy\horizon dist\legacy\horizon\plugins) do if not exist %%d mkdir %%d

set "DEFS=/DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"
set "CXX=cl /nologo /c /O2 /MT /GS /Zi /W4 /WX /EHsc /std:c++17 /utf-8 %DEFS% /Iinclude /Isrc /Ithird_party\imgui"

rem --- Legacy Loader: DirectInput pass-through + loads horizon\host.dll. Nothing else.
cl /nologo /c /O1 /GS- /W4 /WX /EHs-c- /std:c++17 %DEFS% /Iinclude /Fo"build\loader\\" src\loader\loader.cpp || exit /b 1
link /nologo /DLL /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /NODEFAULTLIB /ENTRY:DllMain /DEF:src\loader\loader.def /OUT:dist\legacy\dinput8.dll /IMPLIB:build\loader\dinput8.lib build\loader\loader.obj user32.lib kernel32.lib || exit /b 1

rem --- Dear ImGui (vendored, not ours: no /WX)
cl /nologo /c /O2 /MT /GS /Zi /W3 /EHsc /std:c++17 /utf-8 %DEFS% /Ithird_party\imgui /Fo"build\imgui\\" /Fd"build\imgui\imgui.pdb" third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_dx9.cpp third_party\imgui\backends\imgui_impl_win32.cpp || exit /b 1
lib /nologo /OUT:build\imgui.lib build\imgui\*.obj || exit /b 1
rem plugins link only the core (no Direct3D backend: a plugin never imports d3d9.dll)
lib /nologo /OUT:build\imgui_core.lib build\imgui\imgui.obj build\imgui\imgui_draw.obj build\imgui\imgui_tables.obj build\imgui\imgui_widgets.obj || exit /b 1

rem --- common code shared by host, plugins and tests
%CXX% /Fo"build\common\\" /Fd"build\common\common.pdb" src\common\*.cpp || exit /b 1
lib /nologo /OUT:build\common.lib build\common\*.obj || exit /b 1

rem --- host
%CXX% /Fo"build\host\\" /Fd"build\host\host.pdb" src\host\*.cpp src\host\ui\*.cpp src\host\game\*.cpp || exit /b 1
rem vendored from legacy-randomizer (commit 7482627): reflection, game control, GBX header reader
cl /nologo /c /O2 /MT /GS /Zi /W3 /EHsc /std:c++17 %DEFS% /Isrc /Fo"build\host\\" /Fd"build\host\host.pdb" src\reflect\reflect.cpp src\gamectl\gamectl.cpp src\core\gbx.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:dist\legacy\horizon\host.dll /PDB:build\host.pdb /IMPLIB:build\host_imp.lib build\host\*.obj build\common.lib build\imgui.lib user32.lib kernel32.lib gdi32.lib shell32.lib ole32.lib bcrypt.lib wininet.lib dxguid.lib version.lib psapi.lib || exit /b 1

rem --- plugins: each folder under plugins\ with a build.cmd fragment
for /d %%p in (plugins\*) do (
  if exist "%%p\build_plugin.cmd" (
    call "%%p\build_plugin.cmd" || exit /b 1
  )
)

rem --- tests
if exist tests\build_tests.cmd call tests\build_tests.cmd || exit /b 1

if /i "%1"=="package" python -I tools\package.py || exit /b 1
echo build ok
exit /b 0
:novc
echo MSVC vcvarsamd64_x86.bat not found
exit /b 1
