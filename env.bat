@echo off
rem Sets up the build environment (MSVC libs + Windows SDK, Clang, CMake, Ninja).
rem Finds the newest Visual Studio / Build Tools with the C++ tools via vswhere (2022, 2026, ...).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
rem vcvars64.bat runs vswhere itself and expects it on PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
rem CMake and Ninja: standalone installs first, then the copies bundled with Visual Studio.
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe;%PATH%;%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja"
