@echo off
rem SlippWii: sets up your Wii's SD card for Slippi Online.
rem Put the SD card in this PC, then double-click this file.
title SlippWii installer
if not exist "%~dp0installer\start.ps1" (
    echo The installer's files are missing. Extract the whole zip first:
    echo right-click the zip file, choose "Extract All...", then open the
    echo extracted folder and double-click "Install SlippWii.bat" there.
    echo.
    pause
    exit /b 1
)
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" (
    echo Windows PowerShell was not found. The installer needs Windows 10 or 11.
    echo.
    pause
    exit /b 1
)
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\start.ps1" "%~1"
echo.
echo You can close this window now.
pause
