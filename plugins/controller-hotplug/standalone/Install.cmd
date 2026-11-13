@echo off
rem Puts the Controller Hotplug loader (dinput8.dll) next to the game exe. An existing dinput8.dll of
rem another mod becomes dinput8.chain.dll and keeps working.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0controller-hotplug\install.ps1" install
pause
