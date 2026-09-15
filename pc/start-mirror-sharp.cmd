@echo off
REM ---------------------------------------------------------------------------
REM start-mirror-sharp.cmd -- mirror the laptop's internal panel to the tablet.
REM                           [sharpness first]
REM
REM   Uses 2560x1600 @ 30fps: full panel resolution, no downscale.
REM   Measured sender-side latency 31ms at a steady 30fps with zero dropped
REM   frames. The tablet decodes 2560x1600, which is the heavy part.
REM
REM   If it feels laggy, use start-mirror.cmd (1920x1200@45, GPU scaled) instead
REM   -- same latency ballpark as the old 1280x800@60 but far clearer.
REM
REM   PREREQUISITE: the panel must be ON. Press Win+P and pick Extend.
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

set SRC=NE160QDM

echo ============================================================
echo  SubScreen mirror  -  laptop panel to tablet
echo  mode    : sharpness first        (2560x1600 @ 30fps)
echo  source  : %SRC%
echo  Keep this window open. Press Ctrl+C to stop.
echo ============================================================
echo.

subscreen_sender.exe --source %SRC% --size 2560x1600 --fps 30 %*

set RC=%ERRORLEVEL%
echo.
echo mirror exited with code %RC%
pause
endlocal
