[CmdletBinding()]
param(
    [ValidateSet('zink','stock')][string]$Umd = 'zink',
    [string]$ZinkSource = 'C:\DroidVM\ZinkD3D2\viogpud3d-zink.dll'
)
$ErrorActionPreference = 'Stop'

$d = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })[0]
$p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$key = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($p.Data)"
$cur = (Get-Item -LiteralPath $key).GetValue('UserModeDriverName')
Write-Output "current_umd=$($cur -join ' | ')"

# Resolve the driver store directory the miniport was installed from.
$svc = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
$img = [Environment]::ExpandEnvironmentVariables([string]$svc.GetValue('ImagePath','')).Trim('"')
if ($img.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
    $img = Join-Path $env:SystemRoot $img.Substring(12)
}
$store = Split-Path $img -Parent
Write-Output "driver_store=$store"

if ($Umd -eq 'zink') {
    if (-not (Test-Path -LiteralPath $ZinkSource)) { throw "missing $ZinkSource" }
    # The DriverStore is TrustedInstaller-owned, so load the UMD from where it
    # is already staged; the runtime loads whatever path this value names.
    $dest = $ZinkSource
    Write-Output "using=$dest size=$((Get-Item $dest).Length)"
    $val = @($dest, $dest, $dest)
} else {
    $val = @((Join-Path $store 'viogpud3d.dll')) * 3
}

Set-ItemProperty -LiteralPath $key -Name 'UserModeDriverName' -Value $val -Type MultiString
Write-Output "new_umd=$(((Get-Item -LiteralPath $key).GetValue('UserModeDriverName')) -join ' | ')"
Write-Output "NOTE=restart the display device for D3D to reload the UMD"
