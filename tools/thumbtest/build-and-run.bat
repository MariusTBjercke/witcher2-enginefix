@echo off
rem Unit test for the save-thumbnail pairing and stale-preview marks in src\enginefix.c.
rem Compiles the DLL source into a console exe with fake Steam writes; no game needed.
rem The DLL source needs the script-patch sources and tinf linked in, as in build.bat.
setlocal enabledelayedexpansion
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist ..\..\build mkdir ..\..\build
set "SP="
for %%f in (..\..\src\sp_*.c) do set "SP=!SP! %%f"
set "TINF="
for %%f in (..\..\src\third_party\tinf\*.c) do (
    cl /nologo /O2 /W3 /wd4018 /MT /c "%%f" /Fo..\..\build\ || exit /b 1
    set "TINF=!TINF! ..\..\build\%%~nf.obj"
)
rc /nologo /fo ..\..\build\scriptpatches.res ..\..\src\scriptpatches.rc || exit /b 1
cl /nologo /W4 /MT thumbtest.c !SP! !TINF! ..\..\build\scriptpatches.res /Fo..\..\build\ /Fe..\..\build\thumbtest.exe user32.lib gdi32.lib shell32.lib || exit /b 1
..\..\build\thumbtest.exe
