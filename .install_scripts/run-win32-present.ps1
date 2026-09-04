[CmdletBinding()]
param(
    [string]$InnerScript = 'C:\DroidVM\agent-scripts\win32-present-inner.ps1',
    [string]$BundleRoot = 'C:\DroidVM\TurnipRuns\run-33322445949',
    [string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\evidence-win32',
    [string]$TaskName = 'DroidVM-Turnip-Win32-Present',
    [string]$InteractiveUserId = 'DROIDVM\USER',
    [ValidateRange(30, 600)]
    [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principalCheck = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principalCheck.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This runner must be elevated to register a scheduled task.'
}
if (-not (Test-Path -LiteralPath $InnerScript -PathType Leaf)) { throw "Missing inner script: $InnerScript" }

$stdout = Join-Path $OutputDirectory 'win32-probe.stdout.txt'
$exitFile = Join-Path $OutputDirectory 'win32-probe.exit.txt'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Remove-Item -LiteralPath $stdout, $exitFile -ErrorAction SilentlyContinue

$argument = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{0}" -BundleRoot "{1}" -OutputDirectory "{2}"' -f $InnerScript, $BundleRoot, $OutputDirectory
$action = New-ScheduledTaskAction -Execute 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' -Argument $argument
$principal = New-ScheduledTaskPrincipal -UserId $InteractiveUserId -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Seconds $TimeoutSeconds)
$task = New-ScheduledTask -Action $action -Principal $principal -Settings $settings

$taskResult = $null
try {
    Register-ScheduledTask -TaskName $TaskName -InputObject $task -Force | Out-Null
    Start-ScheduledTask -TaskName $TaskName

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        $info = Get-ScheduledTaskInfo -TaskName $TaskName
        $state = (Get-ScheduledTask -TaskName $TaskName).State
        if ($state -ne 'Running' -and (Test-Path -LiteralPath $exitFile)) {
            $taskResult = $info.LastTaskResult
            break
        }
    }
    if ($null -eq $taskResult) {
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        $taskResult = (Get-ScheduledTaskInfo -TaskName $TaskName).LastTaskResult
    }
} finally {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
}

$probeExit = if (Test-Path -LiteralPath $exitFile) { (Get-Content -LiteralPath $exitFile -Raw).Trim() } else { '<missing>' }
$probeOutput = if (Test-Path -LiteralPath $stdout) { @(Get-Content -LiteralPath $stdout) } else { @() }

[ordered]@{
    TaskName = $TaskName
    InteractiveUserId = $InteractiveUserId
    ScheduledTaskResult = $taskResult
    ProbeExitCode = $probeExit
    Passed = ($probeExit -eq '0')
    Output = $probeOutput
} | ConvertTo-Json -Depth 5
