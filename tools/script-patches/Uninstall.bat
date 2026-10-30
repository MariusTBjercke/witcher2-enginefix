@echo off
rem Removes all optional script patches and restores the original files.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ScriptPatches.ps1" -Action Uninstall
pause
