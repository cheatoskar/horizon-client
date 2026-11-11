@echo off
rem Removes the Controller Hotplug loader and controller_hotplug.dll. The log stays.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0controller-hotplug\install.ps1" uninstall
pause
