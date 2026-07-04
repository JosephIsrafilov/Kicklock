@echo off
setlocal enabledelayedexpansion

echo ===========================================
echo   KickLock VST3 Release Builder
echo ===========================================
echo.

echo Looking for Visual Studio CMake...
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist %VSWHERE% (
    echo Error: vswhere.exe not found. Is Visual Studio installed?
    pause
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`) do (
  set CMAKE_PATH="%%i"
)

if not defined CMAKE_PATH (
    echo Error: Could not find CMake inside Visual Studio.
    echo Please ensure the "Desktop development with C++" workload is installed.
    pause
    exit /b 1
)

echo Found CMake at: !CMAKE_PATH!
echo.

echo [1/2] Generating Visual Studio solution (if anything changed)...
!CMAKE_PATH! -B build-release -DCMAKE_BUILD_TYPE=Release

if %errorlevel% neq 0 (
    echo.
    echo Error: CMake generation failed!
    pause
    exit /b %errorlevel%
)

echo.
echo [2/2] Building project in Release mode...
!CMAKE_PATH! --build build-release --config Release -j 8

if %errorlevel% neq 0 (
    echo.
    echo Error: Build failed! Check the errors above.
    pause
    exit /b %errorlevel%
)

echo.
echo =======================================================
echo                 BUILD SUCCEEDED!
echo =======================================================
echo The VST3 file is ready and located at:
echo %~dp0build-release\KickLock_artefacts\Release\VST3\KickLock.vst3
echo.
pause
