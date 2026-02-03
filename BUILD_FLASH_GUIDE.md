# Hướng dẫn Build và Flash Firmware

## Yêu cầu

1. **ESP-IDF v5.4 trở lên** - Cần cài đặt ESP-IDF development framework
2. **Python 3.8+** - Đã được cài cùng ESP-IDF
3. **ESP32 device** - Kết nối qua USB

## Cách 1: Sử dụng ESP-IDF Extension trong Cursor/VSCode (Khuyến nghị)

1. Cài đặt extension **ESP-IDF** trong Cursor/VSCode
2. Mở project trong Cursor/VSCode
3. Extension sẽ tự động setup ESP-IDF environment
4. Sử dụng Command Palette (Ctrl+Shift+P):
   - `ESP-IDF: Build your project` - Để build
   - `ESP-IDF: Flash your project` - Để flash
   - `ESP-IDF: Monitor your device` - Để xem log

## Cách 2: Sử dụng ESP-IDF Command Prompt

1. Mở **ESP-IDF Command Prompt** (từ Start Menu sau khi cài ESP-IDF)
2. Chuyển đến thư mục project:
   ```cmd
   cd "F:\New folder\xiaozhi-esp32-main_322\xiaozhi-esp32-main"
   ```
3. Build project:
   ```cmd
   idf.py build
   ```
4. Flash firmware (thay COM3 bằng COM port của bạn):
   ```cmd
   idf.py -p COM3 flash
   ```
5. Mở monitor để xem log:
   ```cmd
   idf.py -p COM3 monitor
   ```

## Cách 3: Sử dụng script tự động

1. Chạy file `build_and_flash.bat`:
   ```cmd
   build_and_flash.bat
   ```
2. Script sẽ tự động:
   - Setup ESP-IDF environment (nếu tìm thấy)
   - Build project
   - Hỏi COM port và flash firmware
   - Mở monitor

## Tìm COM Port

- **Windows**: Mở Device Manager → Ports (COM & LPT) → Tìm "USB Serial Port" hoặc "Silicon Labs CP210x"
- Hoặc chạy: `idf.py flash` (không chỉ định port) để tự động tìm

## Lưu ý

- Nếu build lỗi, đảm bảo ESP-IDF đã được cài đặt đúng cách
- Nếu flash lỗi, kiểm tra:
  - COM port đúng chưa
  - Device đã kết nối chưa
  - Driver USB đã cài chưa (CP210x, CH340, etc.)
  - Nhấn nút BOOT trên ESP32 khi flash (nếu cần)

## Troubleshooting

### ESP-IDF không tìm thấy
- Cài ESP-IDF Extension trong Cursor/VSCode
- Hoặc cài ESP-IDF thủ công từ: https://github.com/espressif/esp-idf

### Build lỗi
- Kiểm tra ESP-IDF version (cần v5.4+)
- Chạy `idf.py fullclean` rồi build lại

### Flash lỗi
- Thử nhấn nút RESET trên ESP32
- Thử nhấn nút BOOT + RESET cùng lúc khi flash
- Kiểm tra cable USB (dùng cable data, không phải chỉ sạc)

