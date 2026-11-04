@echo off
rem Build detection, import slots by name and the per-build gate in src\enginefix.c, checked
rem against the real exes (mapped, never run):
rem   tools\buildprobe\build-and-run.bat "<steam>\bin\witcher2.exe" "<gog>\bin\witcher2.exe"
rem Compiles src\enginefix.c, so the script-patch sources and tinf are linked in as in build.bat.
setlocal enabledelayedexpansion
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul 2>nul || exit /b 1
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
cl /nologo /O2 /W4 /MT tools\buildprobe\probe.c !SP! !TINF! build\scriptpatches.res /Fobuild\ /Fe:build\buildprobe.exe user32.lib gdi32.lib shell32.lib || exit /b 1
build\buildprobe.exe %*
exit /b %errorlevel%
