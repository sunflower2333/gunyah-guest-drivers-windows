[CmdletBinding()]
param(
    [string]$BundleRoot = 'C:\DroidVM\TurnipRuns\run-33322445949\turnip-wddm-arm64-icd-bundle',
    [string]$RunnerPath = 'C:\DroidVM\TurnipRuns\run-33322445949\run-turnip-wddm-win32-direct.cmd',
    [string]$OutputPrefix = 'C:\DroidVM\TurnipRuns\evidence\win32-interactive-fresh-20260831',
    [string]$TaskName = 'DroidVM-Turnip-Win32-Probe',
    [string]$UserId = 'DROIDVM\USER',
    [ValidateRange(10, 600)]
    [int]$TimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

foreach ($path in @($BundleRoot, $RunnerPath)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing interactive probe input: $path"
    }
}

$existing = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if ($null -ne $existing) {
    throw "Scheduled task already exists: $TaskName"
}

$arguments = "/d /c `"`"$RunnerPath`" `"$BundleRoot`" `"$OutputPrefix`"`""
$action = New-ScheduledTaskAction -Execute 'C:\Windows\System32\cmd.exe' -Argument $arguments
$principal = New-ScheduledTaskPrincipal -UserId $UserId -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
$task = New-ScheduledTask -Action $action -Principal $principal -Settings $settings
$startedAt = Get-Date
$timedOut = $false
$info = $null

try {
    Register-ScheduledTask -TaskName $TaskName -InputObject $task -Force | Out-Null
    Start-ScheduledTask -TaskName $TaskName
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 250
        $registered = Get-ScheduledTask -TaskName $TaskName
        $info = Get-ScheduledTaskInfo -TaskName $TaskName
        if ($registered.State -ne 'Running' -and $info.LastRunTime.Year -gt 2000) {
            break
        }
    } while ([DateTime]::UtcNow -lt $deadline)

    if ($registered.State -eq 'Running' -or $info.LastRunTime.Year -le 2000) {
        $timedOut = $true
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    }

    $result = [ordered]@{
        StartedAt = $startedAt.ToString('o')
        FinishedAt = (Get-Date).ToString('o')
        TaskName = $TaskName
        UserId = $UserId
        TimedOut = $timedOut
        LastRunTime = $info.LastRunTime.ToString('o')
        LastTaskResult = [int64]$info.LastTaskResult
        OutputPrefix = $OutputPrefix
    }
    $result | ConvertTo-Json | Set-Content -LiteralPath "$OutputPrefix.task.json" -Encoding UTF8
    $result | ConvertTo-Json
} finally {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
}

if ($timedOut) {
    exit 124
}
exit ([int]$info.LastTaskResult)
