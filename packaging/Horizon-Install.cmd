@echo off
rem Puts the Horizon Legacy Loader (dinput8.dll) next to the game's exe. An existing dinput8.dll of
rem another mod becomes dinput8.chain.dll and keeps working.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0horizon\loader\install.ps1" install
pause
