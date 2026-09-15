@echo off
REM ---------------------------------------------------------------------------
REM start-mirror.cmd -- mirror the laptop's internal panel to the tablet.
REM                     [best balance: sharp AND responsive]
REM
REM   Uses 1920x1200 @ 45fps with the GPU doing BGRA->NV12 + scaling in one pass
REM   (D3D11 VideoProcessor). Only the small NV12 is read back, so CPU cost is
REM   gone and the hardware encoder sees a much smaller frame.
REM
REM   Measured (sender-side latency / dropped frames):
REM     1280x800  @60  CPU   19.3ms   was the old default -- visibly soft
REM     1920x1200 @45  GPU   20.4ms   ZERO dropped frames   <- this script
REM     1920x1200 @50  GPU   24.5ms   drops frames (encoder is maxed out)
REM     2048x1280 @45  GPU   21.8ms   drops a few frames
REM     2560x1600 @30  GPU   31.2ms   slow, but pixel-perfect 1:1
REM   The HARDWARE ENCODER is the bottleneck: pixel count x fps is what costs.
REM   Want maximum sharpness instead? Run start-mirror-sharp.cmd
REM
REM   PREREQUISITE: the panel must be ON. Press Win+P and pick Extend, or enable
REM   it in Display settings. Desktop Duplication can only capture an ACTIVE
REM   display -- if the panel is off, this script prints an error.
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

REM matched by EDID model -- survives resolution changes, unlike \\.\DISPLAYn.
REM run  subscreen_sender.exe --list  to see the keys for your own machine.
set SRC=NE160QDM

echo ============================================================
echo  SubScreen mirror  -  laptop panel to tablet
echo  mode    : balanced   (1920x1200 @ 45fps, GPU scaled)
echo  source  : %SRC%
echo  For max sharpness use start-mirror-sharp.cmd
echo  Keep this window open. Press Ctrl+C to stop.
echo ============================================================
echo.

subscreen_sender.exe --source %SRC% --size 1920x1200 --fps 45 %*

set RC=%ERRORLEVEL%
echo.
echo mirror exited with code %RC%
pause
endlocal
