@echo off
rem Unit test for the save-list trace (call-site redirect, counters, memory walk) in src\enginefix.c.
rem Linked at 0x400000 with a big zero-filled arena that covers the game addresses.
rem Compiles the DLL source into a console exe with the builder and its call sites placed at their real addresses; no game needed.
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist ..\..\build mkdir ..\..\build
cl /nologo /W4 /MT savelisttest.c /Fo..\..\build\savelisttest.obj /Fe..\..\build\savelisttest.exe user32.lib gdi32.lib /link /BASE:0x400000 /DYNAMICBASE:NO /FIXED || exit /b 1
..\..\build\savelisttest.exe
