@echo off
rem Removes the Horizon Legacy Loader; dinput8.chain.dll gets its old name back.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0horizon\loader\install.ps1" uninstall
pause
