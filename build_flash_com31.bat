@echo off
REM Build and Flash Script for ESP32 to COM31
REM Requires ESP-IDF v5.5+ to be installed

echo ========================================
echo Building and Flashing Firmware to COM31
echo ========================================
echo.

REM Try to setup ESP-IDF environment
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
        echo.
        echo Please install ESP-IDF v5.5 first:
        echo   1. Install ESP-IDF Extension in Cursor/VSCode (recommended)
        echo   2. Or download from: https://github.com/espressif/esp-idf/releases/tag/v5.5
        echo   3. Or use ESP-IDF installer: https://dl.espressif.com/dl/esp-idf/
        echo.
        pause
        exit /b 1
    )
) else (
    echo ESP-IDF environment already set: %IDF_PATH%
)

echo.
cd /d "%~dp0"

echo Step 1: Releasing COM31 port...
taskkill /F /IM python.exe /T >nul 2>&1
taskkill /F /IM pythonw.exe /T >nul 2>&1
timeout /t 1 /nobreak >nul
echo   COM port released
echo.

echo Step 2: Building project...
idf.py build
if errorlevel 1 (
    echo.
    echo ========================================
    echo Build failed!
    echo ========================================
    pause
    exit /b 1
)

echo.
echo Step 3: Releasing COM31 before flashing...
taskkill /F /IM python.exe /T >nul 2>&1
taskkill /F /IM pythonw.exe /T >nul 2>&1
timeout /t 1 /nobreak >nul
echo.

echo Step 4: Flashing firmware to COM31...
idf.py -p COM31 flash
if errorlevel 1 (
    echo.
    echo ========================================
    echo Flash failed!
    echo ========================================
    echo.
    echo Troubleshooting:
    echo   - Check if COM31 is correct port
    echo   - Make sure ESP32 is connected
    echo   - Try pressing BOOT button on ESP32
    echo   - Check USB cable (use data cable, not charge-only)
    echo.
    pause
    exit /b 1
)

echo.
echo ========================================
echo Flash completed successfully!
echo ========================================
echo.
echo Opening monitor on COM31...
echo Press Ctrl+] to exit monitor
echo.

idf.py -p COM31 monitor

pause

