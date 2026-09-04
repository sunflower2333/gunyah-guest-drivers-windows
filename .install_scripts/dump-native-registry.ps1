$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$device = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
if ($device.Count -ne 1) { throw "expected one display device, got $($device.Count)" }
$property = Get-PnpDeviceProperty -InstanceId $device[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
$key = Get-Item -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
$result = [ordered]@{}
foreach ($name in $key.GetValueNames() | Where-Object { $_ -like 'Native*' } | Sort-Object) {
    $result[$name] = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
}
$result | ConvertTo-Json -Depth 4
