$ErrorActionPreference = 'Stop'
$d = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })[0]
$p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$k = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($p.Data)"
$umd = @((Get-Item -LiteralPath $k).GetValue('UserModeDriverName'))[0]
$pkg = Split-Path -Parent $umd

# The shipped shim is the 105904-byte parked copy.
$shim = Get-ChildItem -LiteralPath $pkg -Filter 'viogpud3d.dll.prev-*' |
        Where-Object { $_.Length -eq 105904 } | Sort-Object Name | Select-Object -First 1
if (-not $shim) { throw "no 105904-byte parked shim in $pkg" }

# A loaded image cannot be overwritten but can be renamed out of the way.
$stamp = Get-Date -Format 'yyyyMMddHHmmss'
Move-Item -LiteralPath $umd -Destination "$umd.probe-$stamp" -Force
Copy-Item -LiteralPath $shim.FullName -Destination $umd -Force
Remove-Item 'C:\DroidVM\ZinkD3D\interpose.on' -Force -ErrorAction SilentlyContinue
Write-Output "restored=$((Get-Item -LiteralPath $umd).Length) from=$($shim.Name) device=$($d.Status)/$($d.Problem)"
