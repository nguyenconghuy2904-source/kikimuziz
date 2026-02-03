# Build and Flash Script for ESP32 to COM31
# PowerShell version

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Building and Flashing Firmware to COM31" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Setup ESP-IDF environment
$idfPath = $null
if ($env:IDF_PATH) {
    $idfPath = $env:IDF_PATH
    Write-Host "ESP-IDF environment already set: $idfPath" -ForegroundColor Green
} else {
    Write-Host "Setting up ESP-IDF environment..." -ForegroundColor Yellow
    
    # Try common paths
    $paths = @(
        "D:\Espressif\frameworks\esp-idf-v5.5.1",
        "D:\Espressif\frameworks\esp-idf-v5.5",
        "C:\Espressif\frameworks\esp-idf-v5.5.1",
        "C:\Espressif\frameworks\esp-idf-v5.5",
        "$env:USERPROFILE\.espressif\esp-idf"
    )
    
    foreach ($path in $paths) {
        if (Test-Path "$path\export.bat") {
            $idfPath = $path
            Write-Host "ESP-IDF found at: $idfPath" -ForegroundColor Green
            break
        }
    }
    
    if (-not $idfPath) {
        Write-Host "ERROR: ESP-IDF v5.5 not found!" -ForegroundColor Red
        Write-Host ""
        Write-Host "Please install ESP-IDF v5.5 first:" -ForegroundColor Yellow
        Write-Host "  1. Install ESP-IDF Extension in Cursor/VSCode (recommended)"
        Write-Host "  2. Or download from: https://github.com/espressif/esp-idf/releases/tag/v5.5"
        Write-Host ""
        exit 1
    }
    
    # Setup ESP-IDF environment using cmd
    Write-Host "Activating ESP-IDF environment..." -ForegroundColor Yellow
    $env:IDF_PATH = $idfPath
    & cmd /c "`"$idfPath\export.bat`" && set" | ForEach-Object {
        if ($_ -match "^(.+?)=(.*)$") {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        }
    }
}

Write-Host ""
Write-Host "Step 1: Releasing COM31 port..." -ForegroundColor Cyan
Get-Process python, pythonw -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Write-Host "  COM port released" -ForegroundColor Green
Write-Host ""

Write-Host "Step 2: Building project..." -ForegroundColor Cyan
& idf.py build
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "Build failed!" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Step 3: Releasing COM31 before flashing..." -ForegroundColor Cyan
Get-Process python, pythonw -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Write-Host ""

Write-Host "Step 4: Flashing firmware to COM31..." -ForegroundColor Cyan
& idf.py -p COM31 flash
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "Flash failed!" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    Write-Host ""
    Write-Host "Troubleshooting:" -ForegroundColor Yellow
    Write-Host "  - Check if COM31 is correct port"
    Write-Host "  - Make sure ESP32 is connected"
    Write-Host "  - Try pressing BOOT button on ESP32"
    Write-Host "  - Check USB cable (use data cable, not charge-only)"
    Write-Host ""
    exit 1
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "Flash completed successfully!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "Opening monitor on COM31..." -ForegroundColor Cyan
Write-Host "Press Ctrl+] to exit monitor" -ForegroundColor Yellow
Write-Host ""

& idf.py -p COM31 monitor

