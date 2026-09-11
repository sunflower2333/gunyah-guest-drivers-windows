# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [ValidateSet('Verify','Install','Rollback')][string]$Action = 'Verify',
    [string]$PackageRoot,
    [string]$InstanceId,
    [string]$BackupPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$PSBoundParameters.ContainsKey('PackageRoot')) {
    $PackageRoot = Join-Path $PSScriptRoot 'opengl'
}
Import-Module (Join-Path $PSScriptRoot 'opengl-registration.psm1') -Force

function Assert-Catalog([string]$Root) {
    $catalog = Join-Path $Root 'opengl.cat'
    $payload = Join-Path $Root 'payload'
    $signature = Get-AuthenticodeSignature -LiteralPath $catalog
    if ($signature.Status -ne 'Valid') { throw "OpenGL catalog signature is not trusted: $($signature.Status)" }
    if ((Test-FileCatalog -Path $payload -CatalogFilePath $catalog) -ne 'Valid') { throw 'OpenGL catalog content mismatch' }
    foreach ($file in Get-ChildItem -LiteralPath $payload -Recurse -File) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point in payload: $($file.FullName)" }
        if ($file.Extension -in @('.dll','.exe')) {
            $peSignature = Get-AuthenticodeSignature -LiteralPath $file.FullName
            if ($peSignature.Status -ne 'Valid' -or $peSignature.SignerCertificate.Thumbprint -ne $signature.SignerCertificate.Thumbprint) {
                throw "OpenGL payload signer mismatch: $($file.Name)"
            }
        }
    }
    return $signature.SignerCertificate.Thumbprint
}

if ($Action -ne 'Rollback') {
    $PackageRoot = (Resolve-Path -LiteralPath $PackageRoot).Path
    $thumbprint = Assert-Catalog $PackageRoot
    Write-Output "PASS signed OpenGL sidecar verification: $thumbprint"
    if ($Action -eq 'Verify') { return }
}
if (![Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell on Windows ARM64' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (!(New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Adapter registration requires an elevated administrator session'
}
if ([string]::IsNullOrWhiteSpace($InstanceId)) { throw 'Explicit VIOGPU InstanceId is required' }
$device = Get-PnpDevice -InstanceId $InstanceId -ErrorAction Stop
if ($device.Class -ne 'Display' -or $InstanceId -notmatch '^PCI\\VEN_1AF4&DEV_1050') {
    throw 'The selected device is not the active VIOGPU display adapter'
}
if ($Action -eq 'Install' -and $device.Status -ne 'OK') {
    throw 'New ICD installation requires a healthy VIOGPU adapter; rollback remains available for a problem state'
}
$driverKey = (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Driver').Data
if ((Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Service').Data -ne 'VioGpuWddm') {
    throw 'The selected adapter is not bound to the VioGpuWddm service'
}
if ($driverKey -notmatch '^\{4d36e968-e325-11ce-bfc1-08002be10318\}\\\d{4}$') { throw 'Unexpected display adapter registry path' }
$machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
$key = $machine.OpenSubKey("SYSTEM\CurrentControlSet\Control\Class\$driverKey", $true)
if (!$key) { throw 'Cannot open the selected display adapter key' }
$installBase = Join-Path $env:ProgramFiles 'DroidVM\OpenGL'
try {
    if ($Action -eq 'Rollback') {
        if (!$BackupPath) { throw 'Rollback requires the exact saved BackupPath' }
        $BackupPath = (Resolve-Path -LiteralPath $BackupPath).Path
        if (!$BackupPath.StartsWith($installBase + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Rollback state must be in the protected OpenGL installation directory' }
        $state = Import-Clixml -LiteralPath $BackupPath
        if ($state.InstanceId -ne $InstanceId -or $state.DriverKey -ne $driverKey) { throw 'Rollback state belongs to another adapter' }
        if (!(Test-IcdSnapshot $key $state.Installed)) { throw 'Adapter values changed after installation; refusing to overwrite newer state' }
        Invoke-IcdRegistration $key $state.Previous
        Write-Output 'PASS restored the eight original adapter values; package files retained for recovery'
        return
    }
    $binding = Get-Content (Join-Path $PackageRoot 'payload/package-binding.json') -Raw | ConvertFrom-Json
    $service = $machine.OpenSubKey('SYSTEM\CurrentControlSet\Services\VioGpuWddm')
    if (!$service) { throw 'Installed VioGpuWddm service is missing' }
    try { $driverPath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath')) } finally { $service.Dispose() }
    $driverPath = $driverPath -replace '^\\SystemRoot', $env:SystemRoot -replace '^\\\?\?\\', ''
    if (![IO.Path]::IsPathRooted($driverPath)) { $driverPath = Join-Path $env:SystemRoot $driverPath }
    if ((Get-FileHash -LiteralPath $driverPath -Algorithm SHA256).Hash -ne $binding.kmd_sha256) {
        throw 'The active KMD does not match this jointly signed package; install its base driver first'
    }
    $packageId = (Get-FileHash (Join-Path $PackageRoot 'opengl.cat') -Algorithm SHA256).Hash.ToLowerInvariant()
    New-Item -ItemType Directory -Path $installBase -Force | Out-Null
    if ((Get-Item -LiteralPath $installBase).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'OpenGL installation root cannot be a reparse point' }
    $destination = Join-Path $installBase $packageId
    if (Test-Path -LiteralPath $destination) { throw 'This sidecar is already staged; use its saved registration or rollback state' }
    New-Item -ItemType Directory -Path $destination | Out-Null
    Copy-Item -LiteralPath (Join-Path $PackageRoot 'payload') -Destination $destination -Recurse
    Copy-Item -LiteralPath (Join-Path $PackageRoot 'opengl.cat') -Destination $destination
    [void](Assert-Catalog $destination)
    $desired = Get-IcdRegistration (Join-Path $destination 'payload')
    $state = [pscustomobject]@{ InstanceId = $InstanceId; DriverKey = $driverKey
        Previous = @(Get-IcdSnapshot $key); Installed = @($desired); PackageId = $packageId }
    $BackupPath = Join-Path $destination 'registration-backup.clixml'
    $state | Export-Clixml -LiteralPath $BackupPath -Depth 8
    $saved = Import-Clixml -LiteralPath $BackupPath
    if (!(Test-IcdSnapshot $key $saved.Previous)) { throw 'Saved original registration did not verify' }
    Invoke-IcdRegistration $key $desired
    Write-Output "PASS registered system OpenGL native/AMD64 ARM64X + x86 Wow ICD; BackupPath=$BackupPath"
    Write-Output 'Start new application processes for validation. Registration success is not rendering acceptance.'
} finally {
    $key.Dispose()
    $machine.Dispose()
}
