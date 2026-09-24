@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo [PowerDriveSim] Windows Release build

tasklist /FI "IMAGENAME eq powerdrive-desktop.exe" 2>NUL | find /I "powerdrive-desktop.exe" >NUL
if not errorlevel 1 (
    echo.
    echo ERROR: PowerDriveSim is running and locks the output executable.
    echo Close every PowerDriveSim window, then run this script again.
    goto :failed
)

if defined PDS_QT_ROOT (
    set "PDS_BUILD_QT=%PDS_QT_ROOT%"
) else if defined QTDIR (
    set "PDS_BUILD_QT=%QTDIR%"
) else (
    set "PDS_BUILD_QT=%CD%\.deps\qt\6.5.3\msvc2019_64"
)

if not exist "%PDS_BUILD_QT%\bin\windeployqt.exe" (
    echo.
    echo ERROR: Qt was not found at:
    echo   %PDS_BUILD_QT%
    echo Set PDS_QT_ROOT to a Qt 6.5+ MSVC directory and run again.
    goto :failed
)

where cmake >NUL 2>NUL
if errorlevel 1 (
    echo ERROR: cmake.exe was not found in PATH.
    goto :failed
)

echo [1/4] Configuring...
cmake -S . -B build -DPDS_BUILD_DESKTOP=ON -DCMAKE_PREFIX_PATH="%PDS_BUILD_QT%"
if errorlevel 1 goto :failed

echo [2/4] Building...
cmake --build build --config Release --target powerdrive-desktop --parallel
if errorlevel 1 goto :failed

echo [3/4] Checking deployed Qt runtime...
for %%F in (Qt6Core.dll Qt6Gui.dll Qt6Widgets.dll platforms\qwindows.dll) do (
    if not exist "build\Release\%%F" (
        echo ERROR: Missing build\Release\%%F after CMake build.
        goto :failed
    )
)

echo [4/4] Smoke test...
set "QT_QPA_PLATFORM=windows"
set "QT_QPA_PLATFORM_PLUGIN_PATH="
set "QT_QPA_FONTDIR=C:\Windows\Fonts"
"build\Release\powerdrive-desktop.exe" --smoke-test
if errorlevel 1 goto :failed

echo.
echo BUILD SUCCEEDED:
echo   %CD%\build\Release\powerdrive-desktop.exe
echo.
pause
exit /b 0

:failed
echo.
echo BUILD FAILED. See the messages above.
echo.
pause
exit /b 1
