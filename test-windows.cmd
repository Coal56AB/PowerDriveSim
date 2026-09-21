@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo [PowerDriveSim] Windows Release tests

if defined PDS_QT_ROOT (
    set "PDS_TEST_QT=%PDS_QT_ROOT%"
) else if defined QTDIR (
    set "PDS_TEST_QT=%QTDIR%"
) else (
    set "PDS_TEST_QT=%CD%\.deps\qt\6.5.3\msvc2019_64"
)

if not exist "%PDS_TEST_QT%\plugins\platforms\qoffscreen.dll" (
    echo.
    echo ERROR: Qt offscreen platform plugin was not found at:
    echo   %PDS_TEST_QT%
    echo Set PDS_QT_ROOT to a Qt 6.5+ MSVC directory and run again.
    goto :failed
)

where cmake >NUL 2>NUL
if errorlevel 1 (
    echo ERROR: cmake.exe was not found in PATH.
    goto :failed
)
where ctest >NUL 2>NUL
if errorlevel 1 (
    echo ERROR: ctest.exe was not found in PATH.
    goto :failed
)

echo [1/3] Configuring...
cmake -S . -B build -DPDS_BUILD_DESKTOP=ON -DCMAKE_PREFIX_PATH="%PDS_TEST_QT%"
if errorlevel 1 goto :failed

echo [2/3] Building tests...
cmake --build build --config Release --parallel
if errorlevel 1 goto :failed

echo [3/3] Running tests offscreen...
set "PATH=%PDS_TEST_QT%\bin;%PATH%"
set "QT_QPA_PLATFORM=offscreen"
set "QT_QPA_PLATFORM_PLUGIN_PATH=%PDS_TEST_QT%\plugins\platforms"
set "QT_QPA_FONTDIR=C:\Windows\Fonts"
ctest --test-dir build -C Release --output-on-failure
if errorlevel 1 goto :failed

echo.
echo ALL TESTS PASSED
echo.
pause
exit /b 0

:failed
echo.
echo TESTS FAILED. See the messages above.
echo.
pause
exit /b 1
