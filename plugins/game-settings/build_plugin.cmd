@echo off
rem Fragment called by build.cmd (vcvars x86 set, cwd = repo root, %CXX% set, build\imgui_core.lib exists).
rem   dist\legacy\horizon\plugins\game-settings\   the Horizon module
rem   build\test\gs_tests.exe                       unit tests
rem The core (gs_game, gs_master) is also used by Legacy Essentials (vendored by its tools\sync-horizon.ps1).
set "PD=plugins\game-settings"
set "BD=build\plugins\game-settings"
set "OD=dist\legacy\horizon\plugins\game-settings"
for %%d in ("%BD%" "%BD%\core" "%BD%\module" "%BD%\tests" "%OD%" build\test) do if not exist %%d mkdir %%d

rem shared code (the game side, the Windows audio session)
%CXX% /Fo"%BD%\core\\" /Fd"%BD%\core\c.pdb" %PD%\gs_game.cpp %PD%\gs_master.cpp || exit /b 1

rem the Horizon module
%CXX% /Fo"%BD%\module\\" /Fd"%BD%\module\m.pdb" %PD%\gs_plugin.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:"%OD%\game-settings.dll" /PDB:"%BD%\game-settings.pdb" /IMPLIB:"%BD%\game-settings.lib" "%BD%\module\*.obj" "%BD%\core\*.obj" build\imgui_core.lib user32.lib kernel32.lib ole32.lib || exit /b 1
copy /y %PD%\plugin.json "%OD%\plugin.json" >nul || exit /b 1
if exist %PD%\icon.png copy /y %PD%\icon.png "%OD%\icon.png" >nul
if exist %PD%\icon-alt.png copy /y %PD%\icon-alt.png "%OD%\icon-alt.png" >nul

rem unit tests
%CXX% /Fo"%BD%\tests\\" /Fd"%BD%\tests\t.pdb" %PD%\tests\gs_tests.cpp || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:build\test\gs_tests.exe "%BD%\tests\gs_tests.obj" "%BD%\core\*.obj" user32.lib kernel32.lib ole32.lib || exit /b 1
exit /b 0
