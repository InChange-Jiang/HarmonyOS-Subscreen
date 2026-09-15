@echo off
REM ---------------------------------------------------------------------------
REM start-watch.cmd -- run SubScreen as a USB hot-plug daemon.
REM
REM   plug the tablet in  -> Windows gains a third display, streaming starts
REM   unplug the tablet   -> the virtual display is torn down automatically
REM
REM Keep this file ASCII-only (cmd.exe reads it with the OEM codepage).
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

if not exist "subscreen_sender.exe" (
    echo [ERROR] subscreen_sender.exe not found in "%CD%"
    echo         build it first:
    echo           powershell -ExecutionPolicy Bypass -File .\build.ps1
    exit /b 1
)

echo ============================================================
echo  SubScreen watcher - plug in the tablet, it becomes a screen
echo  Keep this window open. Press Ctrl+C to stop.
echo ============================================================
echo.

subscreen_sender.exe --watch %*

set RC=%ERRORLEVEL%
echo.
echo watcher exited with code %RC%
pause
endlocal
