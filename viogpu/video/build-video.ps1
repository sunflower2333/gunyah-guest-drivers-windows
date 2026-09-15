# SPDX-License-Identifier: BSD-3-Clause
# Build the existing shared transport, integrated UMD, and VioGPU media function.
[CmdletBinding()]
param([string]$KitVersion = $env:DROIDVM_KIT_VERSION)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Push-Location $root
try {
    $common=@('/m','/p:Configuration=Win11 Release','/p:Platform=ARM64',
              '/p:ApiValidator_Enable=false','/p:SpectreMitigation=false','/p:SignMode=Off')
    if ($KitVersion) { $common += "/p:WindowsTargetPlatformVersion=$KitVersion" }
    foreach ($project in @('VirtIO/VirtioLib.vcxproj','VirtIO/WDF/VirtioLib-WDF.vcxproj',
                           'viogpu/viogpud3d/viogpud3d.vcxproj','viogpu/viogpuvideo/viogpuvideo.vcxproj')) {
        & msbuild $project @common
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $project ($LASTEXITCODE)" }
    }
} finally { Pop-Location }
