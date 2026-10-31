@echo off
rem Builds build\dinput8.dll - the runtime fix proxy for The Witcher 2.
rem Needs Visual Studio with the "Desktop development with C++" x86 tools.
rem Set VCVARS to your vcvars32.bat if Visual Studio is not in the default location.
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo vcvars32.bat not found. Set VCVARS to its full path.
    exit /b 1
)
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build

rc /nologo /fo build\version.res src\version.rc || exit /b 1
cl /nologo /O2 /W4 /MT /LD src\enginefix.c build\version.res /Fobuild\ /link user32.lib gdi32.lib /DEF:src\dinput8.def /OUT:build\dinput8.dll /IMPLIB:build\dinput8.lib || exit /b 1
echo Built build\dinput8.dll
