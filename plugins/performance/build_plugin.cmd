@echo off
rem Fragment called by build.cmd (vcvars x86 set, cwd = repo root, %CXX% set, build\imgui_core.lib exists).
set "PD=plugins\performance"
set "BD=build\plugins\performance"
set "OD=dist\legacy\horizon\plugins\performance"
if not exist "%BD%" mkdir "%BD%"
if not exist "%OD%" mkdir "%OD%"
%CXX% /Fo"%BD%\\" /Fd"%BD%\p.pdb" %PD%\perf_plugin.cpp %PD%\perf_core.cpp %PD%\d3d_hooks.cpp %PD%\sysopts.cpp %PD%\stall.cpp %PD%\series.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:"%OD%\performance.dll" /PDB:"%BD%\performance.pdb" /IMPLIB:"%BD%\performance.lib" "%BD%\perf_plugin.obj" "%BD%\perf_core.obj" "%BD%\d3d_hooks.obj" "%BD%\sysopts.obj" "%BD%\stall.obj" "%BD%\series.obj" build\imgui_core.lib user32.lib kernel32.lib gdi32.lib psapi.lib || exit /b 1
copy /y %PD%\plugin.json "%OD%\plugin.json" >nul || exit /b 1
copy /y %PD%\icon.png "%OD%\icon.png" >nul || exit /b 1
if exist %PD%\icon-alt.png copy /y %PD%\icon-alt.png "%OD%\icon-alt.png" >nul
exit /b 0
