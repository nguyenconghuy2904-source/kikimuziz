@echo off
REM Menuconfig Script for ESP32
REM This script will setup ESP-IDF and open menuconfig

echo ========================================
echo Opening ESP-IDF Menuconfig
echo ========================================
echo.

REM Check if IDF_PATH is set, if not try default location
if "%IDF_PATH%"=="" (
    echo ESP-IDF environment not found, trying to setup...
    
    REM Try common ESP-IDF installation paths (check v5.5 first, then v5.5.1)
    if exist "C:\Espressif\frameworks\esp-idf-v5.5\export.bat" (
        set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5"
        call "C:\Espressif\frameworks\esp-idf-v5.5\export.bat"
        echo ESP-IDF v5.5 found and activated
    ) else if exist "C:\Espressif\frameworks\esp-idf-v5.5.1\export.bat" (
        set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.1"
        call "C:\Espressif\frameworks\esp-idf-v5.5.1\export.bat"
        echo ESP-IDF v5.5.1 found and activated
    ) else if exist "D:\Espressif\frameworks\esp-idf-v5.5\export.bat" (
        set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5"
        call "D:\Espressif\frameworks\esp-idf-v5.5\export.bat"
        echo ESP-IDF v5.5 found and activated
    ) else if exist "D:\Espressif\frameworks\esp-idf-v5.5.1\export.bat" (
        set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5.1"
        call "D:\Espressif\frameworks\esp-idf-v5.5.1\export.bat"
        echo ESP-IDF v5.5.1 found and activated
    ) else if exist "%USERPROFILE%\.espressif\esp-idf\export.bat" (
        call "%USERPROFILE%\.espressif\esp-idf\export.bat"
        echo ESP-IDF found and activated
    ) else (
        echo.
        echo ERROR: ESP-IDF not found!
        echo Please install ESP-IDF or set IDF_PATH environment variable.
        echo.
        echo You can install ESP-IDF using:
        echo   - ESP-IDF Extension for VSCode/Cursor
        echo   - Or download from: https://github.com/espressif/esp-idf
        echo.
        pause
        exit /b 1
    )
) else (
    echo ESP-IDF environment already set: %IDF_PATH%
)

echo ESP-IDF environment ready
echo.

cd /d "%~dp0"

echo Opening menuconfig...
echo.
idf.py menuconfig

if errorlevel 1 (
    echo.
    echo ========================================
    echo Failed to open menuconfig!
    echo ========================================
    pause
    exit /b 1
)

pause

