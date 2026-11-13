@echo off
rem Fragment called by build.cmd (vcvars x86 set, cwd = repo root, %CXX% set, build\imgui_core.lib exists).
set "PD=plugins\eswc-autosave"
set "OD=dist\legacy\horizon\plugins\eswc-autosave"
if not exist build\plugins\eswc-autosave mkdir build\plugins\eswc-autosave
if not exist "%OD%" mkdir "%OD%"
%CXX% /Fo"build\plugins\eswc-autosave\\" /Fd"build\plugins\eswc-autosave\p.pdb" %PD%\autosave_plugin.cpp %PD%\tmxcopy.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:"%OD%\eswc-autosave.dll" /PDB:build\plugins\eswc-autosave\eswc-autosave.pdb /IMPLIB:build\plugins\eswc-autosave\eswc-autosave.lib build\plugins\eswc-autosave\*.obj build\imgui_core.lib user32.lib kernel32.lib gdi32.lib shell32.lib ole32.lib || exit /b 1
rem Sunrise/Original fake host test (run: plugins\eswc-autosave\tests\run_tests.cmd); own folder, so the *.obj above stay the DLL's
set "TD=build\plugins\eswc-autosave\test"
if not exist "%TD%" mkdir "%TD%"
%CXX% /Fo"%TD%\\" /Fd"%TD%\t.pdb" %PD%\tests\legacy_fakehost.cpp || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:"%TD%\legacy_fakehost.exe" "%TD%\legacy_fakehost.obj" user32.lib kernel32.lib || exit /b 1
copy /y %PD%\plugin.json "%OD%\plugin.json" >nul || exit /b 1
copy /y %PD%\icon.png "%OD%\icon.png" >nul || exit /b 1
if exist %PD%\icon-alt.png copy /y %PD%\icon-alt.png "%OD%\icon-alt.png" >nul
exit /b 0
