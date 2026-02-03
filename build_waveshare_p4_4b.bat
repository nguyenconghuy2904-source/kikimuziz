@echo off
REM Build script for Waveshare ESP32-P4-WIFI6-Touch-LCD-4B
echo ========================================
echo Building Waveshare P4 WiFi6 Touch LCD 4B
echo ========================================
echo.

REM Setup ESP-IDF environment using official export.bat
call C:\Espressif\frameworks\esp-idf-v5.5\export.bat

REM Force Python 3.11
set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;%PATH%
set PYTHON=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe

cd /d "%~dp0"

echo.
echo Verifying configuration:
echo - Target: ESP32-P4
echo - Board: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B
echo.

echo Killing existing processes...
taskkill /F /IM python.exe /T >nul 2>&1
taskkill /F /IM cmake.exe /T >nul 2>&1
taskkill /F /IM ninja.exe /T >nul 2>&1
timeout /t 2 /nobreak >nul

echo.
echo Building project with Python 3.11...
python --version

REM Disable ccache to avoid issues
set IDF_CCACHE_ENABLE=0

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
echo ========================================
echo Build completed successfully!
echo ========================================
echo.
echo Firmware binary location:
echo   build\kikichatwiath_ai-master.bin
echo.
echo To flash the firmware, run:
echo   idf.py -p COMx flash
echo   (Replace COMx with your actual COM port)
echo.
pause
