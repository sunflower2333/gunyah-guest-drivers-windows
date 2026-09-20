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
        & cl @common /LD "$root/viogpu/video/mft_decoder.cpp" "$root/viogpu/video/video_client.cpp" setupapi.lib @libs /link /OUT:viogpuvideo_mft.dll
        if ($LASTEXITCODE) { throw "MFT build failed: $LASTEXITCODE" }
        & cl @common "$root/viogpu/video/mft_decoder.cpp" "$root/viogpu/tests/video/mft_contract_test.cpp" @libs /Fe:mft-contract-test.exe
        if ($LASTEXITCODE) { throw "MFT contract build failed: $LASTEXITCODE" }
        & ./mft-contract-test.exe
        if ($LASTEXITCODE) { throw "MFT contract test failed: $LASTEXITCODE" }
    } finally { Pop-Location }
} finally { Pop-Location }
