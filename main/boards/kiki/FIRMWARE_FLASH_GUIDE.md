# Otto Robot Firmware - Flash Guide

## 📦 Các file firmware

### 1. **firmware-complete.bin** (Merged Binary - Khuyên dùng)
- **Kích thước**: ~8.77MB
- **Địa chỉ flash**: 0x0
- **Mô tả**: File binary hoàn chỉnh đã merge tất cả partitions
- **Cách flash đơn giản nhất**

### 2. **xiaozhi.bin** (App Binary)
- **Kích thước**: ~3.49MB  
- **Địa chỉ flash**: 0x20000
- **Mô tả**: Application binary (cần flash kèm bootloader và partition table)

## 🚀 Cách flash firmware

### Phương pháp 1: Flash Complete Binary (Đơn giản nhất)

```bash
esptool.py --chip esp32s3 --port COM24 --baud 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x0 firmware-complete.bin
```

### Phương pháp 2: Flash từng phần (Advanced)

```bash
esptool.py --chip esp32s3 --port COM24 --baud 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 ../../../build_otto/bootloader/bootloader.bin \
  0x8000 ../../../build_otto/partition_table/partition-table.bin \
  0xd000 ../../../build_otto/ota_data_initial.bin \
  0x20000 xiaozhi.bin \
  0x800000 ../../../build_otto/generated_assets.bin
```

### Phương pháp 3: Sử dụng ESP-IDF

```bash
cd C:\Users\congh\Downloads\Compressed\xiaozhi-esp32-2.0.3otto2\xiaozhi-esp32-2.0.3
idf.py -p COM24 -B build_otto flash
```

## 🔧 Các tính năng trong firmware này

### ✅ Cải tiến động tác
- **Góc tiến/lùi**: Giảm xuống 35°/145° (nhẹ nhàng hơn)
- **Động tác vẫy tay**: Thực hiện trong tư thế ngồi

### ✅ Giao diện Web
- **URL**: http://192.168.0.38 (sau khi kết nối WiFi)
- **Không cần password**
- **Điều khiển âm lượng**: Thanh slider 0-100%
- **Touch sensor**: Chạm 5 lần liên tiếp hiển thị IP

### ✅ Wake Word
- **"Hi ESP"**: wn9s_hiesp
- **"你好小智"**: wn9_nihaoxiaozhi_tts
- **AFE Audio Pipeline**: Với VAD và WakeNet

### ✅ Động tác robot
- walk_forward, walk_backward
- turn_left, turn_right  
- sit_down, lie_down
- jump, bow, dance
- wave_right_foot (trong tư thế ngồi)
- và nhiều động tác khác...

## 📋 Thông số kỹ thuật

- **Chip**: ESP32-S3
- **PSRAM**: 8MB
- **Flash**: 16MB
- **Board**: otto-robot
- **Version**: 2.0.3
- **Build date**: Oct 18 2025 23:40:31

## 🔗 Pin Configuration

- **LEFT_LEG_PIN** (Left Front): GPIO 17
- **RIGHT_LEG_PIN** (Right Front): GPIO 18
- **LEFT_FOOT_PIN** (Left Back): GPIO 12
- **RIGHT_FOOT_PIN** (Right Back): GPIO 38
- **Touch Sensor**: GPIO 2

## 📝 Lưu ý

1. Sử dụng baud rate 460800 cho flash nhanh, hoặc 115200 nếu gặp lỗi
2. Đảm bảo ESP32 đã vào chế độ download (tự động)
3. Sau khi flash xong, robot sẽ tự reset và khởi động
4. Kết nối WiFi để truy cập web interface

## 🐛 Troubleshooting

- **Lỗi kết nối COM port**: Kiểm tra driver CH340/CP2102
- **Flash timeout**: Giảm baud rate xuống 115200
- **Robot không hoạt động**: Kiểm tra nguồn điện và servo connections
