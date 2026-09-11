[CmdletBinding()]
param(
    [ValidateSet('Verify','Install','Rollback')][string]$Action = 'Verify',
    [string]$PackageRoot = (Join-Path $PSScriptRoot 'opencl'),
    [string]$InstanceId,
    [string]$BackupPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'opencl-registration.psm1') -Force
function Assert-Package([string]$Root) {
    $catalog = Join-Path $Root 'opencl.cat'
    $signature = Get-AuthenticodeSignature $catalog
    if ($signature.Status -ne 'Valid') { throw 'OpenCL catalog signature is not trusted' }
    if ((Test-FileCatalog -Path "$Root/payload" -CatalogFilePath $catalog) -ne 'Valid') { throw 'OpenCL catalog content mismatch' }
    foreach ($file in Get-ChildItem "$Root/payload" -Recurse -Force) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Payload contains reparse point' }
        if (!$file.PSIsContainer -and $file.Extension -in @('.dll','.exe')) {
            $sig = Get-AuthenticodeSignature $file.FullName
            if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Thumbprint -ne $signature.SignerCertificate.Thumbprint) { throw "OpenCL signer mismatch: $($file.Name)" }
        }
    }
}
if ($Action -ne 'Rollback') {
    $PackageRoot = (Resolve-Path $PackageRoot).Path
    Assert-Package $PackageRoot
    if ($Action -eq 'Verify') { Write-Output 'PASS signed OpenCL package; GPU not tested'; return }
}
if (![Environment]::Is64BitProcess -or $env:PROCESSOR_ARCHITECTURE -ne 'ARM64') { throw 'Use native ARM64 PowerShell' }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrator required for global registration' }
$installBase = Join-Path $env:ProgramFiles 'DroidVM\OpenCL'
$systemTargets = @((Join-Path $env:SystemRoot 'System32\OpenCL.dll'), (Join-Path $env:SystemRoot 'SysWOW64\OpenCL.dll'))
if ($Action -eq 'Rollback') {
    if (!$BackupPath) { throw 'Exact BackupPath required' }
    $BackupPath = (Resolve-Path $BackupPath).Path
    if (!$BackupPath.StartsWith($installBase + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Rollback state must be in protected installation directory' }
    $state = Import-Clixml $BackupPath
    if (!(Test-OpenClSnapshot $state.Installed)) { throw 'Registration changed; refusing to overwrite newer state' }
    foreach ($file in $state.CreatedLoaders) {
        if ($file.Path -notin $systemTargets -or !(Test-Path $file.Path) -or (Get-FileHash $file.Path).Hash -ne $file.Hash) { throw 'Owned system loader changed; manual review required' }
    }
    Set-OpenClSnapshot $state.Previous
    if (!(Test-OpenClSnapshot $state.Previous)) { throw 'Registry rollback readback failed' }
    foreach ($file in $state.CreatedLoaders) { Remove-Item -LiteralPath $file.Path }
    Write-Output 'PASS original vendor values restored; only unchanged loaders created by this installation removed; signed payload retained'
    return
}
if (!$InstanceId -or $InstanceId -notmatch '^PCI\\VEN_1AF4&DEV_1050') { throw 'Explicit VIOGPU adapter required' }
$device = Get-PnpDevice -InstanceId $InstanceId
if ($device.Class -ne 'Display' -or $device.Status -ne 'OK') { throw 'Healthy VIOGPU display adapter required' }
if ((Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Service').Data -ne 'VioGpuWddm') { throw 'VIOGPU service mismatch' }
$binding = Get-Content "$PackageRoot/payload/package-binding.json" -Raw | ConvertFrom-Json
$driverPath = [Environment]::ExpandEnvironmentVariables((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\VioGpuWddm').ImagePath)
$driverPath = $driverPath -replace '^\\SystemRoot', $env:SystemRoot -replace '^\\\?\?\\', ''
if (![IO.Path]::IsPathRooted($driverPath)) { $driverPath = Join-Path $env:SystemRoot $driverPath }
if ((Get-FileHash $driverPath).Hash -ne $binding.kmd_sha256 -or
    (Get-FileHash (Join-Path (Split-Path $driverPath) 'viogpud3d.dll')).Hash -ne $binding.d3d_umd_sha256) { throw 'Installed KMD/UMD does not match joint package' }
$packageId = (Get-FileHash "$PackageRoot/opencl.cat").Hash.ToLowerInvariant()
New-Item -ItemType Directory -Force $installBase | Out-Null
if ((Get-Item $installBase).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Install directory cannot be a reparse point' }
$destination = Join-Path $installBase $packageId
if (Test-Path $destination) { throw 'Package already staged; inspect its saved state' }
New-Item -ItemType Directory $destination | Out-Null
Copy-Item "$PackageRoot/payload" $destination -Recurse
Copy-Item "$PackageRoot/opencl.cat" $destination
Assert-Package $destination
$entries = @(Get-OpenClEntries "$destination/payload")
$previous = @(Get-OpenClSnapshot $entries)
if (@($previous | Where-Object Present).Count) { throw 'Vendor targets already registered' }
$desired = @($entries | ForEach-Object {[pscustomobject]@{View=$_.View; Name=$_.Name; Present=$true; Kind='DWord'; Value=0}})
$created = @()
$state = [pscustomobject]@{InstanceId=$InstanceId; Previous=$previous; Installed=$desired; CreatedLoaders=@(); PackageId=$packageId}
$BackupPath = Join-Path $destination 'registration-backup.clixml'
$state | Export-Clixml $BackupPath -Depth 8
try {
    # Never overwrite an existing Windows/OpenCL vendor loader. If absent,
    # create it exclusively and record its signed identity before registration.
    foreach ($pair in @(@('arm64x',$systemTargets[0]), @('x86',$systemTargets[1]))) {
        if (!(Test-Path -LiteralPath $pair[1])) {
            $source = Join-Path "$destination/payload/loaders" "$($pair[0])/OpenCL.dll"
            $record = [pscustomobject]@{Path=$pair[1]; Hash=(Get-FileHash $source).Hash}
            $created += $record
            $state.CreatedLoaders = $created
            $state | Export-Clixml $BackupPath -Depth 8
            [IO.File]::Copy($source, $pair[1], $false)
        }
    }
    foreach ($arch in @('arm64','x64','x86')) {
        $target = if ($arch -eq 'x86') {$systemTargets[1]} else {$systemTargets[0]}
        & (Join-Path "$destination/payload/loaders" "$arch/loader-check.exe") $target
        if ($LASTEXITCODE) { throw "Existing system OpenCL loader cannot serve $arch; it was preserved" }
    }
    Set-OpenClSnapshot $desired
    if (!(Test-OpenClSnapshot $desired)) { throw 'Registration readback failed' }
} catch {
    Set-OpenClSnapshot $previous
    foreach ($file in $created) {
        if ((Test-Path $file.Path) -and (Get-FileHash $file.Path).Hash -eq $file.Hash) { Remove-Item -LiteralPath $file.Path }
    }
    throw
}
Write-Output "PASS global Khronos vendor registration ARM64/x64/x86; BackupPath=$BackupPath"
Write-Output 'New ordinary application processes must validate system loader discovery, Turnip selection and compiler/kernel execution. No GPU acceptance is implied.'
