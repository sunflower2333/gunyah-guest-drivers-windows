# SPDX-License-Identifier: MIT
# Disposable Windows CI runner only. No adapter staging, binding or GPU context.
[CmdletBinding()]
param([string]$Output='out', [string]$GlProbes='opengl-payload', [string]$ClProbes='opencl-payload',
    [string]$D3dProbes='d3d10-payload', [string]$DxvkProbes='dxvk-umd')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true') { throw 'This trust/ABI fixture is for disposable GitHub runners' }
$outputRoot = (Resolve-Path $Output).Path
$driver = Join-Path $outputRoot 'drivers/viogpu'
$gl = (Resolve-Path $GlProbes).Path
$cl = (Resolve-Path $ClProbes).Path
$d3d = (Resolve-Path $D3dProbes).Path
$dxvk = (Resolve-Path $DxvkProbes).Path
Import-Module (Join-Path $outputRoot 'viogpu-install-certificates.psm1') -Force
Add-Type -Path (Join-Path $outputRoot 'viogpu-install-native.cs')
$cat = Join-Path $driver 'viogpuwddm.cat'
$cert = Assert-GpuBundledCertificate (Join-Path $outputRoot 'DroidVM_Test.cer') $cat
$created = @()
try {
    foreach ($store in @('Root','TrustedPublisher')) {
        if ([DroidVmGpuInstall.Native]::AddCertificateNew($store, $cert.RawData, $true)) {
            $created += [pscustomobject]@{Store=$store;Location='LocalMachine';Thumbprint=$cert.Thumbprint;
                Hash=(Get-GpuCertificateHash $cert);Created=$true;Status='created'}
        }
    }
    $manifestPath = Join-Path $driver 'viogpu-flat-package.json'
    $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
    [DroidVmGpuInstall.Native]::VerifyCatalogMember($cat, $manifestPath)
    $count = 1
    foreach ($entry in $manifest.files.PSObject.Properties) {
        $path = Join-Path $driver $entry.Name
        if ((Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.Value) {
            throw "Signed inventory hash changed: $($entry.Name)"
        }
        [DroidVmGpuInstall.Native]::VerifyCatalogMember($cat, $path)
        if ([IO.Path]::GetExtension($path) -in @('.sys','.dll','.exe')) {
            $signature = Get-AuthenticodeSignature -LiteralPath $path
            if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate -or
                (Get-GpuCertificateHash $signature.SignerCertificate) -cne (Get-GpuCertificateHash $cert)) {
                throw "PE does not have the common valid signer: $path"
            }
        }
        $count++
    }
    Write-Host "PASS $count actual Windows catalog members and exact PE signer"
    foreach ($script in Get-ChildItem -LiteralPath $outputRoot -File | Where-Object Extension -in @('.ps1','.psm1')) {
        $signature = Get-AuthenticodeSignature -LiteralPath $script.FullName
        if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate -or
            (Get-GpuCertificateHash $signature.SignerCertificate) -cne (Get-GpuCertificateHash $cert)) {
            throw "Invalid unified installer script signature: $($script.Name)"
        }
    }
    foreach ($arch in @('arm64','x64','x86')) {
        $loader = if ($arch -eq 'x86') {'OpenCL32.dll'} else {'OpenCL.dll'}
        & (Join-Path $driver $manifest.loader_probes.$arch) (Join-Path $driver $loader)
        if ($LASTEXITCODE) { throw "Signed $arch public loader ABI failed" }
        & (Join-Path $cl "windows-flat-check-$arch.exe") (Join-Path $driver "viogpucl_$arch.dll") "viogpucl_vk_$arch.dll"
        if ($LASTEXITCODE) { throw "Signed $arch CL runtime/compiler ABI failed" }
        $icd = if ($arch -eq 'x86') {'viogpuopengl_x86.dll'} else {'viogpuopengl.dll'}
        foreach ($mode in @('--vulkan','--opengl')) {
            & (Join-Path $gl "small-stack-probe-$arch.exe") $mode (Join-Path $driver $icd)
            if ($LASTEXITCODE) { throw "Signed $arch $mode constrained-stack validation failed" }
        }
        & (Join-Path $gl "system-probe-$arch.exe") --load-only $driver
        if ($LASTEXITCODE) { throw "Signed $arch GL/Vulkan ABI failed" }
        & (Join-Path $gl "gles-probe-$arch.exe") --load-only $driver
        if ($LASTEXITCODE) { throw "Signed $arch GLES ABI failed" }
        # ARM64 and x64 processes enter through the ARM64X viogpud3dx.dll, x86
        # through viogpud3d_x86.dll; each must reach its own Mesa UMD build.
        & (Join-Path $d3d "d3d-umd-probe-$arch.exe") $driver
        if ($LASTEXITCODE) { throw "Signed $arch D3D10 UMD ABI failed" }
    }
    # DXVK candidates: ARM64 and x64 processes enter through the ARM64X
    # viogpudxvkx.dll, x86 through viogpudxvk_x86.dll. Each must reach its own
    # signed DXVK build, find its OpenAdapter10 gate closed and resolve its
    # private viogpu_gl_loader_<arch>.dll beside it.
    $dxvkLog = Join-Path $env:RUNNER_TEMP 'dxvk-umd-probe-log'
    New-Item -ItemType Directory -Force $dxvkLog | Out-Null
    foreach ($arch in @('arm64','x64','x86')) {
        $env:DXVK_LOG_PATH = $dxvkLog
        try {
            & (Join-Path $dxvk "$arch/dxvk-umd-probe-$arch.exe") $driver
            $probeExit = $LASTEXITCODE
        } finally {
            Remove-Item Env:DXVK_LOG_PATH
        }
        if ($probeExit) { throw "Signed $arch DXVK candidate UMD ABI failed" }
    }
    Get-ChildItem -LiteralPath $dxvkLog -File | ForEach-Object { Write-Host "== $($_.Name)"; Get-Content -LiteralPath $_.FullName }
    # The unified installer's own package reader must accept this receipt,
    # including the candidate fields it does not act on.
    Import-Module (Join-Path $outputRoot 'viogpu-install-state.psm1') -Force
    $installerAst = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $outputRoot 'viogpu-unified-install.ps1'), [ref]$null, [ref]$null)
    foreach ($function in $installerAst.FindAll({ param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -in @('Assert-FlatName', 'Read-FlatPackage') }, $true)) {
        . ([scriptblock]::Create($function.Extent.Text))
    }
    $package = Read-FlatPackage $driver
    if ($package.Manifest.candidate_activation -cne 'unregistered-candidate' -or
        @($package.Manifest.registration.PSObject.Properties.Name) -contains 'UserModeDriverName') {
        throw 'Unified installer reads DXVK as anything but an unregistered candidate'
    }
    Write-Host "PASS unified installer Read-FlatPackage accepts the signed receipt ($(@($package.Manifest.files.PSObject.Properties).Count) files)"
    Write-Host 'PASS signed flat native/EC/x86 GL, CL and D3D UMD loading, native/EC/x86 DXVK candidate loading; GPU rendering and driver binding remain untested'
} finally {
    Remove-GpuAttemptTrust $created
    $cert.Dispose()
}
