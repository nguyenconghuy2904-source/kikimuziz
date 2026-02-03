# 📦 Merge Firmware Binary Guide

## ✅ Đã tạo thành công!

File merge bin đã được tạo tại:
```
build/kiki_merged_flash.bin
```

**Kích thước:** ~8.4 MB  
**Flash address:** 0x0

---

## 🔧 Cách Flash

### **Cách 1: Dùng script có sẵn**
```batch
flash_merged_bin.bat
```

### **Cách 2: Dùng esptool trực tiếp**
```bash
python -m esptool -p COM31 -b 921600 write_flash 0x0 build/kiki_merged_flash.bin
```

### **Cách 3: Dùng ESP Flash Download Tool**
1. Mở ESP Flash Download Tool
2. Chọn chip: **ESP32-S3**
3. Thêm file: `kiki_merged_flash.bin` tại địa chỉ **0x0**
4. COM: **COM31**, Baud: **921600**
5. Click **START**

---

## 📋 Merge bin bao gồm:

| Offset | File | Description |
|--------|------|-------------|
| 0x0 | bootloader.bin | Bootloader |
| 0x8000 | partition-table.bin | Partition table |
| 0xd000 | ota_data_initial.bin | OTA data |
| 0x20000 | xiaozhi.bin | Main application |
| 0x800000 | generated_assets.bin | Assets (emojis, sounds) |

---

## 🔄 Tạo lại merge bin

Nếu cần tạo lại merge bin sau khi build:

```bash
# Cách 1: Dùng script
create_merged_bin.bat

# Cách 2: Dùng Python
python create_merged_bin.py

# Cách 3: Manual
cd build
python -m esptool --chip esp32s3 merge-bin \
    -o kiki_merged_flash.bin \
    --flash-mode dio \
    --flash-freq 80m \
    --flash-size 16MB \
    0x0 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0xd000 ota_data_initial.bin \
    0x20000 xiaozhi.bin \
    0x800000 generated_assets.bin
```

---

## ⚠️ Lưu ý

1. **Erase flash trước khi flash:**
   ```bash
   python -m esptool -p COM31 erase_flash
   ```

2. **Chỉ cần flash 1 file duy nhất** tại địa chỉ 0x0

3. **Tốc độ baud 921600** cho flash nhanh (có thể dùng 115200 nếu gặp lỗi)

4. **Dung lượng flash:** 16MB (check board phải có đủ 16MB)

---

## 🎯 Ưu điểm Merge Bin

✅ **Dễ phân phối:** Chỉ 1 file thay vì 5 files  
✅ **Flash đơn giản:** Chỉ cần chỉ định địa chỉ 0x0  
✅ **Ít lỗi:** Không lo lệch địa chỉ partition  
✅ **Production ready:** Phù hợp cho sản xuất hàng loạt  

---

## 📝 Troubleshooting

### **Lỗi "A fatal error occurred"**
- Giảm baud rate: `-b 115200`
- Hold BOOT button khi flash
- Check cable USB

### **Lỗi "File not found"**
- Chạy `create_merged_bin.bat` trước
- Check file tồn tại trong `build/`

### **Lỗi "esptool not found"**
```bash
python -m pip install esptool --user
```

---

## 🚀 Quick Flash Command

```bash
# Erase + Flash + Monitor
python -m esptool -p COM31 erase_flash && ^
python -m esptool -p COM31 -b 921600 write_flash 0x0 build/kiki_merged_flash.bin && ^
idf.py -p COM31 monitor
```
