@echo off
rem Fragment called by build.cmd (vcvars x86 set, cwd = repo root, %CXX% set, build\imgui_core.lib exists).
rem The sample goes to build\plugins\sample, not into dist.
set "PD=plugins\sample"
set "OD=build\plugins\sample"
if not exist "%OD%" mkdir "%OD%"
%CXX% /Fo"%OD%\\" /Fd"%OD%\p.pdb" %PD%\sample.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:"%OD%\sample.dll" /PDB:"%OD%\sample.pdb" /IMPLIB:"%OD%\sample.lib" "%OD%\sample.obj" build\imgui_core.lib user32.lib kernel32.lib gdi32.lib || exit /b 1
copy /y %PD%\plugin.json "%OD%\plugin.json" >nul || exit /b 1
copy /y %PD%\icon.png "%OD%\icon.png" >nul || exit /b 1
exit /b 0
