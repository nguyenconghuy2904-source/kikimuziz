@echo off
echo ========================================
echo Building for Kiki Robot Board (ESP32-S3)
echo ========================================

REM Change to project directory
cd /d %~dp0

REM Build the project
echo Running idf.py build...
idf.py build

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================
    echo Build SUCCESS for Kiki Robot!
    echo ========================================
    echo.
    echo To flash: idf.py -p COM(X) flash monitor
    echo Replace COM(X) with your actual port
) else (
    echo.
    echo ========================================
    echo Build FAILED! Check errors above.
    echo ========================================
)

pause
