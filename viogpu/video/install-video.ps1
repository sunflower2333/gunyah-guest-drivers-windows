# SPDX-License-Identifier: BSD-3-Clause
# Install only the media PCI function. Does not replace display drivers, modify
# OpenGL/Vulkan settings, install rdmapool, or turn off signature enforcement.
[CmdletBinding(SupportsShouldProcess)]
param([Parameter(Mandatory)][string]$PackageDirectory)
$ErrorActionPreference='Stop'
$inf=Join-Path (Resolve-Path -LiteralPath $PackageDirectory) 'viogpuvideo.inf'
if (!(Test-Path -LiteralPath $inf -PathType Leaf)) { throw "Missing $inf" }
if ($PSCmdlet.ShouldProcess($inf,'Install signed VioGPU media function driver')) {
    & pnputil.exe /add-driver $inf /install
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "pnputil failed: $LASTEXITCODE" }
}
