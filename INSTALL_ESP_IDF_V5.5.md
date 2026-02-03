# Hướng dẫn cài đặt ESP-IDF v5.5

## Cách 1: Cài qua ESP-IDF Extension trong Cursor/VSCode (Khuyến nghị - Dễ nhất)

### Bước 1: Cài Extension
1. Mở Cursor/VSCode
2. Vào Extensions (Ctrl+Shift+X)
3. Tìm và cài **"ESP-IDF"** (by Espressif Systems)
4. Extension sẽ tự động hướng dẫn cài ESP-IDF

### Bước 2: Setup ESP-IDF
1. Sau khi cài extension, mở Command Palette (Ctrl+Shift+P)
2. Chọn: `ESP-IDF: Configure ESP-IDF extension`
3. Chọn version **v5.5** hoặc **v5.5.1**
4. Chọn cài đặt:
   - **Express** (nhanh, đủ dùng)
   - Hoặc **Advanced** (tùy chỉnh)
5. Chờ cài đặt hoàn tất (có thể mất 10-30 phút)

### Bước 3: Verify
1. Mở Command Palette (Ctrl+Shift+P)
2. Chọn: `ESP-IDF: Show Examples`
3. Nếu mở được examples thì đã cài thành công

## Cách 2: Cài thủ công qua ESP-IDF Installer

### Windows Installer (Khuyến nghị cho Windows)

1. **Download ESP-IDF v5.5 Installer:**
   - Link: https://dl.espressif.com/dl/esp-idf/
   - Chọn file: `esp-idf-tools-setup-online-5.5.exe` (hoặc version mới nhất)

2. **Chạy Installer:**
   - Chọn cài vào: `D:\Espressif` hoặc `C:\Espressif`
   - Chọn ESP-IDF version: **v5.5** hoặc **v5.5.1**
   - Chọn cài đặt Python, Git (nếu chưa có)
   - Chờ cài đặt hoàn tất

3. **Verify:**
   - Mở **ESP-IDF Command Prompt** từ Start Menu
   - Chạy: `idf.py --version`
   - Nếu hiển thị version thì đã cài thành công

### Manual Installation (Linux/Mac)

```bash
# Clone ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.5.1

# Install
./install.sh esp32,esp32s3,esp32c3,esp32c6,esp32p4

# Setup environment
. ./export.sh
```

## Cách 3: Cài qua Python (Advanced)

```bash
# Cài ESP-IDF Python package
pip install esp-idf

# Setup ESP-IDF v5.5
esp-idf install --version v5.5.1

# Verify
esp-idf --version
```

## Verify Installation

Sau khi cài xong, verify bằng cách:

```cmd
# Mở ESP-IDF Command Prompt hoặc terminal đã setup ESP-IDF
idf.py --version
```

Nếu hiển thị version 5.5.x thì đã cài thành công!

## Sau khi cài xong

1. **Nếu dùng Extension:** Mở project trong Cursor/VSCode, extension sẽ tự động detect ESP-IDF

2. **Nếu cài thủ công:** 
   - Mở **ESP-IDF Command Prompt** từ Start Menu
   - Hoặc chạy script `build_flash_com31.bat` (sẽ tự động setup)

3. **Build và Flash:**
   ```cmd
   cd "F:\New folder\xiaozhi-esp32-main_322\xiaozhi-esp32-main"
   idf.py build
   idf.py -p COM31 flash
   ```

## Troubleshooting

### Extension không tìm thấy ESP-IDF
- Đảm bảo đã cài ESP-IDF qua extension
- Thử restart Cursor/VSCode
- Check ESP-IDF path trong extension settings

### Build lỗi "idf.py not found"
- Mở ESP-IDF Command Prompt (không phải cmd thường)
- Hoặc chạy `export.bat` từ thư mục ESP-IDF trước

### Flash lỗi
- Kiểm tra COM port đúng chưa (COM31)
- Thử nhấn nút BOOT trên ESP32 khi flash
- Kiểm tra USB driver (CP210x, CH340)

## Links hữu ích

- ESP-IDF Releases: https://github.com/espressif/esp-idf/releases
- ESP-IDF Installer: https://dl.espressif.com/dl/esp-idf/
- ESP-IDF Docs: https://docs.espressif.com/projects/esp-idf/en/latest/

