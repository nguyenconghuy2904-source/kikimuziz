# Build Script for Waveshare ESP32-P4-WIFI6-Touch-LCD-4B
# PowerShell version

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Building Waveshare P4 WiFi6 Touch LCD 4B" -ForegroundColor Cyan
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
        Write-Host "  1. Install ESP-IDF Extension in VSCode (recommended)"
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
Write-Host "Step 1: Verify Configuration..." -ForegroundColor Cyan
Write-Host "  Target: ESP32-P4" -ForegroundColor Green
Write-Host "  Board: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B" -ForegroundColor Green
Write-Host ""

Write-Host "Step 2: Cleaning build..." -ForegroundColor Cyan
Get-Process python, pythonw, cmake, ninja -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Write-Host "  Processes stopped" -ForegroundColor Green
Write-Host ""

Write-Host "Step 3: Building project..." -ForegroundColor Cyan
& idf.py build
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "Build failed!" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "Build completed successfully!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "Build output directory: build/" -ForegroundColor Yellow
Write-Host ""
Write-Host "To flash the firmware, run:" -ForegroundColor Yellow
Write-Host "  idf.py -p COMx flash" -ForegroundColor Cyan
Write-Host "  (Replace COMx with your actual port)" -ForegroundColor Gray
Write-Host ""
