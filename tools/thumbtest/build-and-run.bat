@echo off
rem Unit test for the save-thumbnail pairing and stale-preview marks in src\enginefix.c.
rem Compiles the DLL source into a console exe with fake Steam writes; no game needed.
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist ..\..\build mkdir ..\..\build
cl /nologo /W4 /MT thumbtest.c /Fo..\..\build\thumbtest.obj /Fe..\..\build\thumbtest.exe user32.lib gdi32.lib || exit /b 1
..\..\build\thumbtest.exe
