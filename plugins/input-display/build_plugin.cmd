@echo off
rem Fragment called by build.cmd (after plugins\client-common). Input Display: build\plugins\input-display and dist-client\plugins\input-display.
set "PD=plugins\input-display"
set "OD=build\plugins\input-display"
set "PK=dist-client\plugins\input-display"
for %%d in ("%OD%" "%PK%") do if not exist %%d mkdir %%d
if exist "%OD%\*.obj" del /q "%OD%\*.obj"
%CXX% /Iplugins\client-common /Fo"%OD%\\" /Fd"%OD%\p.pdb" %PD%\input_display.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:"%OD%\input-display.dll" /PDB:"%OD%\input-display.pdb" /IMPLIB:"%OD%\input-display.lib" ^
  "%OD%\input_display.obj" build\client\ckcommon.lib build\imgui_core.lib user32.lib kernel32.lib || exit /b 1
copy /y %PD%\plugin.json "%OD%\plugin.json" >nul || exit /b 1
copy /y %PD%\icon.png "%OD%\icon.png" >nul || exit /b 1
if exist %PD%\icon-alt.png copy /y %PD%\icon-alt.png "%OD%\icon-alt.png" >nul
for %%f in (input-display.dll plugin.json icon.png) do copy /y "%OD%\%%f" "%PK%\%%f" >nul || exit /b 1
set "TD=build\client\test"
%CXX% /Iplugins\client-common /Iplugins\client-common\tests /Fo"%TD%\\" /Fd"%TD%\inp.pdb" %PD%\tests\input_display_fakehost.cpp || exit /b 1
rem large address aware like TMLoader's TmForever.exe: the test puts the car above 2 GB
link /nologo /DEBUG /MACHINE:X86 /LARGEADDRESSAWARE /OUT:"%TD%\input_display_fakehost.exe" "%TD%\input_display_fakehost.obj" build\client\clienthost.lib build\imgui_core.lib user32.lib kernel32.lib || exit /b 1
exit /b 0
