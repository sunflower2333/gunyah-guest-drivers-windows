# SPDX-License-Identifier: BSD-3-Clause
# Installs only the signed virtio-media package; never changes certificate trust,
# signing policy, display binding, MFT registration or VM configuration.
[CmdletBinding(SupportsShouldProcess)]
param([Parameter(Mandatory)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:PROCESSOR_ARCHITECTURE -cne 'ARM64') { throw 'Run from native ARM64 PowerShell' }
$verify = Join-Path $PSScriptRoot 'verify-video-package.ps1'
$signature = Get-AuthenticodeSignature -LiteralPath $verify
if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate -or
    $signature.SignerCertificate.Thumbprint -cne '8705CD4DDD6DA49685EB34430106FA543FB59D53') {
    throw 'Signed package verifier is missing or untrusted'
}
$package = & $verify -PackageDirectory $PackageDirectory
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run from an elevated ARM64 shell' }
$devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1070*' })
if (!$devices.Count) { throw 'No virtio-media PCI function present; enable compatible backend first' }
$inf = Join-Path $package.Directory 'viogpuvideo.inf'
if ($PSCmdlet.ShouldProcess($inf, 'Install signed virtio-media companion and application-local MFT files')) {
    & pnputil.exe /add-driver $inf /install
    $result = $LASTEXITCODE
    if ($result -ne 0 -and $result -ne 3010) { throw "VPU pnputil failed: $result" }
    [pscustomobject]@{Staged=$true;DriverVersion=$package.DriverVersion;NeedReboot=($result -eq 3010);
        HardwareId='PCI\VEN_1AF4&DEV_1070';SourceCommit=$package.SourceCommit}
    Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1070*' } |
        Select-Object Status,FriendlyName,InstanceId
}
