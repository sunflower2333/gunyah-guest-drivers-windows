[CmdletBinding()]
param(
    [string]$Root = 'C:\DroidVM\viogpu-58207',
    [string]$ExpectedVersion = '100.6.101.58207'
)
$ErrorActionPreference = 'Stop'

Expand-Archive -LiteralPath (Join-Path $Root 'd58207.zip') -DestinationPath $Root -Force
$inf = Join-Path $Root 'viogpuwddm.inf'
if (-not (Test-Path $inf)) { throw "missing $inf" }

foreach ($f in @('viogpuwddm.sys','viogpud3d.dll','viogpuwddm.cat')) {
    $s = Get-AuthenticodeSignature -LiteralPath (Join-Path $Root $f)
    if ($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -ne 'CN=DroidVM Test') {
        throw "bad signature on ${f}: $($s.Status)"
    }
}
Write-Output "signatures=valid"

# Export the currently published package as a rollback before replacing it.
$d = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })[0]
$p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$k = Get-Item -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($p.Data)"
$beforeVer = $k.GetValue('DriverVersion'); $beforeInf = $k.GetValue('InfPath')
Write-Output "before_version=$beforeVer before_inf=$beforeInf"

$rb = "C:\DroidVM\rollback-pre-$ExpectedVersion"
if (-not (Test-Path $rb)) {
    New-Item -ItemType Directory -Path $rb | Out-Null
    $null = & pnputil.exe /export-driver $beforeInf $rb 2>&1
    Write-Output "rollback_files=$((Get-ChildItem $rb -Recurse -File).Count)"
}

$out = & pnputil.exe /add-driver $inf /install 2>&1
Write-Output "pnputil_exit=$LASTEXITCODE"
$out | Where-Object { $_ -match 'Published|installed on device|successfully' } | ForEach-Object { "  $_" }

Start-Sleep -Seconds 10
$d2 = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })[0]
$p2 = Get-PnpDeviceProperty -InstanceId $d2.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$k2 = Get-Item -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($p2.Data)"
Write-Output "after_version=$($k2.GetValue('DriverVersion')) status=$($d2.Status)"
