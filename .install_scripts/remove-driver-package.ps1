[CmdletBinding()]
param([string]$Version = '100.6.101.58207')
$ErrorActionPreference = 'Stop'
# Find the published oem*.inf whose DriverVer matches the package to remove.
$target = $null
Get-ChildItem "$env:SystemRoot\INF\oem*.inf" -ErrorAction SilentlyContinue | ForEach-Object {
    $t = Get-Content $_.FullName -Raw -ErrorAction SilentlyContinue
    if ($t -match 'viogpuwddm' -and $t -match [regex]::Escape($Version)) { $target = $_.Name }
}
if (-not $target) { Write-Output "NOT_FOUND version=$Version"; exit 0 }
Write-Output "removing=$target ($Version)"
$out = & pnputil.exe /delete-driver $target /uninstall /force 2>&1
Write-Output "pnputil_exit=$LASTEXITCODE"
$out | Select-Object -First 4 | ForEach-Object { "  $_" }
Start-Sleep -Seconds 14
$d = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })[0]
$p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$k = Get-Item -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($p.Data)"
Write-Output "now_version=$($k.GetValue('DriverVersion')) status=$($d.Status)"
Get-CimInstance Win32_VideoController | ForEach-Object {
    "controller={0} avail={1} mode={2}" -f $_.Name, $_.Availability, $_.VideoModeDescription
}
