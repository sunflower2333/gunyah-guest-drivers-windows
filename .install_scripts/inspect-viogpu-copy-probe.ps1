[CmdletBinding()]
param(
    [string]$DevicePattern = 'PCI\VEN_1AF4&DEV_1050*'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$devices = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.InstanceId -like $DevicePattern }
)
if ($devices.Count -ne 1) {
    throw "Expected one present virtio-gpu device matching '$DevicePattern', found $($devices.Count)."
}

$driverProperty = Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
if ($null -eq $driverProperty.PSObject.Properties['Data'] -or
    [string]::IsNullOrWhiteSpace([string]$driverProperty.Data)) {
    throw "Device '$($devices[0].InstanceId)' has no driver registry key."
}

$driverKey = [string]$driverProperty.Data
$registryPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$driverKey"
$key = Get-Item -LiteralPath $registryPath
$values = @(
    foreach ($name in $key.GetValueNames() | Where-Object { $_ -like 'NativePresentCopyProbe*' } | Sort-Object) {
        [pscustomobject]@{
            Name = $name
            Kind = [string]$key.GetValueKind($name)
            Value = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        }
    }
)

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    InstanceId = $devices[0].InstanceId
    DriverKey = $driverKey
    ValueCount = $values.Count
    Values = $values
} | ConvertTo-Json -Depth 5
