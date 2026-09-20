# SPDX-License-Identifier: BSD-3-Clause
# Run only after the main owner has enabled the compatible backend and installed
# a signed viogpuvideo companion. Does not change VM configuration or install drivers.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$FixtureDirectory,
    [string]$ProbeDirectory,
    [uint32]$DecoderIndex = 0,
    [string]$OutputDirectory = 'C:\DroidVMTests\video-mft'
)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path $PackageDirectory).Path
$fixture = (Resolve-Path $FixtureDirectory).Path
if (!$ProbeDirectory) { $ProbeDirectory = $package }
$probe = (Resolve-Path $ProbeDirectory).Path
$dll = Join-Path $package 'viogpuvideo_mft.dll'
$exe = Join-Path $probe 'mft-probe.exe'
foreach ($file in @($dll, $exe)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing $file" }
}
if (Test-Path -LiteralPath (Join-Path $package 'viogpuvideo-package.json')) {
    $verify = Join-Path $probe 'verify-video-package.ps1'
    foreach ($file in @($verify, $exe)) {
        $signature = Get-AuthenticodeSignature -LiteralPath $file
        if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate -or
            $signature.SignerCertificate.Thumbprint -cne '8705CD4DDD6DA49685EB34430106FA543FB59D53') {
            throw "Untrusted VPU acceptance tool: $file"
        }
    }
    & $verify -PackageDirectory $package | Out-Host
}
$manifest = Get-Content -Raw (Join-Path $fixture 'manifest.json') | ConvertFrom-Json
$coded = Join-Path $fixture 'canonical-12frames.h264'
if ((Get-FileHash $coded -Algorithm SHA256).Hash.ToLowerInvariant() -ne $manifest.encoded_sha256) {
    throw 'Encoded fixture identity mismatch'
}
$devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1070*' })
if (!$devices.Count -or !($devices | Where-Object Status -eq 'OK')) {
    throw 'No started virtio-media companion; backend/driver bring-up must happen first'
}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$out = [IO.Path]::GetFullPath($OutputDirectory)
# Windows resolves the imported MFT beside the executable. Stage just these
# two files in the test-output directory; the signed driver package stays flat
# and contains no probe. Recheck copied bytes before executing the probe.
foreach ($file in @($dll, $exe)) {
    $staged = Join-Path $out ([IO.Path]::GetFileName($file))
    if ($file -ine $staged) { Copy-Item -LiteralPath $file -Destination $staged -Force }
    if ((Get-FileHash -LiteralPath $file).Hash -cne (Get-FileHash -LiteralPath $staged).Hash) {
        throw "Staged VPU acceptance file mismatch: $file"
    }
}
# Two independent short sessions test cleanup/reopen as well as initial decode.
for ($run = 1; $run -le 2; $run++) {
    $raw = Join-Path $out "decode-$run.nv12"
    & (Join-Path $out 'mft-probe.exe') $DecoderIndex $manifest.width $manifest.height $manifest.fps `
        $coded $raw $manifest.frames $manifest.decoded_sha256
    if ($LASTEXITCODE) { throw "MFT device probe failed, run $run exit $LASTEXITCODE" }
    if ((Get-FileHash $raw -Algorithm SHA256).Hash.ToLowerInvariant() -ne $manifest.decoded_sha256) {
        throw "Independent output hash check failed for run $run"
    }
}
Write-Output 'PASS two app-local MF decode sessions; Android hardware codec logs remain a required separate gate'
