# 📁 Otto Robot - Directory Structure

## 📂 Files Overview

### 🔧 Source Code Files
- `otto_robot.cc` - Main robot implementation
- `otto_controller.cc` - Servo control and action queue
- `otto_movements.cc` - Movement definitions (walk, jump, dance, etc.)
- `otto_webserver.cc` - Web interface server
- `otto_emoji_display.cc` - Display and emoji management
- `oscillator.cc` - Servo oscillator for smooth movements
- `config.h` - Pin configuration and constants
- `config.json` - Board configuration

### 📦 Firmware Files
- `firmware-complete.bin` (8.77MB) - **Complete merged binary** ✅ Recommended
- `xiaozhi.bin` (3.49MB) - Application binary only
- `firmware.bin` - Backup/alternative firmware

### 📝 Documentation
- `README.md` - Chinese documentation (original)
- `QUICK_START.md` - Quick start guide (Vietnamese)
- `FIRMWARE_FLASH_GUIDE.md` - Detailed flash instructions
- `WEB_CONTROLLER_README.md` - Web interface guide
- `firmware_info.json` - Firmware metadata

### 🛠️ Flash Tools
- `flash_firmware.bat` - Windows flash script
- `flash_firmware.sh` - Linux/Mac flash script  
- `flash_args.txt` - ESP-IDF flash arguments

## 🚀 Quick Flash

### Method 1: Use Flash Scripts (Easiest)

**Windows:**
```cmd
flash_firmware.bat COM24
```

**Linux/Mac:**
```bash
chmod +x flash_firmware.sh
./flash_firmware.sh /dev/ttyUSB0
```

### Method 2: Direct esptool

```bash
python -m esptool --chip esp32s3 --port COM24 --baud 460800 \
  write_flash 0x0 firmware-complete.bin
```

### Method 3: ESP-IDF (From project root)

```bash
cd ../../../../
idf.py -p COM24 -B build_otto flash
```

## 📊 Version Information

**Current Version**: 2.0.3  
**Build Date**: 2025-10-18 23:40:31  
**Board**: otto-robot  
**Chip**: ESP32-S3

## ✨ Features in This Build

### Movement Improvements
- ✅ Softer walk angles: 35°/145° (was 80°/100°)
- ✅ Wave hand in sitting position
- ✅ Smoother servo movements

### Web Interface
- ✅ Volume control slider (0-100%)
- ✅ Touch sensor IP display (5 touches)
- ✅ No password required
- ✅ Real-time robot control

### Wake Word
- ✅ "Hi ESP" (English)
- ✅ "你好小智" (Chinese)

### AI Integration
- ✅ MCP (Model Context Protocol) support
- ✅ 26 robot actions via MCP
- ✅ MQTT connectivity
- ✅ WiFi station mode

## 🔌 Pin Configuration

```cpp
LEFT_LEG_PIN     = GPIO 17  // Left Front Servo
RIGHT_LEG_PIN    = GPIO 18  // Right Front Servo
LEFT_FOOT_PIN    = GPIO 12  // Left Back Servo
RIGHT_FOOT_PIN   = GPIO 38  // Right Back Servo
TOUCH_SENSOR     = GPIO 2   // Touch input
```

## 📱 After Flashing

1. **Power on** - Robot boots and initializes servos
2. **Connect WiFi** - Auto-connects to configured network
3. **Touch sensor 5x** - Display shows IP address
4. **Open browser** - Navigate to `http://[IP_ADDRESS]`
5. **Control robot** - Use web interface (no password!)

## 🎮 Available Robot Actions

### Basic Movements
- `walk_forward` / `walk_backward`
- `turn_left` / `turn_right`
- `sit_down` / `lie_down`
- `jump` / `home` / `stop`

### Dance & Tricks
- `dance` / `dance_4_feet`
- `swing` / `stretch`
- `bow` / `wave_right_foot`

### Combat & Expression
- `defend` / `attack`
- `celebrate` / `greet`
- `scratch` / `retreat` / `search`

## 🐛 Troubleshooting

### Flash Issues
- **Port not found**: Check device manager for COM port
- **Connection timeout**: Try slower baud: `--baud 115200`
- **Permission denied**: Run as administrator (Windows) or add to dialout group (Linux)

### Robot Issues
- **Servos not moving**: Check power supply (5V, 3A+ recommended)
- **Erratic movement**: Check servo connections
- **No WiFi**: Reconfigure via web interface

### Web Interface
- **Can't find IP**: Touch sensor 5 times to display
- **Can't connect**: Check robot and PC on same network
- **Volume not working**: Normal - software volume only

## 📚 Full Documentation

- **Chinese Guide**: See `README.md`
- **Flash Guide**: See `FIRMWARE_FLASH_GUIDE.md`
- **Quick Start**: See `QUICK_START.md`
- **Web Controller**: See `WEB_CONTROLLER_README.md`

## 🔗 Links

- **GitHub**: [xiaozhi-esp32-otto-robot](https://github.com/nguyenconghuy2904-source/xiaozhi-esp32-otto-robot)
- **Original Otto**: [ottodiy.tech](https://www.ottodiy.tech)
- **Hardware**: [立创开源](https://oshwhub.com/txp666/ottorobot)

## 📝 Changelog

### v2.0.3 (2025-10-18)
- Removed news function
- Reduced movement angles to 35°/145°
- Wave hand action in sitting position  
- Added web volume control slider
- Added touch sensor IP display (5 touches)
- Improved servo smoothness
- Fixed Application reference in webserver

---

**Ready to use!** Flash the firmware and start controlling your Otto Robot! 🎉
