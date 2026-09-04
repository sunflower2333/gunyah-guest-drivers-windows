[CmdletBinding()]
param(
    [string]$InnerScript = 'C:\DroidVM\agent-scripts\inspect-display-topology-inner.ps1',
    [string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\evidence-topology',
    [string]$TaskName = 'DroidVM-Display-Topology',
    [string]$InteractiveUserId = 'DROIDVM\USER',
    [ValidateRange(30,900)][int]$TimeoutSeconds = 240
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$principalCheck = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principalCheck.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Must run elevated.' }

$stdout = Join-Path $OutputDirectory 'vkcube.stdout.txt'
$exitFile = Join-Path $OutputDirectory 'vkcube.exit.txt'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Remove-Item -LiteralPath $stdout, $exitFile -ErrorAction SilentlyContinue

$argument = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -OutputDirectory "{1}"' -f $InnerScript, $OutputDirectory
$action = New-ScheduledTaskAction -Execute 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' -Argument $argument
$principal = New-ScheduledTaskPrincipal -UserId $InteractiveUserId -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Seconds $TimeoutSeconds)
$task = New-ScheduledTask -Action $action -Principal $principal -Settings $settings

try {
    Register-ScheduledTask -TaskName $TaskName -InputObject $task -Force | Out-Null
    Start-ScheduledTask -TaskName $TaskName
    Write-Output "STARTED_IN_SESSION1=$TaskName"
} catch {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    throw
}
