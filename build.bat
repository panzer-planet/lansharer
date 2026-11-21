@echo off
setlocal enabledelayedexpansion
rem Builds lansharer.exe (GUI) and lansharer-cli.exe with MSVC, then runs the self-test.
rem On Linux: cc -O2 -o lansharer cli.c net.c
rem Delayed expansion because the ")" in "(x86)" breaks the for-loop parser otherwise.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VS=%%i"
if not defined VS (echo Visual Studio with the C++ tools was not found.& exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
cl /nologo /W4 /O2 /Fe:lansharer.exe gui_win32.c net.c /link /MANIFEST:EMBED || exit /b 1
cl /nologo /W4 /O2 /Fe:lansharer-cli.exe cli.c net.c || exit /b 1
del *.obj
.\lansharer-cli.exe --test
