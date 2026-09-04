[CmdletBinding()]
param(
    [string]$DevicePattern = 'PCI\VEN_1AF4&DEV_1050*',
    [int]$WaitSeconds = 8
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like $DevicePattern })
if ($devices.Count -ne 1) {
    throw "Expected one present VirtIO GPU device, found $($devices.Count)."
}

$instanceId = [string]$devices[0].InstanceId
& pnputil.exe /restart-device $instanceId
if ($LASTEXITCODE -ne 0) {
    throw "pnputil restart failed with exit code $LASTEXITCODE."
}

Start-Sleep -Seconds $WaitSeconds
$device = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -eq $instanceId }
[pscustomobject]@{
    InstanceId = $instanceId
    Status = if ($null -eq $device) { 'Missing' } else { $device.Status }
    FriendlyName = if ($null -eq $device) { $null } else { $device.FriendlyName }
} | ConvertTo-Json
