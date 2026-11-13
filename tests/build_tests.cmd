@echo off
rem Fragment called by build.cmd (vcvars set, cwd = repo root): unit tests, chain stand-in, host test.
if not exist build\test mkdir build\test
%CXX% /Fo"build\test\\" /Fd"build\test\t.pdb" tests\unit_tests.cpp || exit /b 1
link /nologo /MACHINE:X86 /OUT:build\test\unit_tests.exe build\test\unit_tests.obj build\common.lib bcrypt.lib user32.lib kernel32.lib || exit /b 1
%CXX% /Fo"build\test\\" /Fd"build\test\t.pdb" tests\chain_dll.cpp || exit /b 1
echo LIBRARY dinput8> build\test\chain.def
echo EXPORTS>> build\test\chain.def
echo     DirectInput8Create = Chain_DirectInput8Create>> build\test\chain.def
link /nologo /DLL /MACHINE:X86 /DEF:build\test\chain.def /OUT:build\test\dinput8.chain.dll /IMPLIB:build\test\chain.lib build\test\chain_dll.obj user32.lib kernel32.lib || exit /b 1
%CXX% /Fo"build\test\\" /Fd"build\test\t.pdb" tests\host_test.cpp || exit /b 1
link /nologo /MACHINE:X86 /OUT:build\test\host_test.exe build\test\host_test.obj user32.lib kernel32.lib ole32.lib windowscodecs.lib dxguid.lib || exit /b 1
rem ABI layout test: a plugin built against the frozen 1.2 header (tests\abi12\horizon_plugin.h, found first
rem because it sits next to the source)
if not exist build\test\abi12 mkdir build\test\abi12
%CXX% /Fo"build\test\abi12\\" /Fd"build\test\abi12\p.pdb" tests\abi12\abi12_plugin.cpp || exit /b 1
link /nologo /DLL /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:build\test\abi12\abi12.dll /IMPLIB:build\test\abi12\abi12.lib build\test\abi12\abi12_plugin.obj user32.lib kernel32.lib || exit /b 1
copy /y tests\abi12\plugin.json build\test\abi12\plugin.json >nul || exit /b 1
rem stand-in for Competition Patch (only the module name TMCompPatch.dll matters), loaded late by host_test keys
if not exist build\test\cp mkdir build\test\cp
%CXX% /Fo"build\test\cp\\" /Fd"build\test\cp\p.pdb" tests\fake_cp.cpp || exit /b 1
link /nologo /DLL /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:build\test\cp\TMCompPatch.dll /IMPLIB:build\test\cp\TMCompPatch.lib build\test\cp\fake_cp.obj kernel32.lib || exit /b 1
rem headless menu input test (the dim layer must not take the pointer)
%CXX% /Fo"build\test\\" /Fd"build\test\t.pdb" /Ithird_party\imgui tests\ui_input_test.cpp src\host\ui\ui_style.cpp src\host\ui\ui_menu.cpp src\host\ui\ui_bar.cpp || exit /b 1
link /nologo /MACHINE:X86 /OUT:build\test\ui_input_test.exe build\test\ui_input_test.obj build\test\ui_style.obj build\test\ui_menu.obj build\test\ui_bar.obj build\imgui_core.lib user32.lib gdi32.lib kernel32.lib ole32.lib windowscodecs.lib || exit /b 1
rem crash test: two modules that throw while drawing (tests\crashy, one source, ids crashmenu and crashoverlay)
for %%M in (menu overlay) do (
  if not exist build\test\crash%%M mkdir build\test\crash%%M
  %CXX% /DCRASH_%%M /Fo"build\test\crash%%M\\" /Fd"build\test\crash%%M\p.pdb" tests\crashy\crashy_plugin.cpp || exit /b 1
  link /nologo /DLL /MACHINE:X86 /SAFESEH /DYNAMICBASE /NXCOMPAT /OUT:build\test\crash%%M\crash%%M.dll /IMPLIB:build\test\crash%%M\crash%%M.lib build\test\crash%%M\crashy_plugin.obj build\imgui_core.lib user32.lib kernel32.lib gdi32.lib || exit /b 1
)
exit /b 0
