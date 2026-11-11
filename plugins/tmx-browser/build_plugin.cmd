@echo off
rem Fragment called by build.cmd (vcvars x86 set, cwd = repo root, %CXX% and %DEFS% set, build\imgui_core.lib exists).
rem Needs plugins\client-common\build_plugin.cmd to have run (clienthost.lib for the fake-host test).
rem TMX Browser: the DLL, then its two test programs (build\client\test\tmx_browser_test.exe, tmx_browser_fakehost.exe).
set "PD=plugins\tmx-browser"
set "CK=plugins\client-common"
set "BD=build\plugins\tmx-browser"
set "TD=build\client\test"
set "OD=dist-client\plugins\tmx-browser"
for %%d in ("%BD%" "%TD%" "%TD%\tmxb" dist-client dist-client\plugins "%OD%") do if not exist %%d mkdir %%d
if exist "%BD%\*.obj" del /q "%BD%\*.obj"
if exist "%TD%\tmxb\*.obj" del /q "%TD%\tmxb\*.obj"
%CXX% /I%CK% /I%PD%\src /Fo"%BD%\\" /Fd"%BD%\p.pdb" %PD%\src\tmx_browser.cpp %PD%\src\tmx_logic.cpp || exit /b 1
rem stb_image (not ours: /W3). A private static copy with JPEG, PNG and BMP: the shared ckcommon.lib decoder is PNG only.
cl /nologo /c /O2 /MT /GS /Zi /W3 /EHsc /std:c++17 %DEFS% /I%PD%\src /Ithird_party\stb /Fo"%BD%\\" /Fd"%BD%\p.pdb" %PD%\src\thumb_decode.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:"%BD%\tmx-browser.dll" /PDB:"%BD%\tmx-browser.pdb" /IMPLIB:"%BD%\tmx-browser.lib" %BD%\*.obj build\imgui_core.lib user32.lib kernel32.lib shell32.lib || exit /b 1
copy /y %PD%\plugin.json "%BD%\plugin.json" >nul || exit /b 1
copy /y %PD%\icon.png "%BD%\icon.png" >nul || exit /b 1
copy /y "%BD%\tmx-browser.dll" "%OD%\tmx-browser.dll" >nul || exit /b 1
copy /y %PD%\plugin.json "%OD%\plugin.json" >nul || exit /b 1
copy /y %PD%\icon.png "%OD%\icon.png" >nul || exit /b 1
rem unit tests of the logic
%CXX% /I%CK% /I%PD%\src /Fo"%TD%\tmxb\\" /Fd"%TD%\tmxb\t.pdb" %PD%\tests\tmx_browser_test.cpp || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:"%TD%\tmx_browser_test.exe" /PDB:"%TD%\tmxb\tmx_browser_test.pdb" "%TD%\tmxb\tmx_browser_test.obj" "%BD%\tmx_logic.obj" "%BD%\thumb_decode.obj" user32.lib kernel32.lib || exit /b 1
rem fake-host test: loads the DLL, drives it through HorizonClient_TestHook
%CXX% /I%CK% /I%CK%\tests /I%PD%\src /Fo"%TD%\tmxb\\" /Fd"%TD%\tmxb\t.pdb" %PD%\tests\tmx_browser_fakehost.cpp || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:"%TD%\tmx_browser_fakehost.exe" /PDB:"%TD%\tmxb\tmx_browser_fakehost.pdb" "%TD%\tmxb\tmx_browser_fakehost.obj" "%BD%\tmx_logic.obj" "%BD%\thumb_decode.obj" build\client\clienthost.lib build\imgui_core.lib user32.lib kernel32.lib shell32.lib gdi32.lib || exit /b 1
exit /b 0
