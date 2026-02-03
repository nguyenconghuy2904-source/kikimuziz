@echo off
REM ===================================
REM Flash Merged Firmware to KiKi Robot
REM ===================================

echo.
echo ===================================
echo Flash Merged Firmware to ESP32-S3
echo COM Port: COM31
echo Baud: 921600
echo ===================================
echo.

cd /d %~dp0build

if not exist kiki_merged_flash.bin (
    echo ERROR: kiki_merged_flash.bin not found!
    echo Please run create_merged_bin.bat first.
    pause
    exit /b 1
)

echo File: kiki_merged_flash.bin
for %%A in (kiki_merged_flash.bin) do echo Size: %%~zA bytes (%%~zAKB)
echo.

set /p confirm="Flash to COM31? (Y/N): "
if /i not "%confirm%"=="Y" (
    echo Cancelled.
    pause
    exit /b 0
)

echo.
echo Flashing...
echo.

python -m esptool -p COM31 -b 921600 write_flash 0x0 kiki_merged_flash.bin

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ===================================
    echo SUCCESS! Firmware flashed
    echo ===================================
) else (
    echo.
    echo ===================================
    echo ERROR: Flash failed
    echo ===================================
)

echo.
pause
