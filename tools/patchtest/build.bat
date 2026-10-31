@echo off
rem Builds build\sptest.exe - the offline harness for src\sp_*.c.
rem Same toolchain and flags as build.bat. Needs Visual Studio with the x86 C++ tools.
rem Set VCVARS to your vcvars32.bat if Visual Studio is not in the default location.
rem
rem The exe is NOT called patchtest.exe: Windows' UAC installer detection treats a
rem 32-bit, manifest-less exe whose name contains "patch" as an installer and asks for
rem elevation on every start (consent.exe), which hangs an unattended run.
setlocal enabledelayedexpansion
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo vcvars32.bat not found. Set VCVARS to its full path.
    exit /b 1
)
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0..\.."
if not exist build mkdir build

rem cl does not expand wildcards itself; every src\sp_*.c goes into the harness.
set "SP="
for %%f in (src\sp_*.c) do set "SP=!SP! %%f"

rem tinf is third-party and kept unaltered. It is not /W4-clean (two signed/unsigned
rem mismatches remain at /W3 too), so it is compiled on its own with /W3 /wd4018 and the
rem object is linked into the harness.
set "TINF="
for %%f in (src\third_party\tinf\*.c) do (
    cl /nologo /O2 /W3 /wd4018 /MT /c "%%f" /Fobuild\ || exit /b 1
    set "TINF=!TINF! build\%%~nf.obj"
)

rem The script-patch data (edits.txt and the .ws fragments) is embedded as RCDATA; see
rem src\scriptpatches.rc. FindResource/LoadResource live in kernel32, no extra lib needed.
rc /nologo /fo build\scriptpatches.res src\scriptpatches.rc || exit /b 1

cl /nologo /O2 /W4 /MT /Isrc tools\patchtest\patchtest.c !SP! !TINF! build\scriptpatches.res /Fobuild\ /Fe:build\sptest.exe || exit /b 1
echo Built build\sptest.exe
