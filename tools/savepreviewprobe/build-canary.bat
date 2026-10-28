@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0\..\.."
cl /nologo /O2 /W4 /MT tools\savepreviewprobe\log-lock-canary.c /Fobuild\log-lock-canary.obj /Fe:build\log-lock-canary.exe || exit /b 1
