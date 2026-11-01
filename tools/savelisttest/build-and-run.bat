@echo off
rem Unit test for the save-list trace (call-site redirect, counters, memory walk) and the
rem fast_list lazy save reads in src\enginefix.c.
rem Linked at 0x400000 with a big zero-filled arena that covers the game addresses.
rem Compiles the DLL source into a console exe with the builder and its call sites placed at their real addresses; no game needed.
rem Optional argument: a folder of real .sav files (read only) - each is attached lazily and
rem compared byte for byte with a full unpack, e.g. the Steam remote folder of app 20920.
setlocal enabledelayedexpansion
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist ..\..\build mkdir ..\..\build
rem enginefix.c needs the script-patch sources and tinf, as in build.bat.
set "SP="
for %%f in (..\..\src\sp_*.c) do set "SP=!SP! %%f"
set "TINF="
for %%f in (..\..\src\third_party\tinf\*.c) do (
    cl /nologo /O2 /W3 /wd4018 /MT /c "%%f" /Fo..\..\build\ || exit /b 1
    set "TINF=!TINF! ..\..\build\%%~nf.obj"
)
cl /nologo /W4 /MT savelisttest.c !SP! !TINF! /Fo..\..\build\ /Fe..\..\build\savelisttest.exe user32.lib gdi32.lib /link /BASE:0x400000 /DYNAMICBASE:NO /FIXED || exit /b 1
..\..\build\savelisttest.exe %*
