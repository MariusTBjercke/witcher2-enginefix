@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0\..\.."
cl /nologo /O2 /W4 /MT /guard:cf tools\distanceprobe\probe.c /Fobuild\distanceprobe.obj /Fe:build\distanceprobe.exe || exit /b 1
build\distanceprobe.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT tools\distanceprobe\hotkeys.c /Fobuild\distancehotkeys.obj /Fe:build\distancehotkeys.exe user32.lib gdi32.lib || exit /b 1
build\distancehotkeys.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT tools\distanceprobe\presets.c /Fobuild\distancepresets.obj /Fe:build\distancepresets.exe user32.lib gdi32.lib || exit /b 1
build\distancepresets.exe
exit /b %errorlevel%
