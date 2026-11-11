@echo off
rem Builds build\plugin-hello.dll - the example plugin for the script-binding loader.
rem Same toolchain as build.bat. Set VCVARS if Visual Studio is not in the default location.
rem Install it into <game>\bin\witcher2_enginefix_plugins\ (the folder is created by hand).
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo vcvars32.bat not found. Set VCVARS to its full path.
    exit /b 1
)
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0..\.."
if not exist build mkdir build

cl /nologo /O2 /W4 /MT /LD /Isrc tools\plugin-example\plugin.c /Fobuild\ /Fe:build\plugin-hello.dll || exit /b 1
echo Built build\plugin-hello.dll
