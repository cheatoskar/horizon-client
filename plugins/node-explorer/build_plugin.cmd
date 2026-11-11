@echo off
rem Fragment called by build.cmd (vcvars x86 set, cwd = repo root, %CXX% and %DEFS% set, build\imgui_core.lib exists).
rem Node Explorer is a developer tool: it builds into build\plugins\node-explorer and is packaged into
rem dist-explorer\node-explorer\ only, never into dist\ (the release zips stay without it).
set "PD=plugins\node-explorer"
set "OD=build\plugins\node-explorer"
set "TD=build\test\node-explorer"
set "PK=dist-explorer\node-explorer"
for %%d in ("%OD%" "%TD%" dist-explorer "%PK%") do if not exist %%d mkdir %%d
%CXX% /I%PD%\src /Fo"%OD%\\" /Fd"%OD%\p.pdb" %PD%\src\*.cpp || exit /b 1
rem vendored reflect module (from legacy-randomizer, not ours: /W3 like in build.cmd)
cl /nologo /c /O2 /MT /GS /Zi /W3 /EHsc /std:c++17 %DEFS% /Isrc /Fo"%OD%\reflect.obj" /Fd"%OD%\p.pdb" src\reflect\reflect.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:"%OD%\node-explorer.dll" /PDB:"%OD%\node-explorer.pdb" /IMPLIB:"%OD%\node-explorer.lib" "%OD%\*.obj" build\imgui_core.lib user32.lib kernel32.lib || exit /b 1
copy /y %PD%\plugin.json "%OD%\plugin.json" >nul || exit /b 1
copy /y %PD%\icon.png "%OD%\icon.png" >nul || exit /b 1

rem package: this folder goes into <game>\horizon\plugins\node-explorer\ of a test copy
copy /y "%OD%\node-explorer.dll" "%PK%\node-explorer.dll" >nul || exit /b 1
copy /y "%OD%\plugin.json" "%PK%\plugin.json" >nul || exit /b 1
copy /y "%OD%\icon.png" "%PK%\icon.png" >nul || exit /b 1

rem tests: unit tests + fake engine graph + the DLL in a fake host (run: plugins\node-explorer\tests\run_tests.cmd)
%CXX% /I%PD%\src /I%PD%\tests /Fo"%TD%\\" /Fd"%TD%\t.pdb" %PD%\tests\explorer_test.cpp %PD%\tests\fake_graph.cpp %PD%\tests\stub_host.cpp %PD%\src\values.cpp %PD%\src\path.cpp %PD%\src\catalog.cpp %PD%\src\safe_read.cpp || exit /b 1
copy /y "%OD%\reflect.obj" "%TD%\reflect.obj" >nul || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:"%TD%\explorer_test.exe" /PDB:"%TD%\explorer_test.pdb" "%TD%\*.obj" build\imgui_core.lib user32.lib kernel32.lib || exit /b 1
exit /b 0
