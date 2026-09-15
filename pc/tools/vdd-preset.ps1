# vdd-preset.ps1 -- manage Parsec VDD custom resolution presets.
#
# The driver's built-in modes are all 16:9 landscape, so a portrait size such as
# 946x1440 (matching a 2800x1840 tablet) has to be added as a custom preset.
# Presets live in HKLM\SOFTWARE\Parsec\vdd\<0..5> as {width,height,hz} DWORDs,
# which requires administrator rights -- so this script self-elevates.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\tools\vdd-preset.ps1 list
#   powershell -ExecutionPolicy Bypass -File .\tools\vdd-preset.ps1 set 946x1440@60
#   powershell -ExecutionPolicy Bypass -File .\tools\vdd-preset.ps1 clear
#   powershell -ExecutionPolicy Bypass -File .\tools\vdd-preset.ps1 remove 1
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads a .ps1 without a
# BOM as ANSI; non-ASCII characters would break parsing.

param(
    [Parameter(Position = 0)][string]$Command = "list",
    [Parameter(Position = 1)][string]$Spec = ""
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projDir = Split-Path -Parent $scriptDir
$exe = Join-Path $projDir "subscreen_sender.exe"

if (-not (Test-Path $exe)) {
    Write-Host "[ERROR] sender not found: $exe"
    Write-Host "        build it first: powershell -ExecutionPolicy Bypass -File .\build.ps1"
    exit 1
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($isAdmin) {
    & $exe --vdd-preset $Command $Spec
    exit $LASTEXITCODE
}

Write-Host "Administrator rights are required to write HKLM\SOFTWARE\Parsec\vdd."
Write-Host "A UAC prompt will appear. Close the elevated window when you are done reading it."
Write-Host ""

# Run through cmd /k so the elevated output stays on screen.
$inner = "`"$exe`" --vdd-preset $Command"
if ($Spec -ne "") { $inner = "$inner $Spec" }
$p = Start-Process -FilePath "cmd.exe" -ArgumentList "/k", $inner -Verb RunAs -Wait -PassThru
exit 0
