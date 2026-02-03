#!/bin/bash
# Otto Robot Firmware Flash Script
# Version 2.0.3 - Complete Binary

echo "========================================"
echo "Otto Robot Firmware Flash Tool"
echo "========================================"
echo ""
echo "Firmware: firmware-complete.bin"
echo "Version: 2.0.3"
echo "Board: otto-robot (ESP32-S3)"
echo ""
echo "Features:"
echo "- Improved movements (35/145 angles)"
echo "- Web volume control"
echo "- Touch sensor IP display"
echo "- Wake word support"
echo ""
echo "========================================"
echo ""

# Check for port argument
if [ -z "$1" ]; then
    PORT="/dev/ttyUSB0"
    echo "Using default port: /dev/ttyUSB0"
else
    PORT="$1"
    echo "Using port: $1"
fi

echo ""
echo "Flashing firmware to $PORT..."
echo ""

# Flash with high baud rate first, fallback to slow if failed
python -m esptool --chip esp32s3 --port $PORT --baud 460800 \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 firmware-complete.bin

if [ $? -ne 0 ]; then
    echo ""
    echo "Fast flash failed, trying slower baud rate..."
    echo ""
    python -m esptool --chip esp32s3 --port $PORT --baud 115200 \
        --before default_reset --after hard_reset \
        write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
        0x0 firmware-complete.bin
fi

if [ $? -eq 0 ]; then
    echo ""
    echo "========================================"
    echo "Flash successful!"
    echo "========================================"
    echo ""
    echo "Otto Robot is rebooting..."
    echo "Connect to WiFi to access web interface"
    echo "IP will be displayed after 5 touch sensor presses"
    echo ""
else
    echo ""
    echo "========================================"
    echo "Flash failed!"
    echo "========================================"
    echo ""
    echo "Troubleshooting:"
    echo "1. Check ESP32 is connected"
    echo "2. Try different port (e.g., /dev/ttyUSB1)"
    echo "3. Check USB cable"
    echo "4. Install CH340/CP2102 driver"
    echo "5. Add user to dialout group: sudo usermod -a -G dialout \$USER"
    echo ""
fi
