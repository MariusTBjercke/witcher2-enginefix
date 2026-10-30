@echo off
rem Optional script patches (shop book marker, Continue, pause-menu Quit). Close the game first.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ScriptPatches.ps1" -Action Install
pause
