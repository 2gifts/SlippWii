@echo off
rem SlippWii: copies your Slippi account (from Slippi Launcher on this PC) to
rem the SD card. Double-click it on the SD card, after extracting SlippWii there.
title SlippWii: copy your Slippi account
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%~dp0slippwii\copy-account.ps1" (
    echo This file belongs on the SD card, next to the slippwii folder.
    echo Extract the whole SlippWii zip onto your SD card, then run it from there.
    echo.
    pause
    exit /b 1
)
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0slippwii\copy-account.ps1" -Here "%~dp0."
echo.
pause
