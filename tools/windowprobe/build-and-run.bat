@echo off
rem Native harness for the window-mode detour and style stub (no game needed).
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0\..\.."
if not exist build mkdir build
cl /nologo /O2 /W4 /MT tools\windowprobe\probe.c /Fobuild\windowprobe.obj /Fe:build\windowprobe.exe || exit /b 1
build\windowprobe.exe
exit /b %errorlevel%
