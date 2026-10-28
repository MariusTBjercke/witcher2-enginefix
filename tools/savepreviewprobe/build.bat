@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0\..\.."
if not exist build mkdir build
cl /nologo /O2 /W4 /MT tools\savepreviewprobe\probe.cpp /Fobuild\savepreviewprobe.obj /Fe:build\savepreviewprobe.exe user32.lib || exit /b 1
