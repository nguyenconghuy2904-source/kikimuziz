@echo off
REM Otto Robot Firmware Flash Script
REM Version 2.0.3 - Complete Binary

echo ========================================
echo Otto Robot Firmware Flash Tool
echo ========================================
echo.
echo Firmware: firmware-complete.bin
echo Version: 2.0.3
echo Board: otto-robot (ESP32-S3)
echo.
echo Features:
echo - Improved movements (35/145 angles)
echo - Web volume control
echo - Touch sensor IP display
echo - Wake word support
echo.
echo ========================================
echo.

REM Check for COM port argument
if "%1"=="" (
    set COMPORT=COM24
    echo Using default port: COM24
) else (
    set COMPORT=%1
    echo Using port: %1
)

echo.
echo Flashing firmware to %COMPORT%...
echo.

REM Flash with high baud rate first, fallback to slow if failed
python -m esptool --chip esp32s3 --port %COMPORT% --baud 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 firmware-complete.bin

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Fast flash failed, trying slower baud rate...
    echo.
    python -m esptool --chip esp32s3 --port %COMPORT% --baud 115200 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 firmware-complete.bin
)

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================
    echo Flash successful!
    echo ========================================
    echo.
    echo Otto Robot is rebooting...
    echo Connect to WiFi to access web interface
    echo IP will be displayed after 5 touch sensor presses
    echo.
) else (
    echo.
    echo ========================================
    echo Flash failed!
    echo ========================================
    echo.
    echo Troubleshooting:
    echo 1. Check ESP32 is connected
    echo 2. Try different COM port
    echo 3. Check USB cable
    echo 4. Install CH340/CP2102 driver
    echo.
)

pause
