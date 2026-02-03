# 🤖 Otto Robot - Documentation Index

## 📖 Bắt đầu nhanh (Start Here!)

1. **🚀 [QUICK_START.md](QUICK_START.md)** - Flash và sử dụng ngay trong 2 phút!
   - Hướng dẫn flash nhanh
   - Cách truy cập web interface
   - Các tính năng chính

## 📚 Tài liệu chi tiết

### Firmware & Flash
- **📦 [DIRECTORY_INFO.md](DIRECTORY_INFO.md)** - Cấu trúc thư mục và file
- **🔧 [FIRMWARE_FLASH_GUIDE.md](FIRMWARE_FLASH_GUIDE.md)** - Hướng dẫn flash chi tiết
- **📋 [firmware_info.json](firmware_info.json)** - Thông tin firmware (JSON)

### Điều khiển & Sử dụng
- **🌐 [WEB_CONTROLLER_README.md](WEB_CONTROLLER_README.md)** - Giao diện web
- **🤖 [README.md](README.md)** - Tài liệu Otto Robot (Tiếng Trung)

## 🛠️ Flash Tools

### Windows Users
```cmd
flash_firmware.bat COM24
```

### Linux/Mac Users
```bash
chmod +x flash_firmware.sh
./flash_firmware.sh /dev/ttyUSB0
```

## 📦 Firmware Files

| File | Size | Description | Flash Address |
|------|------|-------------|---------------|
| **firmware-complete.bin** | 8.77MB | ✅ **Merged binary (Khuyên dùng)** | 0x0 |
| xiaozhi.bin | 3.49MB | Application only | 0x20000 |
| firmware.bin | - | Backup firmware | - |

## ✨ Tính năng v2.0.3

### 🎯 Cải tiến động tác
- ✅ Góc đi/lùi mượt: 35°/145° (nhẹ nhàng hơn)
- ✅ Vẫy tay khi ngồi (tự nhiên hơn)

### 🌐 Web Interface
- ✅ Điều khiển âm lượng (slider 0-100%)
- ✅ Hiển thị IP (chạm 5 lần)
- ✅ Không cần password

### 🎤 Wake Word
- ✅ "Hi ESP" (English)
- ✅ "你好小智" (Chinese)

### 🔌 AI Integration
- ✅ 26 động tác MCP
- ✅ MQTT support
- ✅ WiFi station mode

## 🎮 Robot Actions

### Di chuyển cơ bản
```
walk_forward, walk_backward
turn_left, turn_right
sit_down, lie_down, jump
```

### Múa & Biểu diễn
```
dance, dance_4_feet
swing, stretch, bow
wave_right_foot
```

### Chiến đấu & Biểu cảm
```
defend, attack, celebrate
greet, scratch, retreat, search
```

## 🔌 Pin Configuration

```
LEFT_LEG_PIN     = GPIO 17  (Left Front)
RIGHT_LEG_PIN    = GPIO 18  (Right Front)
LEFT_FOOT_PIN    = GPIO 12  (Left Back)
RIGHT_FOOT_PIN   = GPIO 38  (Right Back)
TOUCH_SENSOR     = GPIO 2   (Touch Input)
```

## 🐛 Khắc phục sự cố

### Flash lỗi?
1. ✅ Kiểm tra COM port
2. ✅ Thử baud thấp hơn: `--baud 115200`
3. ✅ Cài driver CH340/CP2102
4. ✅ Thử cable USB khác

### Robot không hoạt động?
1. ✅ Kiểm tra nguồn 5V (≥3A)
2. ✅ Kiểm tra kết nối servo
3. ✅ Xem serial monitor logs

### Không thấy IP?
1. ✅ Chạm cảm biến 5 lần
2. ✅ Kiểm tra WiFi đã kết nối
3. ✅ Xem serial monitor

## 📞 Support & Links

- **GitHub**: [xiaozhi-esp32-otto-robot](https://github.com/nguyenconghuy2904-source/xiaozhi-esp32-otto-robot)
- **Otto DIY**: [ottodiy.tech](https://www.ottodiy.tech)
- **Hardware**: [立创开源](https://oshwhub.com/txp666/ottorobot)

## 🎉 Quick Commands

### Flash firmware
```bash
# Windows
flash_firmware.bat COM24

# Linux/Mac
./flash_firmware.sh /dev/ttyUSB0

# Direct esptool
python -m esptool --chip esp32s3 --port COM24 \
  --baud 460800 write_flash 0x0 firmware-complete.bin
```

### Access web interface
```
1. Touch sensor 5 times to see IP
2. Open browser: http://[IP_ADDRESS]
3. Control robot (no password!)
```

---

**🚀 Bắt đầu ngay**: [QUICK_START.md](QUICK_START.md)

**📖 Đọc thêm**: [FIRMWARE_FLASH_GUIDE.md](FIRMWARE_FLASH_GUIDE.md)
