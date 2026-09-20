# SPDX-License-Identifier: BSD-3-Clause
# Receives only a public certificate-store thumbprint; private-key import belongs
# to the isolated CI step. Never exports or accepts private key/password data.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CertificateThumbprint,
    [ValidatePattern('^0\.2\.0\.\d{1,5}$')][string]$DriverVersion='0.2.0.1',
    [string]$OutputDirectory='artifacts/video-signed',
    [string]$ToolsDirectory='artifacts/video-signed-tools'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true') { throw 'Signing package assembly runs only on disposable CI runners' }
if ($CertificateThumbprint -cne '8705CD4DDD6DA49685EB34430106FA543FB59D53') { throw 'Unexpected fixed public signer' }
if ([version]$DriverVersion -gt [version]'0.2.0.65535') { throw 'Invalid VPU version component' }
$cert=Get-Item -LiteralPath "Cert:\CurrentUser\My\$CertificateThumbprint"
if (!$cert.HasPrivateKey) { throw 'Configured fixed certificate must be present in CI store' }
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$source=(& git -C $root rev-parse HEAD).Trim()
# Read the raw commit header: revision traversal hides parents at a depth=1
# checkout even though the commit object still carries the real parent ID.
$parents=@(& git -C $root cat-file -p HEAD | Select-String '^parent ([0-9a-f]{40})$')
if ($LASTEXITCODE -or $parents.Count -ne 1) { throw 'Expected a single source parent' }
$parent=$parents[0].Matches[0].Groups[1].Value
if ($source -notmatch '^[0-9a-f]{40}$' -or $parent -notmatch '^[0-9a-f]{40}$') { throw 'Source identity unresolved' }
foreach ($path in @($OutputDirectory,$ToolsDirectory)) {
    if (Test-Path -LiteralPath $path) { throw "Output must be fresh: $path" }
    New-Item -ItemType Directory -Path $path | Out-Null
}
$driver=(Resolve-Path $OutputDirectory).Path
$tools=(Resolve-Path $ToolsDirectory).Path
$sys=Join-Path $root 'viogpu/viogpuvideo/objfre_win11_arm64/arm64/viogpuvideo.sys'
$mft=Join-Path $root 'artifacts/video-mft-arm64/viogpuvideo_mft.dll'
Copy-Item -LiteralPath $sys -Destination $driver
Copy-Item -LiteralPath $mft -Destination $driver

# The WDK-only source INF remains buildable independently. Final composition is
# exact and fail-closed, like the existing main flat-driver packaging flow.
$inf=Get-Content -LiteralPath (Join-Path $root 'viogpu/viogpuvideo/viogpuvideo.inf') -Raw
function Replace-Once([string]$Text,[string]$Pattern,[string]$Replacement) {
    if ([regex]::Matches($Text,$Pattern).Count -ne 1) { throw "INF composition anchor count: $Pattern" }
    return [regex]::Replace($Text,$Pattern,$Replacement)
}
$inf=Replace-Once $inf '(?m)^DriverVer=[^\r\n]+' ('DriverVer=09/20/2026,'+$DriverVersion)
$inf=Replace-Once $inf '(?m)^viogpuvideo\.sys=1[\t ]*\r?$' "viogpuvideo.sys=1`r`nviogpuvideo_mft.dll=1`r`nviogpuvideo-package.json=1"
$inf=Replace-Once $inf '(?m)^\[VideoFiles\][\t ]*\r?\nviogpuvideo\.sys[\t ]*\r?$' "[VideoFiles]`r`nviogpuvideo.sys`r`nviogpuvideo_mft.dll`r`nviogpuvideo-package.json"
$infPath=Join-Path $driver 'viogpuvideo.inf'
[IO.File]::WriteAllText($infPath,$inf,[Text.UTF8Encoding]::new($false))
foreach ($name in @('viogpuvideo.sys','viogpuvideo_mft.dll')) {
    & $env:SIGNTOOL_PATH sign /fd SHA256 /s My /sha1 $CertificateThumbprint (Join-Path $driver $name)
    if ($LASTEXITCODE) { throw "VPU signing failed: $name" }
}
$files=[ordered]@{}
foreach ($name in @('viogpuvideo.inf','viogpuvideo.sys','viogpuvideo_mft.dll')) {
    $files[$name]=(Get-FileHash -LiteralPath (Join-Path $driver $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$manifest=[ordered]@{schema=1;kind='viogpuvideo-arm64';driver_version=$DriverVersion;
    hardware_id='PCI\VEN_1AF4&DEV_1070';source_commit=$source;source_parent=$parent;
    implementation_commit='3d53f725355030bc42644e56596e83247d00fdcd';
    signer_thumbprint=$CertificateThumbprint;files=$files;system_mft_registered=$false;dxva_advertised=$false}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $driver 'viogpuvideo-package.json') -Encoding utf8
& $env:INFVERIF_PATH /w /v $infPath
if ($LASTEXITCODE) { throw 'Final VPU InfVerif failed' }
& $env:INF2CAT_PATH /driver:$driver /os:Server10_ARM64
if ($LASTEXITCODE) { throw 'Final VPU Inf2Cat failed' }
$catalog=Join-Path $driver 'viogpuvideo.cat'
& $env:SIGNTOOL_PATH sign /fd SHA256 /s My /sha1 $CertificateThumbprint $catalog
if ($LASTEXITCODE) { throw 'VPU catalog signing failed' }

foreach ($name in @('verify-video-package.ps1','install-video-package.ps1','MFT.md')) {
    Copy-Item -LiteralPath (Join-Path $root "viogpu/video/$name") -Destination $tools
}
Copy-Item -LiteralPath (Join-Path $root 'viogpu/tests/video/test-mft-device.ps1') -Destination $tools
Copy-Item -LiteralPath (Join-Path $root 'artifacts/video-mft-arm64/mft-probe.exe') -Destination $tools
Export-Certificate -Cert $cert -FilePath (Join-Path $tools 'DroidVM_Test.cer') | Out-Null
foreach ($script in Get-ChildItem -LiteralPath $tools -Filter '*.ps1' -File) {
    Set-AuthenticodeSignature -LiteralPath $script.FullName -Certificate $cert -HashAlgorithm SHA256 | Out-Null
}
& $env:SIGNTOOL_PATH sign /fd SHA256 /s My /sha1 $CertificateThumbprint (Join-Path $tools 'mft-probe.exe')
if ($LASTEXITCODE) { throw 'VPU probe signing failed' }
foreach ($file in Get-ChildItem -LiteralPath $tools -File | Where-Object Extension -in @('.ps1','.exe')) {
    $signature=Get-AuthenticodeSignature -LiteralPath $file.FullName
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -cne $CertificateThumbprint) {
        throw "VPU validation-tool signature failed: $($file.Name)"
    }
}
& (Join-Path $tools 'verify-video-package.ps1') -PackageDirectory $driver
Write-Output "PASS signed flat VPU package $DriverVersion from $source"
