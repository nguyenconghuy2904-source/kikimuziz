@echo off
REM Flash firmware to COM31

echo ========================================
echo Flashing firmware to COM31
echo ========================================
echo.

echo Step 1: Setting up ESP-IDF environment...
REM Try to find ESP-IDF in common locations
if exist "C:\Espressif\frameworks\esp-idf-v5.5\export.bat" (
    set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5
    call "%IDF_PATH%\export.bat"
) else if exist "C:\Espressif\frameworks\esp-idf-v5.5.1\export.bat" (
    set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.1
    call "%IDF_PATH%\export.bat"
) else if exist "D:\Espressif\frameworks\esp-idf-v5.5.1\export.bat" (
    set IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5.1
    call "%IDF_PATH%\export.bat"
) else if exist "D:\Espressif\frameworks\esp-idf-v5.5\export.bat" (
    set IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5
    call "%IDF_PATH%\export.bat"
) else (
    echo ERROR: ESP-IDF v5.5 not found!
    echo Please install ESP-IDF v5.5 first.
    pause
    exit /b 1
)
if errorlevel 1 (
    echo Failed to setup ESP-IDF environment!
    pause
    exit /b 1
)
echo   ESP-IDF environment ready
echo.

echo Step 2: Releasing COM port (killing Python processes)...
taskkill /F /IM python.exe /T >nul 2>&1
taskkill /F /IM pythonw.exe /T >nul 2>&1
timeout /t 1 /nobreak >nul
echo   COM port released
echo.

echo Step 3: Building project (if needed)...
if not exist "build\bootloader\bootloader.bin" (
    echo   Building project first...
    idf.py build
    if errorlevel 1 (
        echo.
        echo ========================================
        echo Build failed!
        echo ========================================
        pause
        exit /b 1
    )
) else (
    echo   Build files found, skipping build
)

echo.
echo Step 4: Releasing COM port before flashing...
taskkill /F /IM python.exe /T >nul 2>&1
taskkill /F /IM pythonw.exe /T >nul 2>&1
timeout /t 1 /nobreak >nul
echo.

echo Step 5: Flashing firmware to COM31...
idf.py -p COM31 flash
if errorlevel 1 (
    echo.
    echo ========================================
    echo Flash failed!
    echo ========================================
    pause
    exit /b 1
)

echo.
echo ========================================
echo Flash completed successfully!
echo ========================================
echo.
echo Opening monitor on COM31...
echo Press Ctrl+] to exit monitor
echo.
idf.py -p COM31 monitor

pause



