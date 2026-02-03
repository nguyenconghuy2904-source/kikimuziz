@echo off
REM Simple build script with Python 3.11
echo Building with Python 3.11...

REM Setup ESP-IDF environment using official export.bat
call C:\Espressif\frameworks\esp-idf-v5.5\export.bat

REM Force Python 3.11
set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;%PATH%

cd /d "%~dp0"

echo Killing processes...
taskkill /F /IM python.exe /T >nul 2>&1
taskkill /F /IM cmake.exe /T >nul 2>&1
taskkill /F /IM ninja.exe /T >nul 2>&1
timeout /t 2 /nobreak >nul

echo.
echo Building project with Python 3.11...
python --version
idf.py build

if errorlevel 1 (
    echo Build failed!
    pause
    exit /b 1
)

echo.
echo Build successful!
echo.
echo Flashing to COM31...
idf.py -p COM31 flash monitor

pause
