# SPDX-License-Identifier: BSD-3-Clause
[CmdletBinding()]
param([string]$OutDir = 'artifacts/video-mft-arm64')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Push-Location $root
try {
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $out = [IO.Path]::GetFullPath($OutDir)
    Push-Location $out
    try {
        $common = @('/nologo', '/std:c++17', '/EHsc', '/W4', '/WX', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX')
        $libs = @('mfplat.lib', 'mfuuid.lib', 'ole32.lib')
        & cl @common /LD "$root/viogpu/video/mft_decoder.cpp" "$root/viogpu/video/video_client.cpp" setupapi.lib @libs /Fe:viogpuvideo_mft.dll /link /IMPLIB:viogpuvideo_mft.lib
        if ($LASTEXITCODE) { throw "MFT build failed: $LASTEXITCODE" }
        if (!(Test-Path 'viogpuvideo_mft.dll') -or !(Test-Path 'viogpuvideo_mft.lib')) { throw 'Missing flat MFT module/import library' }
        & cl @common "$root/viogpu/video/mft_decoder.cpp" "$root/viogpu/tests/video/mft_contract_test.cpp" @libs /Fe:mft-contract-test.exe
        if ($LASTEXITCODE) { throw "MFT contract build failed: $LASTEXITCODE" }
        & ./mft-contract-test.exe
        if ($LASTEXITCODE) { throw "MFT contract test failed: $LASTEXITCODE" }
        & cl @common "$root/viogpu/tests/video/mft_probe.cpp" viogpuvideo_mft.lib @libs bcrypt.lib /Fe:mft-probe.exe
        if ($LASTEXITCODE) { throw "MFT probe build failed: $LASTEXITCODE" }
        Copy-Item "$root/viogpu/tests/video/test-mft-device.ps1" .
        Copy-Item "$root/viogpu/video/MFT.md" .
    } finally { Pop-Location }
} finally { Pop-Location }
