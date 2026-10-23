@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ==========================================
echo   GCU OLED HUD - ESP8266 flash tool
echo ==========================================
echo.

echo [1/3] 检测串口...
python -c "import serial.tools.list_ports as l;ps=list(l.comports());[print('   ',p.device,'-',p.description) for p in ps];print('   (没有发现串口，请检查USB线/驱动)') if not ps else None"
echo.

set PORT=%~1
if "%PORT%"=="" set /p PORT=请输入串口号(例如 COM5):

echo.
echo [2/3] 选择固件：
echo   1 = 4MB flash  (NodeMCU / Wemos D1 mini / ESP-12F)  默认
echo   2 = 1MB flash  (ESP-01 / ESP-01S)
set CH=1
set /p CH=直接回车用默认:

set BIN=固件\GCU_OLED_8266_4MB.bin
if "%CH%"=="2" set BIN=固件\GCU_OLED_8266_1MB.bin
if not exist "%BIN%" (
  echo 找不到固件：%BIN%
  pause
  exit /b 1
)

echo.
echo [3/3] 开始烧录 %BIN% -^> %PORT%
echo     如果卡在 Connecting 请按住 FLASH 键（GPIO0 接地）再试
echo.
python -m esptool --port %PORT% --baud 460800 --after hard-reset write-flash 0x0 "%BIN%"

echo.
if errorlevel 1 (
  echo 烧录失败。可先执行整片擦除再重试：
  echo    python -m esptool --port %PORT% erase-flash
) else (
  echo 烧录完成，板子已自动重启。
)
pause
