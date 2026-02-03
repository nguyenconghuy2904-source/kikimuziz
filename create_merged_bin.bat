@echo off
REM ================================
REM Create Merged Firmware Binary
REM ================================

cd /d %~dp0

echo [1/2] Loading ESP-IDF environment...
call C:\Espressif\frameworks\esp-idf-v5.5\export.bat

echo.
echo [2/2] Creating merged firmware binary...
cd build

esptool.py --chip esp32s3 merge_bin ^
    -o kiki_merged_flash.bin ^
    --flash_mode dio ^
    --flash_freq 80m ^
    --flash_size 16MB ^
    0x0 bootloader/bootloader.bin ^
    0x8000 partition_table/partition-table.bin ^
    0xd000 ota_data_initial.bin ^
    0x20000 xiaozhi.bin ^
    0x800000 generated_assets.bin

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ================================
    echo SUCCESS! Merged binary created:
    echo build\kiki_merged_flash.bin
    echo ================================
    echo.
    echo To flash: esptool.py -p COM31 -b 921600 write_flash 0x0 build\kiki_merged_flash.bin
    echo.
) else (
    echo.
    echo ================================
    echo ERROR: Failed to create merged binary
    echo ================================
    echo.
)

pause
