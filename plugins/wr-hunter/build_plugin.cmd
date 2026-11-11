@echo off
rem Fragment called by build.cmd (after plugins\client-common). WR Hunter: build\plugins\wr-hunter and dist-client\plugins\wr-hunter.
set "PD=plugins\wr-hunter"
set "OD=build\plugins\wr-hunter"
set "PK=dist-client\plugins\wr-hunter"
for %%d in ("%OD%" "%PK%") do if not exist %%d mkdir %%d
if exist "%OD%\*.obj" del /q "%OD%\*.obj"
%CXX% /Iplugins\client-common /I%PD% /Fo"%OD%\\" /Fd"%OD%\p.pdb" %PD%\wr_hunter.cpp %PD%\src\splits.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:"%OD%\wr-hunter.dll" /PDB:"%OD%\wr-hunter.pdb" /IMPLIB:"%OD%\wr-hunter.lib" ^
  "%OD%\wr_hunter.obj" "%OD%\splits.obj" build\client\ckcommon.lib build\common.lib build\imgui_core.lib bcrypt.lib user32.lib kernel32.lib shell32.lib || exit /b 1
copy /y %PD%\plugin.json "%OD%\plugin.json" >nul || exit /b 1
copy /y %PD%\icon.png "%OD%\icon.png" >nul || exit /b 1
for %%f in (wr-hunter.dll plugin.json icon.png) do copy /y "%OD%\%%f" "%PK%\%%f" >nul || exit /b 1
rem tests (run: plugins\wr-hunter\tests\run_tests.cmd)
set "TD=build\client\test"
%CXX% /Iplugins\client-common /Iplugins\client-common\tests /I%PD% /Fo"%TD%\\" /Fd"%TD%\wrh.pdb" %PD%\tests\wr_hunter_test.cpp %PD%\tests\wr_hunter_fakehost.cpp || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:"%TD%\wr_hunter_test.exe" "%TD%\wr_hunter_test.obj" "%OD%\splits.obj" build\client\ckcommon.lib build\common.lib bcrypt.lib user32.lib kernel32.lib || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:"%TD%\wr_hunter_fakehost.exe" "%TD%\wr_hunter_fakehost.obj" build\client\clienthost.lib build\imgui_core.lib user32.lib kernel32.lib || exit /b 1
exit /b 0
