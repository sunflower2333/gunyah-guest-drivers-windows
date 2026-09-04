[CmdletBinding()]
param(
    [string]$TaskName = 'DroidVM-VioGpu-Visual-Stimulus',
    [string]$UserId = 'DROIDVM\USER',
    [string]$ScriptPath = 'C:\DroidVM\viogpu-58043\run-viogpu-visual-stimulus.ps1'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $ScriptPath -PathType Leaf)) {
    throw "Missing stimulus wrapper: $ScriptPath"
}

Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
$action = New-ScheduledTaskAction `
    -Execute 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$ScriptPath`""
$principal = New-ScheduledTaskPrincipal `
    -UserId $UserId `
    -LogonType Interactive `
    -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
$task = New-ScheduledTask -Action $action -Principal $principal -Settings $settings
Register-ScheduledTask -TaskName $TaskName -InputObject $task -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName
Start-Sleep -Seconds 2

$info = Get-ScheduledTaskInfo -TaskName $TaskName
$registered = Get-ScheduledTask -TaskName $TaskName
[pscustomobject]@{
    LaunchedAt = (Get-Date).ToString('o')
    TaskName = $TaskName
    UserId = $registered.Principal.UserId
    LogonType = [string]$registered.Principal.LogonType
    State = [string]$registered.State
    LastRunTime = $info.LastRunTime
    LastTaskResult = $info.LastTaskResult
} | ConvertTo-Json -Depth 3
