@echo off
REM ---------------------------------------------------------------------------
REM diagnose-leak.cmd -- memory-leak bisection, capture path ONLY.
REM
REM   Runs 3 minutes of capture(+GPU scale) with NO encoder, NO network.
REM   Every 2s it appends "stats(noencode): capFrames=N commit=NNNNMB"
REM   to crash_probe.log (same folder).
REM
REM   Verdict:
REM     commit grows ~4-5MB per frame  -> leak is in the capture/GPU path
REM     commit stays flat              -> leak is in the encoder (MFT)
REM
REM Keep this file ASCII-only (cmd.exe reads it with the OEM codepage).
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"
echo Running capture-only leak test for 180 seconds. Please wait...
subscreen_sender.exe --no-encode --source NE160QDM --size 1920x1200 --duration 180
echo.
echo Done. crash_probe.log now has the memory curve. Send it back / just tell me.
pause
endlocal
