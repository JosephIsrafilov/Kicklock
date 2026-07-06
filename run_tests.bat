@echo off
setlocal enabledelayedexpansion

echo ===========================================
echo   KickLock VST3 Test Runner
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
    pause
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe`) do (
  set CTEST_PATH="%%i"
)

echo Found CMake at: !CMAKE_PATH!
echo.

echo [1/3] Configuring tests...
!CMAKE_PATH! -B build-tests -DCMAKE_BUILD_TYPE=Debug

if %errorlevel% neq 0 (
    echo.
    echo Error: CMake generation failed!
    pause
    exit /b %errorlevel%
)

echo.
echo [2/3] Building tests...
!CMAKE_PATH! --build build-tests --config Debug --target KickLockDspTests

if %errorlevel% neq 0 (
    echo.
    echo Error: Build failed!
    pause
    exit /b %errorlevel%
)

echo.
echo [3/3] Running tests...
cd build-tests
!CTEST_PATH! -C Debug --output-on-failure
set TEST_RESULT=%errorlevel%
cd ..

if !TEST_RESULT! neq 0 (
    echo.
    echo Error: Tests failed!
    pause
    exit /b !TEST_RESULT!
)

echo.
echo =======================================================
echo                 ALL TESTS PASSED!
echo =======================================================
echo.
pause
