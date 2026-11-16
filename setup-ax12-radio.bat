@echo off
rem Double-click launcher for setup-ax12-radio.ps1 (prepares AX12 radios over USB debugging)
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-ax12-radio.ps1" %*
echo.
pause
