# install-autostart.ps1 -- run the SubScreen USB hot-plug daemon at logon.
#
# Registers a per-user Scheduled Task that starts `subscreen_sender.exe --watch`
# at logon, hidden, with output appended to logs\watch.log.
# No administrator rights needed (watch mode never touches HKLM).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\tools\install-autostart.ps1
#   powershell -ExecutionPolicy Bypass -File .\tools\install-autostart.ps1 -Status
#   powershell -ExecutionPolicy Bypass -File .\tools\install-autostart.ps1 -Remove
#
# NOTE: keep this file ASCII-only (PowerShell 5.1 reads no-BOM .ps1 as ANSI).

param(
    [switch]$Remove,
    [switch]$Status,
    [string]$TaskName = "SubScreenWatcher"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projDir = Split-Path -Parent $scriptDir
$exe = Join-Path $projDir "subscreen_sender.exe"
$logDir = Join-Path $projDir "logs"
$log = Join-Path $logDir "watch.log"

if ($Status) {
    $t = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if ($null -eq $t) {
        Write-Host "not installed (no task named '$TaskName')"
    } else {
        $info = Get-ScheduledTaskInfo -TaskName $TaskName -ErrorAction SilentlyContinue
        Write-Host "task    : $TaskName"
        Write-Host "state   : $($t.State)"
        Write-Host "action  : $($t.Actions[0].Execute) $($t.Actions[0].Arguments)"
        if ($null -ne $info) { Write-Host "last run: $($info.LastRunTime)  result=$($info.LastTaskResult)" }
    }
    exit 0
}

if ($Remove) {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Host "removed scheduled task '$TaskName'"
    } else {
        Write-Host "nothing to remove"
    }
    exit 0
}

if (-not (Test-Path $exe)) {
    Write-Host "[ERROR] sender not found: $exe"
    Write-Host "        build it first: powershell -ExecutionPolicy Bypass -File .\build.ps1"
    exit 1
}
if (-not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}

# Launch hidden via powershell so no console window flashes at logon, and append
# output to a log file (the daemon has no UI of its own).
$inner = "& '$exe' --watch *>> '$log'"
$action = New-ScheduledTaskAction -Execute "powershell.exe" `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command `"$inner`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null

Write-Host "installed scheduled task '$TaskName' (runs at logon, hidden)."
Write-Host "  log    : $log"
Write-Host "  status : powershell -ExecutionPolicy Bypass -File .\tools\install-autostart.ps1 -Status"
Write-Host "  remove : powershell -ExecutionPolicy Bypass -File .\tools\install-autostart.ps1 -Remove"
