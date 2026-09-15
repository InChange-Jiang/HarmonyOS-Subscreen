@echo off
REM ---------------------------------------------------------------------------
REM SubScreen-GUI.cmd -- launch the SubScreen control window.
REM
REM   Double-click this file (or subscreen_sender.exe with no arguments).
REM   The console window flashes for a moment, then the GUI takes over.
REM
REM   In the window: press "Start" to
REM     1) verify the tablet is connected and its app is running
REM     2) add the laptop panel to the desktop (two-screen extend)
REM     3) start streaming the panel to the tablet
REM   Press "Stop" to end streaming and restore the single-display layout.
REM
REM Keep this file ASCII-only (cmd.exe reads it with the OEM codepage).
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

if not exist "subscreen_sender.exe" (
    echo [ERROR] subscreen_sender.exe not found in "%CD%"
    echo         build it first:
    echo           powershell -ExecutionPolicy Bypass -File .\build.ps1
    pause
    exit /b 1
)

start "" "subscreen_sender.exe"
endlocal
