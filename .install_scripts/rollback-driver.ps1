[CmdletBinding()]
param([string]$RollbackRoot = 'C:\DroidVM\rollback-pre-100.6.101.58207')
$ErrorActionPreference = 'Stop'
$inf = Get-ChildItem -LiteralPath $RollbackRoot -Recurse -Filter 'viogpuwddm.inf' -ErrorAction SilentlyContinue |
       Select-Object -First 1
if (-not $inf) { throw "no rollback INF under $RollbackRoot" }
Write-Output "rollback_inf=$($inf.FullName)"
$out = & pnputil.exe /add-driver $inf.FullName /install 2>&1
Write-Output "pnputil_exit=$LASTEXITCODE"
$out | Where-Object { $_ -match 'Published|installed on device|successfully' } | ForEach-Object { "  $_" }
Start-Sleep -Seconds 12
$d = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })[0]
$p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$k = Get-Item -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($p.Data)"
Write-Output "after_version=$($k.GetValue('DriverVersion')) status=$($d.Status)"
Get-CimInstance Win32_VideoController | ForEach-Object {
    "controller={0} avail={1} mode={2}" -f $_.Name, $_.Availability, $_.VideoModeDescription
}
