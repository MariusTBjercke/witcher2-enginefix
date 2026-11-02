@echo off
rem Native harnesses for the distance hooks, the LOD hotkeys and the graphics presets.
rem They compile src\enginefix.c (through hotkeys.c/presets.c), so the same script-patch
rem objects, vendored inflate and embedded patch data the DLL needs are linked in too;
rem shell32 comes from the window feature's User.ini read.
setlocal enabledelayedexpansion
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0\..\.."
if not exist build mkdir build

set "SP="
for %%f in (src\sp_*.c) do set "SP=!SP! %%f"
set "TINF="
for %%f in (src\third_party\tinf\*.c) do (
    cl /nologo /O2 /W3 /wd4018 /MT /c "%%f" /Fobuild\ || exit /b 1
    set "TINF=!TINF! build\%%~nf.obj"
)
rc /nologo /fo build\scriptpatches.res src\scriptpatches.rc || exit /b 1

cl /nologo /O2 /W4 /MT /guard:cf tools\distanceprobe\probe.c /Fobuild\distanceprobe.obj /Fe:build\distanceprobe.exe || exit /b 1
build\distanceprobe.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT tools\distanceprobe\hotkeys.c !SP! !TINF! build\scriptpatches.res /Fobuild\ /Fe:build\distancehotkeys.exe user32.lib gdi32.lib shell32.lib || exit /b 1
build\distancehotkeys.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /MT tools\distanceprobe\presets.c !SP! !TINF! build\scriptpatches.res /Fobuild\ /Fe:build\distancepresets.exe user32.lib gdi32.lib shell32.lib || exit /b 1
build\distancepresets.exe
exit /b %errorlevel%
