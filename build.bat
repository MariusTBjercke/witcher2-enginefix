@echo off
rem Builds build\dinput8.dll - the runtime fix proxy for The Witcher 2.
rem Needs Visual Studio with the "Desktop development with C++" x86 tools.
rem Set VCVARS to your vcvars32.bat if Visual Studio is not in the default location.
setlocal enabledelayedexpansion
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo vcvars32.bat not found. Set VCVARS to its full path.
    exit /b 1
)
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build

rem Script-patch code (src\sp_*.c) and the vendored inflate. tinf is third-party and kept
rem unaltered; it is not /W4-clean, so it is compiled on its own like in the test harness.
set "SP="
for %%f in (src\sp_*.c) do set "SP=!SP! %%f"
set "TINF="
for %%f in (src\third_party\tinf\*.c) do (
    cl /nologo /O2 /W3 /wd4018 /MT /c "%%f" /Fobuild\ || exit /b 1
    set "TINF=!TINF! build\%%~nf.obj"
)

rem The script-patch data (edits.txt and the .ws fragments) is embedded as RCDATA.
rc /nologo /fo build\version.res src\version.rc || exit /b 1
rc /nologo /fo build\scriptpatches.res src\scriptpatches.rc || exit /b 1
cl /nologo /O2 /W4 /MT /LD src\enginefix.c !SP! !TINF! build\version.res build\scriptpatches.res /Fobuild\ /link user32.lib gdi32.lib /DEF:src\dinput8.def /OUT:build\dinput8.dll /IMPLIB:build\dinput8.lib || exit /b 1
echo Built build\dinput8.dll
