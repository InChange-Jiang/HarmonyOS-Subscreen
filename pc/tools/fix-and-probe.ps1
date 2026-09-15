# fix-and-probe.ps1 -- one-shot (elevated) repair + verification for the
# "driver accepts ADD but no desktop output ever appears" symptom.
#
# It does, in order:
#   1) clear ALL custom resolution presets, then write exactly one correct
#      portrait preset: 946x1440@60
#   2) re-list the presets so the result is visible
#   3) run --vdd-probe: create a virtual display and watch whether Windows
#      actually attaches a new output to the desktop
#
# Why elevation: the presets live in HKLM\SOFTWARE\Parsec\vdd, which is
# administrator-only. The script re-launches itself elevated, so UAC fires once.
#
# Usage (from the project root):
#   powershell -ExecutionPolicy Bypass -File .\tools\fix-and-probe.ps1
#
# The elevated window stays open (-NoExit) so you can read/copy the output.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads a .ps1 without a
# BOM as ANSI; non-ASCII characters would break parsing.

param(
    [Parameter(Position = 0)][string]$Spec = "946x1440@60"
)

$ErrorActionPreference = "Stop"

$scriptPath = $MyInvocation.MyCommand.Path
$scriptDir  = Split-Path -Parent $scriptPath
$projDir    = Split-Path -Parent $scriptDir
$exe        = Join-Path $projDir "subscreen_sender.exe"

if (-not (Test-Path $exe)) {
    Write-Host "[ERROR] sender not found: $exe"
    Write-Host "        build it first: powershell -ExecutionPolicy Bypass -File .\build.ps1"
    exit 1
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "Administrator rights are required to rewrite HKLM\SOFTWARE\Parsec\vdd."
    Write-Host "A UAC prompt will appear now. The elevated window stays open afterwards."
    Write-Host ""
    Start-Process -FilePath "powershell.exe" `
        -ArgumentList @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit",
            "-File", "`"$scriptPath`"", "-Spec", $Spec
        ) -Verb RunAs | Out-Null
    exit 0
}

# ------------------------------- elevated path -------------------------------

$log = Join-Path $projDir "tools\fix-and-probe.log"
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("=== SubScreen VDD repair ===")
$lines.Add("time   : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
$lines.Add("admin  : yes")
$lines.Add("spec   : $Spec")
$lines.Add("")

function Run-Step([string]$title, [string[]]$argv) {
    $lines.Add("---- $title ----")
    $out = & $exe @argv 2>&1
    foreach ($l in $out) { $lines.Add($l) }
    $lines.Add("(exit=$LASTEXITCODE)")
    $lines.Add("")
    foreach ($l in $out) { Write-Host $l }
    Write-Host "(exit=$LASTEXITCODE)"
    Write-Host ""
}

Run-Step "before: current presets" @("--vdd-preset", "list")
Run-Step "step 1/3: clear + write preset $Spec" @("--vdd-preset", "set", $Spec)
Run-Step "step 2/3: presets now" @("--vdd-preset", "list")
Run-Step "step 3/3: probe (create virtual display)" @("--vdd-probe")

# ---- verdict ----
$text  = $lines -join "`n"
$lines.Add("---- verdict ----")
if ($text -match "\u865a\u62df\u663e\u793a\u5668\u5df2\u5c31\u4f4d") {
    $lines.Add("RESULT: OK - a real desktop output was created. The preset fix worked.")
} elseif ($text -match "\u6ca1\u6709\u65b0\u8f93\u51fa\u6302\u4e0a\u684c\u9762") {
    $lines.Add("RESULT: STILL FAILING - driver accepted ADD but no output appeared.")
    $lines.Add("NEXT: disable the other virtual display adapters (GameViewer / spacedesk)")
    $lines.Add("      and re-run the probe.")
} else {
    $lines.Add("RESULT: inconclusive - read the log above.")
}
foreach ($l in $lines | Select-Object -Last 6) { Write-Host $l }

$lines -join "`r`n" | Out-File -FilePath $log -Encoding UTF8
Write-Host ""
Write-Host "log saved to: $log"
# This window was started with -NoExit, so it stays open for reading/copying.
