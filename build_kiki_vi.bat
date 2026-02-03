@echo off
REM Build script for Kiki board with Vietnamese language
REM Author: AI Assistant
REM Date: 2025-12-12

echo ========================================
echo Building Kiki Board - Vietnamese
echo ========================================
echo.

REM Setup ESP-IDF environment
if "%IDF_PATH%"=="" (
    echo Setting up ESP-IDF environment...
    
    REM Try common ESP-IDF installation paths
    if exist "C:\Espressif\frameworks\esp-idf-v5.5\export.bat" (
        call "C:\Espressif\frameworks\esp-idf-v5.5\export.bat"
        echo ESP-IDF v5.5 activated
    ) else if exist "C:\Espressif\frameworks\esp-idf-v5.5.1\export.bat" (
        call "C:\Espressif\frameworks\esp-idf-v5.5.1\export.bat"
        echo ESP-IDF v5.5.1 activated
    ) else if exist "D:\Espressif\frameworks\esp-idf-v5.5\export.bat" (
        call "D:\Espressif\frameworks\esp-idf-v5.5\export.bat"
        echo ESP-IDF v5.5 activated
    ) else if exist "D:\Espressif\frameworks\esp-idf-v5.5.1\export.bat" (
        call "D:\Espressif\frameworks\esp-idf-v5.5.1\export.bat"
        echo ESP-IDF v5.5.1 activated
    ) else if exist "%USERPROFILE%\.espressif\esp-idf\export.bat" (
        call "%USERPROFILE%\.espressif\esp-idf\export.bat"
        echo ESP-IDF activated
    ) else (
        echo.
        echo ERROR: ESP-IDF v5.5 not found!
        echo Please install ESP-IDF v5.5 first
        echo.
        pause
        exit /b 1
    )
) else (
    echo ESP-IDF environment already set: %IDF_PATH%
)

echo.
cd /d "%~dp0"

REM Set target to ESP32-S3 (required for Kiki board)
echo Step 1: Setting target to ESP32-S3...
idf.py set-target esp32s3
if errorlevel 1 (
    echo ERROR: Failed to set target to ESP32-S3
    pause
    exit /b 1
)

echo.
echo Step 2: Configuring for Kiki board with Vietnamese...
REM Use menuconfig to set board type and language
echo.
echo Please configure in menuconfig:
echo   1. Board Type: Select "kiki"
echo   2. Default Language: Select "Vietnamese"
echo   3. Save and Exit
echo.
pause

idf.py menuconfig
if errorlevel 1 (
    echo ERROR: Configuration failed
    pause
    exit /b 1
)

echo.
echo Step 3: Building firmware...
idf.py build
if errorlevel 1 (
    echo.
    echo ========================================
    echo BUILD FAILED!
    echo ========================================
    pause
    exit /b 1
)

echo.
echo ========================================
echo BUILD SUCCESSFUL!
echo ========================================
echo.
echo Firmware files are in: build\
echo.
echo To flash to device, run:
echo   idf.py -p COMX flash monitor
echo   (replace COMX with your COM port)
echo.
pause
