# Hướng dẫn Upload Firmware lên GitHub Release

## File đã tạo
- **Merged Binary:** `build/merged-binary.bin` (~8.4 MB)
- **Zipped Release:** `releases/v2.0.5_kiki.zip` (~2.6 MB)

## Cách 1: Sử dụng Script (Tự động)

### Bước 1: Tạo GitHub Personal Access Token
1. Truy cập: https://github.com/settings/tokens
2. Click "Generate new token" → "Generate new token (classic)"
3. Đặt tên: "Firmware Upload"
4. Chọn scope: **repo** (full control of private repositories)
5. Click "Generate token"
6. Copy token (chỉ hiển thị 1 lần)

### Bước 2: Upload bằng script

**Windows (PowerShell):**
```powershell
$env:GITHUB_TOKEN="your_token_here"
python scripts/upload_release.py
```

**Hoặc với merged binary:**
```powershell
$env:GITHUB_TOKEN="your_token_here"
python scripts/upload_release.py --merged-bin
```

**Hoặc chỉ định file cụ thể:**
```powershell
$env:GITHUB_TOKEN="your_token_here"
python scripts/upload_release.py --file releases/v2.0.5_kiki.zip
```

## Cách 2: Upload thủ công qua GitHub Web

1. Truy cập: https://github.com/conghuy93/kikichatwiath_ai/releases
2. Click "Draft a new release" hoặc "Create a new release"
3. Điền thông tin:
   - **Tag:** `v2.0.5` (hoặc version mới)
   - **Title:** `Release v2.0.5`
   - **Description:** 
     ```
     Firmware release v2.0.5
     
     ## Features
     - QR code display for control panel
     - Auto-clear QR code after 30 seconds
     - Disabled auto-start webserver on boot
     - Updated ASR error handling
     ```
4. Kéo thả file `releases/v2.0.5_kiki.zip` vào phần "Attach binaries"
5. Click "Publish release"

## File locations
- Merged binary: `F:\xiaozhi-esp32-main\build\merged-binary.bin`
- Zipped release: `F:\xiaozhi-esp32-main\releases\v2.0.5_kiki.zip`

