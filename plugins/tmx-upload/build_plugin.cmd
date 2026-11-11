@echo off
rem Fragment called by build.cmd (vcvars x86 set, cwd = repo root, %CXX% set, build\imgui_core.lib exists).
set "PD=plugins\tmx-upload"
set "OD=dist\legacy\horizon\plugins\tmx-upload"
if not exist build\plugins\tmx-upload mkdir build\plugins\tmx-upload
if not exist "%OD%" mkdir "%OD%"
%CXX% /Fo"build\plugins\tmx-upload\\" /Fd"build\plugins\tmx-upload\p.pdb" %PD%\tmx_upload.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:"%OD%\tmx-upload.dll" /PDB:build\plugins\tmx-upload\tmx-upload.pdb /IMPLIB:build\plugins\tmx-upload\tmx-upload.lib build\plugins\tmx-upload\*.obj build\imgui_core.lib ws2_32.lib user32.lib kernel32.lib gdi32.lib shell32.lib ole32.lib || exit /b 1
copy /y %PD%\plugin.json "%OD%\plugin.json" >nul || exit /b 1
copy /y %PD%\icon.png "%OD%\icon.png" >nul || exit /b 1
exit /b 0
