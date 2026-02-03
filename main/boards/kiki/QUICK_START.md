# 🤖 Otto Robot - Quick Start Guide

## 🚀 Flash nhanh trong 2 bước

### Windows
```cmd
flash_firmware.bat COM24
```

### Linux/Mac  
```bash
chmod +x flash_firmware.sh
./flash_firmware.sh /dev/ttyUSB0
```

## 📱 Sau khi flash xong

1. **Kết nối WiFi**: Robot tự động kết nối WiFi đã cấu hình
2. **Xem IP address**: Chạm cảm biến touch 5 lần liên tiếp
3. **Truy cập Web**: Mở trình duyệt `http://[IP_ADDRESS]`
4. **Điều khiển**: Không cần password!

## 🎮 Các tính năng chính

### Web Interface
- ✅ Điều khiển robot qua web
- ✅ Điều chỉnh âm lượng (slider 0-100%)
- ✅ Không cần password
- ✅ Responsive design

### Wake Word
- 🎤 **"Hi ESP"** - Tiếng Anh
- 🎤 **"你好小智"** - Tiếng Trung

### Động tác Robot
- 🚶 Walk forward/backward
- ↪️ Turn left/right
- 🪑 Sit down
- 🤸 Jump, Dance, Bow
- 👋 Wave hand (sitting position)
- 🏠 Home position

## 🔧 Thông số kỹ thuật

- **Board**: otto-robot
- **Chip**: ESP32-S3 (240MHz)
- **RAM**: 512KB + 8MB PSRAM
- **Flash**: 16MB
- **WiFi**: 2.4GHz 802.11 b/g/n
- **Servos**: 4x (GPIO 17, 18, 12, 38)
- **Touch Sensor**: GPIO 2

## 📋 Cải tiến trong version này

### v2.0.3 (Oct 18, 2025)
- ✅ **Góc tiến/lùi mượt hơn**: 35°/145° (thay vì 80°/100°)
- ✅ **Vẫy tay trong tư thế ngồi**: Tự nhiên hơn
- ✅ **Web volume control**: Thanh điều chỉnh âm lượng
- ✅ **Touch IP display**: 5 lần chạm hiện IP
- ✅ **Bỏ hàm tin tức**: Tối ưu hiệu năng

## 🌐 Web Interface

### Truy cập
```
http://192.168.0.38
```
(IP có thể khác, xem bằng cách chạm cảm biến 5 lần)

### Các điều khiển
- **Volume Slider**: Kéo thanh để điều chỉnh 0-100%
- **Robot Actions**: Các nút điều khiển động tác
- **Status**: Xem trạng thái kết nối

## 🐛 Khắc phục sự cố

### Flash thất bại
1. Kiểm tra COM port đúng
2. Thử baud rate thấp hơn: `--baud 115200`
3. Cài driver CH340/CP2102
4. Thử cable USB khác

### Robot không hoạt động
1. Kiểm tra nguồn điện (5V, đủ dòng cho 4 servo)
2. Kiểm tra kết nối servo
3. Xem logs qua serial monitor

### WiFi không kết nối
1. Cấu hình lại WiFi qua web
2. Kiểm tra SSID/password
3. Reset cấu hình trong settings

## 📞 Support

- **GitHub**: [xiaozhi-esp32-otto-robot](https://github.com/nguyenconghuy2904-source/xiaozhi-esp32-otto-robot)
- **Docs**: Xem `FIRMWARE_FLASH_GUIDE.md`
- **Web Controller**: Xem `WEB_CONTROLLER_README.md`

## 🎉 Enjoy your Otto Robot!

Flash xong là có thể chơi ngay! Chạm cảm biến 5 lần để xem IP và truy cập web interface.
