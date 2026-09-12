# SPDX-License-Identifier: MIT
# Package GPU installer code only. The calling CI owns fixed-certificate import;
# this script never accepts, decodes, exports or prints private-key material.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{40}$')][string]$DriverProducerCommit,
    [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{40}$')][string]$InstallerCommit,
    [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{40}$')][string]$CertificateThumbprint,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$SourceRoot = (Join-Path $PSScriptRoot '../..')
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$SourceRoot=(Resolve-Path -LiteralPath $SourceRoot).Path
$sourceCommit=(& git -C $SourceRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -or $sourceCommit -ine $InstallerCommit) { throw 'InstallerCommit does not match source checkout' }
$certificate=Get-Item -LiteralPath "Cert:\CurrentUser\My\$CertificateThumbprint"
if (!$certificate.HasPrivateKey -or $certificate.Thumbprint -ine $CertificateThumbprint) {
    throw 'Exact signing certificate with private-key access is required in the CI certificate store'
}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$archive=$OutputDirectory + '.zip'
if ((Test-Path -LiteralPath $OutputDirectory) -or (Test-Path -LiteralPath $archive)) {
    throw 'Installer bundle output must be new; existing artifacts are never overwritten'
}
$payload=Join-Path $OutputDirectory 'installer'
New-Item -ItemType Directory $payload -Force | Out-Null
$names=@('viogpu-unified-install.ps1','viogpu-install-state.psm1','viogpu-install-native.cs',
    'viogpu-api-registration.psm1','viogpu-install-certificates.psm1')
$sourceHashes=[ordered]@{}
foreach ($name in $names) {
    $source=Join-Path (Join-Path $SourceRoot '.install_scripts') $name
    $sourceHashes[$name]=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    Copy-Item -LiteralPath $source -Destination (Join-Path $payload $name)
}
Export-Certificate -Cert $certificate -FilePath (Join-Path $payload 'DroidVM_Test.cer') | Out-Null
function Sign-InstallerFile([string]$Path) {
    Set-AuthenticodeSignature -LiteralPath $Path -Certificate $certificate -HashAlgorithm SHA256 | Out-Null
    $signature=Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ine $CertificateThumbprint) {
        throw "Installer signature is not valid for the exact CI certificate: $Path"
    }
}
foreach ($name in $names) {
    if ([IO.Path]::GetExtension($name) -in @('.ps1','.psm1')) { Sign-InstallerFile (Join-Path $payload $name) }
}
@'
This bundle contains the GPU installer and helpers only. It contains no driver
INF, SYS, DLL, Vulkan/OpenCL loader or GPU test executable.

The calling CI imported the existing fixed signing certificate. Verify the
public certificate identity and installer.cat signature, then run Test-FileCatalog
against this installer directory before executing scripts. The catalog covers
all helper code, this note and installer-receipt.json. The receipt records the
driver producer commit and installer source commit separately.

Use native elevated ARM64 Windows PowerShell. Reference the unchanged, separately
authenticated driver package with an explicit PackageRoot, for example:

  .\installer\viogpu-unified-install.ps1 -Action Verify -PackageRoot C:\Existing58474\drivers\viogpu

Install is available through the same entry with -Action Install and the same
explicit PackageRoot. Rollback/Uninstall require the original exact JournalPath.
Fixing console result reporting does not require reinstalling an already
completed GPU transaction. Keep the original driver archive and receipt intact.
'@ | Set-Content -LiteralPath (Join-Path $payload 'README.txt') -Encoding UTF8
$files=[ordered]@{}
foreach ($file in Get-ChildItem -LiteralPath $payload -File | Sort-Object Name) {
    $files[$file.Name]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$receipt=[ordered]@{schema=1;kind='viogpu-installer-only';driver_payload_supplied=$false;
    driver_producer_commit=$DriverProducerCommit.ToLowerInvariant();installer_source_commit=$sourceCommit.ToLowerInvariant();
    signer_thumbprint=$certificate.Thumbprint;source_files_sha256=$sourceHashes;files_after_signing=$files}
$receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $payload 'installer-receipt.json') -Encoding UTF8
$catalog=Join-Path $OutputDirectory 'installer.cat'
New-FileCatalog -Path $payload -CatalogFilePath $catalog -CatalogVersion 2.0 | Out-Null
Sign-InstallerFile $catalog
if ((Test-FileCatalog -Path $payload -CatalogFilePath $catalog) -ne 'Valid') { throw 'Installer-only catalog membership failed' }
Compress-Archive -LiteralPath @($payload,$catalog) -DestinationPath $archive
[pscustomobject]@{Directory=$OutputDirectory;Archive=$archive;
    Sha256=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant();
    DriverProducerCommit=$receipt.driver_producer_commit;InstallerCommit=$receipt.installer_source_commit;
    CertificateThumbprint=$certificate.Thumbprint}
