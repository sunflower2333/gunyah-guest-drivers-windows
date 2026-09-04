[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
$property = Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
$key = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
$item = Get-Item -LiteralPath $key
$names = @($item.GetValueNames() | Where-Object { $_ -like 'Native*' })
foreach ($n in $names) { Remove-ItemProperty -LiteralPath $key -Name $n -ErrorAction SilentlyContinue }
$left = @((Get-Item -LiteralPath $key).GetValueNames() | Where-Object { $_ -like 'Native*' })
Write-Output "CLEARED=$($names.Count) REMAINING=$($left.Count)"
