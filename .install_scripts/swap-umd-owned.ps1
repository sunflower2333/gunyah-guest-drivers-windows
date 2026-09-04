[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Source)
$ErrorActionPreference = 'Stop'
$devs = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
$prop = Get-PnpDeviceProperty -InstanceId $devs[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
$key  = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($prop.Data)"
$umd  = @((Get-Item -LiteralPath $key).GetValue('UserModeDriverName'))[0]
$pkg  = Split-Path -Parent $umd
Write-Output "PKG=$pkg"

# Take ownership of the freshly published package before touching it.
& takeown.exe /F "$pkg" /A /R /D Y | Out-Null
& icacls.exe  "$pkg" /grant "*S-1-5-32-544:(OI)(CI)F" /T /C | Out-Null

# A previous attempt may have parked the shipped UMD; restore it if the target is gone.
if (-not (Test-Path -LiteralPath $umd)) {
    $parked = Get-ChildItem -LiteralPath $pkg -Filter 'viogpud3d.dll.prev-*' | Sort-Object Name | Select-Object -Last 1
    if ($null -ne $parked) {
        Copy-Item -LiteralPath $parked.FullName -Destination $umd -Force
        Write-Output "RESTORED_FROM=$($parked.Name)"
    } else { throw "UMD missing and no parked copy in $pkg" }
}
Write-Output "SIZE_BEFORE=$((Get-Item -LiteralPath $umd).Length)"

$stamp = Get-Date -Format 'yyyyMMddHHmmss'
Move-Item -LiteralPath $umd -Destination "$umd.prev-$stamp" -Force
Copy-Item -LiteralPath $Source -Destination $umd -Force
Write-Output "SIZE_AFTER=$((Get-Item -LiteralPath $umd).Length) SOURCE=$Source"
