# reload-vdd.ps1 -- restart the Parsec Virtual Display Adapter so it re-reads the
# custom resolution presets from HKLM\SOFTWARE\Parsec\vdd.
#
# WHY THIS IS NEEDED
#   Parsec's own documentation: presets are consumed by the driver, and
#   "To apply the resolution change, disconnect and reconnect ... or disable and
#   enable the Virtual Display Adapter inside device manager".
#   In practice writing the registry key alone is NOT enough -- the 946x1440
#   portrait preset sits in the registry but never shows up in the display's mode
#   list until the adapter is cycled.
#
# USAGE (from the project root):
#   powershell -ExecutionPolicy Bypass -File .\tools\reload-vdd.ps1
#
# Self-elevating: UAC fires once. The window stays open (-NoExit) so the output
# can be read; a copy is also written to tools\reload-vdd.log.
#
# SIDE EFFECT: the screen may flicker for a second or two while the adapter is
# cycled. No virtual display is active at that moment (no stream running), so the
# desktop layout is not expected to change.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads a .ps1 without a
# BOM as ANSI; non-ASCII characters would break parsing.

$ErrorActionPreference = "Stop"

$scriptPath = $MyInvocation.MyCommand.Path
$scriptDir  = Split-Path -Parent $scriptPath
$projDir    = Split-Path -Parent $scriptDir
$exe        = Join-Path $projDir "subscreen_sender.exe"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Administrator rights are required to cycle the display adapter."
    Write-Host "A UAC prompt will appear now. The elevated window stays open."
    Write-Host ""
    Start-Process -FilePath "powershell.exe" -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit",
        "-File", "`"$scriptPath`""
    ) -Verb RunAs | Out-Null
    exit 0
}

$log = Join-Path $scriptDir "reload-vdd.log"
function Say([string]$s) {
    Write-Host $s
    Add-Content -Path $log -Value $s -Encoding UTF8
}
Set-Content -Path $log -Value ("=== reload-vdd  " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + " ===") -Encoding UTF8

$dev = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
       Where-Object { $_.FriendlyName -match 'Parsec' -and $_.FriendlyName -match 'Virtual' } |
       Select-Object -First 1
if (-not $dev) {
    Say "[ERROR] Parsec Virtual Display Adapter not found in the Display class."
    Say "        Is the driver installed? Check Device Manager > Display adapters."
    exit 1
}
Say ("adapter : " + $dev.FriendlyName)
Say ("instance: " + $dev.InstanceId)
Say ""

Say "1/3 disabling adapter ..."
Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
Start-Sleep -Seconds 2

Say "2/3 enabling adapter ..."
Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false

Say "3/3 waiting for it to come back ..."
$ok = $false
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 1000
    $d = Get-PnpDevice -InstanceId $dev.InstanceId -ErrorAction SilentlyContinue
    if ($d -and $d.Status -eq 'OK') { $ok = $true; break }
}
if ($ok) {
    Say "     adapter is back (OK) -- presets should be loaded now."
} else {
    Say "     [WARN] adapter did not report OK within 30s. Check Device Manager."
}

if (Test-Path $exe) {
    Say ""
    Say "presets in HKLM\SOFTWARE\Parsec\vdd right now:"
    (& $exe --vdd-preset list 2>&1) | ForEach-Object { Say ("  " + $_) }
}

Say ""
Say "Done. Next step (no admin needed):"
Say "  cd `"$projDir`""
Say "  .\subscreen_sender.exe --vdd 946x1440@60 --side left"
Say ""
Say "If the log then reports using 946x1440 as the initial portrait size, the preset took effect."
# The window was started with -NoExit, so it stays open for reading/copying.
