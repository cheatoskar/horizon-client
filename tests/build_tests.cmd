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
exit /b 0
