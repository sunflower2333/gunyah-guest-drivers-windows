[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Zip,
    [Parameter(Mandatory)][string]$ExpectedVersion
)
$ErrorActionPreference = 'Stop'

Expand-Archive -LiteralPath (Join-Path $Root $Zip) -DestinationPath $Root -Force
$inf = Get-ChildItem -Path $Root -Filter 'viogpuwddm.inf' -Recurse | Select-Object -First 1
if ($null -eq $inf) { throw "no viogpuwddm.inf under $Root" }
$pkg = $inf.DirectoryName
Write-Output "package=$pkg"

foreach ($f in @('viogpuwddm.sys','viogpud3d.dll','viogpuwddm.cat')) {
    $path = Join-Path $pkg $f
    if (-not (Test-Path $path)) { throw "missing $f" }
    $s = Get-AuthenticodeSignature -LiteralPath $path
    if ($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -ne 'CN=DroidVM Test') {
        throw "bad signature on ${f}: $($s.Status) / $($s.SignerCertificate.Subject)"
    }
}
Write-Output "signatures=valid"

$d = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })[0]
$p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$k = Get-Item -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($p.Data)"
$beforeVer = $k.GetValue('DriverVersion'); $beforeInf = $k.GetValue('InfPath')
Write-Output "before_version=$beforeVer before_inf=$beforeInf"

$rb = "C:\DroidVM\rollback-pre-$ExpectedVersion"
if (-not (Test-Path $rb)) {
    New-Item -ItemType Directory -Path $rb -Force | Out-Null
    $null = & pnputil.exe /export-driver $beforeInf $rb 2>&1
    Write-Output "rollback_files=$((Get-ChildItem $rb -Recurse -File).Count)"
}

$out = & pnputil.exe /add-driver (Join-Path $pkg 'viogpuwddm.inf') /install 2>&1
Write-Output "pnputil_exit=$LASTEXITCODE"
$out | Where-Object { $_ -match 'Published|installed on device|successfully|Driver package' } | ForEach-Object { "  $_" }

Start-Sleep -Seconds 12
$d2 = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })[0]
$p2 = Get-PnpDeviceProperty -InstanceId $d2.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$k2 = Get-Item -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($p2.Data)"
Write-Output "after_version=$($k2.GetValue('DriverVersion')) status=$($d2.Status) classkey=$($p2.Data)"
if ($k2.GetValue('DriverVersion') -ne $ExpectedVersion) {
    Write-Output "WARNING: expected $ExpectedVersion"
}
