@echo off
REM Build and Flash Script for ESP32
REM This script will setup ESP-IDF, build, and flash the firmware

echo ========================================
echo Building and Flashing Firmware
echo ========================================
echo.

REM Check if IDF_PATH is set, if not try default location
if "%IDF_PATH%"=="" (
    echo ESP-IDF environment not found, trying to setup...
    
    REM Try common ESP-IDF installation paths (check v5.5 first, then v5.5.1)
    if exist "C:\Espressif\frameworks\esp-idf-v5.5\export.bat" (
        set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5
        call "%IDF_PATH%\export.bat"
        echo ESP-IDF v5.5 found and activated
    ) else if exist "C:\Espressif\frameworks\esp-idf-v5.5.1\export.bat" (
        set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.1
        call "%IDF_PATH%\export.bat"
        echo ESP-IDF v5.5.1 found and activated
    ) else if exist "D:\Espressif\frameworks\esp-idf-v5.5\export.bat" (
        set IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5
        call "%IDF_PATH%\export.bat"
        echo ESP-IDF v5.5 found and activated
    ) else if exist "D:\Espressif\frameworks\esp-idf-v5.5.1\export.bat" (
        set IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5.1
        call "%IDF_PATH%\export.bat"
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

echo Step 1: Releasing COM port (killing Python processes)...
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
echo Step 3: Releasing COM port before flashing...
taskkill /F /IM python.exe /T >nul 2>&1
taskkill /F /IM pythonw.exe /T >nul 2>&1
timeout /t 1 /nobreak >nul
echo.

echo Step 4: Flashing firmware...
REM Check if COM port was provided as command line argument
if "%1"=="" (
    echo Please connect your ESP32 device and note the COM port number.
    echo.
    set /p COM_PORT="Enter COM port (e.g., COM3, COM31): "
    if "%COM_PORT%"=="" (
        echo No COM port specified, trying to auto-detect...
        idf.py flash
    ) else (
        idf.py -p %COM_PORT% flash
    )
) else (
    set COM_PORT=%1
    echo Using COM port: %COM_PORT%
    idf.py -p %COM_PORT% flash
)

if errorlevel 1 (
    echo.
    echo ========================================
    echo Flash failed!
    echo ========================================
    pause
    exit /b 1
)

echo.
echo ========================================
echo Build and Flash completed successfully!
echo ========================================
echo.
echo Opening monitor...
echo Press Ctrl+] to exit monitor
echo.

REM Use COM_PORT from argument or user input
if "%COM_PORT%"=="" (
    if "%1"=="" (
        idf.py monitor
    ) else (
        idf.py -p %1 monitor
    )
) else (
    idf.py -p %COM_PORT% monitor
)

pause

