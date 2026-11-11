@echo off
rem Fragment called by build.cmd (vcvars x86 set, cwd = repo root, %CXX% and %DEFS% set, build\imgui_core.lib exists).
rem Horizon Scripts: the AngelScript script engine as a Horizon plugin. Builds into build\plugins\scripts and is
rem packaged into dist-scripts\ (plugin folder + example scripts) only, never into dist\ (the release zips stay
rem without it until it is tested in the games).
set "PD=plugins\scripts"
set "OD=build\plugins\scripts"
set "CD=build\plugins\scripts\core"
set "AD=build\angelscript"
set "TD=build\test\scripts"
set "PK=dist-scripts"
for %%d in ("%OD%" "%CD%" "%AD%" "%TD%" "%PK%" "%PK%\plugins" "%PK%\plugins\scripts" "%PK%\scripts") do if not exist %%d mkdir %%d

rem --- AngelScript 2.38.0 (vendored, zlib licence, not ours: /W3, no /WX). Built once; delete build\angelscript.lib to rebuild.
if not exist build\angelscript.lib (
  cl /nologo /c /O2 /MT /GS /Zi /W3 /EHsc /std:c++17 /MP %DEFS% /Ithird_party\angelscript\include /Fd"%AD%\as.pdb" /Fo"%AD%\\" ^
    third_party\angelscript\source\*.cpp third_party\angelscript\add_on\scriptarray\scriptarray.cpp ^
    third_party\angelscript\add_on\scriptbuilder\scriptbuilder.cpp third_party\angelscript\add_on\scriptstdstring\scriptstdstring.cpp ^
    third_party\angelscript\add_on\scriptstdstring\scriptstdstring_utils.cpp third_party\angelscript\add_on\scriptmath\scriptmath.cpp || exit /b 1
  lib /nologo /OUT:build\angelscript.lib "%AD%\*.obj" || exit /b 1
)

rem our code: /W4 /WX from %CXX%; AngelScript's headers are external (their warnings are not ours)
set "SINC=/I%PD%\src /Iplugins\node-explorer\src /external:W0 /external:Ithird_party\angelscript\include /external:Ithird_party\angelscript\add_on"
if exist "%CD%\*.obj" del /q "%CD%\*.obj"
%CXX% /MP %SINC% /Fo"%CD%\\" /Fd"%OD%\p.pdb" %PD%\src\*.cpp || exit /b 1
rem the Node Explorer's read layer (safe reads, class catalog, paths, value types): reused as is
%CXX% /MP %SINC% /Fo"%CD%\\" /Fd"%OD%\p.pdb" plugins\node-explorer\src\safe_read.cpp plugins\node-explorer\src\catalog.cpp plugins\node-explorer\src\path.cpp plugins\node-explorer\src\values.cpp || exit /b 1
rem vendored reflect module (from legacy-randomizer, not ours: /W3 like in build.cmd)
cl /nologo /c /O2 /MT /GS /Zi /W3 /EHsc /std:c++17 %DEFS% /Isrc /Fo"%CD%\reflect.obj" /Fd"%OD%\p.pdb" src\reflect\reflect.cpp || exit /b 1
lib /nologo /OUT:"%OD%\scripts_core.lib" "%CD%\*.obj" || exit /b 1

%CXX% %SINC% /Fo"%OD%\plugin_main.obj" /Fd"%OD%\p.pdb" %PD%\plugin_main.cpp || exit /b 1
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:"%OD%\scripts.dll" /PDB:"%OD%\scripts.pdb" /IMPLIB:"%OD%\scripts.lib" ^
  "%OD%\plugin_main.obj" "%OD%\scripts_core.lib" build\angelscript.lib build\imgui_core.lib user32.lib kernel32.lib shell32.lib || exit /b 1
copy /y %PD%\plugin.json "%OD%\plugin.json" >nul || exit /b 1
copy /y %PD%\icon.png "%OD%\icon.png" >nul || exit /b 1

rem --- package: dist-scripts\plugins\scripts\ goes into <game>\horizon\plugins\scripts\, dist-scripts\scripts\* into <game>\horizon\scripts\
copy /y "%OD%\scripts.dll" "%PK%\plugins\scripts\scripts.dll" >nul || exit /b 1
copy /y "%OD%\plugin.json" "%PK%\plugins\scripts\plugin.json" >nul || exit /b 1
copy /y "%OD%\icon.png" "%PK%\plugins\scripts\icon.png" >nul || exit /b 1
xcopy /e /i /q /y %PD%\examples "%PK%\scripts" >nul || exit /b 1
copy /y %PD%\README-dist.txt "%PK%\README.txt" >nul || exit /b 1

rem --- tests (run: plugins\scripts\tests\run_tests.cmd)
rem scripts_test: unit tests + the engine with real scripts, linked from the core library with a stub host
rem scripts_fakehost: scripts.dll in a stub host with the fake engine graph, both example scripts, events
%CXX% /MP %SINC% /I%PD%\tests /Iplugins\node-explorer\tests /Fo"%TD%\\" /Fd"%TD%\t.pdb" %PD%\tests\scripts_test.cpp %PD%\tests\scripts_fakehost.cpp %PD%\tests\test_host.cpp plugins\node-explorer\tests\fake_graph.cpp plugins\node-explorer\tests\stub_host.cpp || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:"%TD%\scripts_test.exe" /PDB:"%TD%\scripts_test.pdb" "%TD%\scripts_test.obj" "%TD%\test_host.obj" "%TD%\fake_graph.obj" "%TD%\stub_host.obj" "%OD%\scripts_core.lib" build\angelscript.lib build\imgui_core.lib user32.lib kernel32.lib shell32.lib || exit /b 1
link /nologo /DEBUG /MACHINE:X86 /OUT:"%TD%\scripts_fakehost.exe" /PDB:"%TD%\scripts_fakehost.pdb" "%TD%\scripts_fakehost.obj" "%TD%\test_host.obj" "%TD%\fake_graph.obj" "%TD%\stub_host.obj" build\imgui_core.lib user32.lib kernel32.lib || exit /b 1
exit /b 0
