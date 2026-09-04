[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [string]$Arguments = '',
    [string]$WorkDir = 'C:\DroidVM\VulkanTools',
    [string]$TaskName = 'DroidVM-Session1-Launch',
    [string]$UserId = 'DROIDVM\USER',
    [switch]$Stop
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 has no Split-Path -LeafBase.
$procName = [IO.Path]::GetFileNameWithoutExtension($Exe)

if ($Stop) {
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Get-Process $procName -ErrorAction SilentlyContinue | Stop-Process -Force
    Write-Output "STOPPED"
    return
}

if ($Arguments -ne '') {
    $action = New-ScheduledTaskAction -Execute $Exe -Argument $Arguments -WorkingDirectory $WorkDir
} else {
    $action = New-ScheduledTaskAction -Execute $Exe -WorkingDirectory $WorkDir
}
$principal = New-ScheduledTaskPrincipal -UserId $UserId -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
$task = New-ScheduledTask -Action $action -Principal $principal -Settings $settings
Register-ScheduledTask -TaskName $TaskName -InputObject $task -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName
Start-Sleep -Seconds 4
$state = (Get-ScheduledTask -TaskName $TaskName).State
$proc = @(Get-Process $procName -ErrorAction SilentlyContinue)
Write-Output "task_state=$state processes=$($proc.Count) sessions=$(($proc | ForEach-Object { $_.SessionId }) -join ',')"
