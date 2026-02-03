@echo off
REM Quick build for Kiki board with Vietnamese - Auto config
REM This script automatically configures without menuconfig
REM Author: AI Assistant

echo ========================================
echo Kiki Board Quick Build - Vietnamese
echo ========================================
echo.

REM Setup ESP-IDF
if "%IDF_PATH%"=="" (
    echo Setting up ESP-IDF...
    if exist "C:\Espressif\frameworks\esp-idf-v5.5\export.bat" (
        call "C:\Espressif\frameworks\esp-idf-v5.5\export.bat"
    ) else if exist "C:\Espressif\frameworks\esp-idf-v5.5.1\export.bat" (
        call "C:\Espressif\frameworks\esp-idf-v5.5.1\export.bat"
    ) else (
        echo ERROR: ESP-IDF not found!
        pause
        exit /b 1
    )
)

cd /d "%~dp0"

echo Step 1: Setting target ESP32-S3...
idf.py set-target esp32s3

echo.
echo Step 2: Configuring Kiki + Vietnamese...
REM Create temporary config file
echo CONFIG_IDF_TARGET_ESP32S3=y > sdkconfig.kiki_vi
echo CONFIG_BOARD_TYPE_KIKI=y >> sdkconfig.kiki_vi
echo CONFIG_LANGUAGE_VI_VN=y >> sdkconfig.kiki_vi

REM Backup current sdkconfig if exists
if exist sdkconfig (
    copy /Y sdkconfig sdkconfig.backup >nul
    echo Backed up current sdkconfig
)

REM Apply config
type sdkconfig.kiki_vi >> sdkconfig

echo.
echo Step 3: Building...
idf.py build

if errorlevel 1 (
    echo.
    echo BUILD FAILED!
    if exist sdkconfig.backup (
        copy /Y sdkconfig.backup sdkconfig >nul
        echo Restored original sdkconfig
    )
    pause
    exit /b 1
)

echo.
echo ========================================
echo BUILD SUCCESS!
echo ========================================
echo Firmware: build\xiaozhi-assistant.bin
echo.
pause
