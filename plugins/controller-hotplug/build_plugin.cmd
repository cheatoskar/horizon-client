@echo off
rem Fragment called by build.cmd (vcvars x86 set, cwd = repo root, %CXX% set, build\imgui_core.lib exists).
rem   dist\legacy\horizon\plugins\controller-hotplug\   the Horizon module
rem   dist\standalone\controller-hotplug\               the standalone build (zip layout, tools\package.py zips it)
rem   build\test\hp_tests.exe                           unit tests
set "PD=plugins\controller-hotplug"
set "BD=build\plugins\controller-hotplug"
set "OD=dist\legacy\horizon\plugins\controller-hotplug"
set "SD=dist\standalone\controller-hotplug"
for %%d in ("%BD%" "%BD%\core" "%BD%\module" "%BD%\standalone" "%BD%\loader" "%BD%\tests" "%OD%" "%SD%" "%SD%\controller-hotplug" build\test) do if not exist %%d mkdir %%d

rem shared code (tracker, game side, worker thread, glue)
%CXX% /Fo"%BD%\core\\" /Fd"%BD%\core\c.pdb" %PD%\hp_tracker.cpp %PD%\hp_enum.cpp %PD%\hp_bind.cpp %PD%\hp_game.cpp %PD%\hp_watch.cpp %PD%\hp_service.cpp || exit /b 1

rem the Horizon module
%CXX% /Fo"%BD%\module\\" /Fd"%BD%\module\m.pdb" %PD%\hotplug_plugin.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:"%OD%\controller-hotplug.dll" /PDB:"%BD%\controller-hotplug.pdb" /IMPLIB:"%BD%\controller-hotplug.lib" "%BD%\module\*.obj" "%BD%\core\*.obj" build\imgui_core.lib user32.lib kernel32.lib dxguid.lib setupapi.lib || exit /b 1
copy /y %PD%\plugin.json "%OD%\plugin.json" >nul || exit /b 1
if exist %PD%\icon.png copy /y %PD%\icon.png "%OD%\icon.png" >nul
if exist %PD%\icon-alt.png copy /y %PD%\icon-alt.png "%OD%\icon-alt.png" >nul

rem standalone: controller_hotplug.dll (same core) + its dinput8.dll loader (no C runtime)
%CXX% /Fo"%BD%\standalone\\" /Fd"%BD%\standalone\s.pdb" %PD%\standalone\standalone.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:"%SD%\controller_hotplug.dll" /PDB:"%BD%\controller_hotplug.pdb" /IMPLIB:"%BD%\controller_hotplug.lib" "%BD%\standalone\*.obj" "%BD%\core\*.obj" user32.lib kernel32.lib dxguid.lib setupapi.lib || exit /b 1
cl /nologo /c /O1 /GS- /W4 /WX /EHs-c- /std:c++17 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /Fo"%BD%\loader\\" %PD%\standalone\loader.cpp || exit /b 1
link /nologo /DLL /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /NODEFAULTLIB /ENTRY:DllMain /DEF:%PD%\standalone\loader.def /OUT:"%SD%\controller-hotplug\dinput8.dll" /IMPLIB:"%BD%\loader\dinput8.lib" "%BD%\loader\loader.obj" user32.lib kernel32.lib || exit /b 1
copy /y %PD%\standalone\README.txt "%SD%\Controller-Hotplug-README.txt" >nul || exit /b 1
copy /y %PD%\standalone\Install.cmd "%SD%\Controller-Hotplug-Install.cmd" >nul || exit /b 1
copy /y %PD%\standalone\Uninstall.cmd "%SD%\Controller-Hotplug-Uninstall.cmd" >nul || exit /b 1
copy /y %PD%\standalone\install.ps1 "%SD%\controller-hotplug\install.ps1" >nul || exit /b 1
if exist "%SD%\controller_hotplug.lib" del "%SD%\controller_hotplug.lib"
if exist "%SD%\controller_hotplug.exp" del "%SD%\controller_hotplug.exp"

rem unit tests
%CXX% /Fo"%BD%\tests\\" /Fd"%BD%\tests\t.pdb" %PD%\tests\hp_tests.cpp || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:build\test\hp_tests.exe "%BD%\tests\hp_tests.obj" "%BD%\core\*.obj" user32.lib kernel32.lib dxguid.lib setupapi.lib || exit /b 1
exit /b 0
