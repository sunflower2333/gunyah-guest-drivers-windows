# SPDX-License-Identifier: MIT
# Actual architecture DLL loading and fail-closed controls on Windows on ARM.
param([Parameter(Mandatory)][string]$Payload)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path -LiteralPath $Payload).Path
$scratch = Join-Path $env:RUNNER_TEMP ('flat-gl-controls-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch | Out-Null
function Invoke-Probe([string]$Probe, [string[]]$Arguments, [bool]$Success) {
    $process = Start-Process -FilePath $Probe -ArgumentList $Arguments -NoNewWindow -PassThru
    $null = $process.Handle
    if (!$process.WaitForExit(30000)) {
        $process.Kill(); $process.WaitForExit()
        throw 'Flat-runtime probe exceeded 30 seconds'
    }
    $process.Refresh()
    if ($null -eq $process.ExitCode) { throw 'Missing probe exit code' }
    $expected = if ($Success) { 0 } else { 1 }
    if ($process.ExitCode -ne $expected) { throw "Unexpected probe exit $($process.ExitCode): $Probe $Arguments" }
}
try {
    foreach ($arch in @('arm64','x64','x86')) {
        $probe = Join-Path $root "system-probe-$arch.exe"
        $private = "viogpu_gl_vk_$arch.dll"
        $fixture = Join-Path $scratch "missing-$arch"
        Copy-Item -LiteralPath $root -Destination $fixture -Recurse
        Remove-Item -LiteralPath (Join-Path $fixture $private)
        Invoke-Probe $probe @('--load-default', ('"{0}"' -f $fixture)) $false
        Write-Host "PASS $arch rejects missing private Turnip"
        $wrongArch = if ($arch -eq 'arm64') { 'x86' } else { 'arm64' }
        Copy-Item -LiteralPath (Join-Path $root "viogpu_gl_vk_$wrongArch.dll") -Destination (Join-Path $fixture $private)
        Invoke-Probe $probe @('--load-default', ('"{0}"' -f $fixture)) $false
        Write-Host "PASS $arch rejects wrong architecture private Turnip"
        Copy-Item -LiteralPath (Join-Path $root $private) -Destination (Join-Path $fixture $private) -Force
        $glesProbe = Join-Path $root "gles-probe-$arch.exe"
        $egl = "viogpu_egl_$arch.dll"
        Remove-Item -LiteralPath (Join-Path $fixture $egl)
        Invoke-Probe $glesProbe @('--load-only', ('"{0}"' -f $fixture)) $false
        Write-Host "PASS $arch rejects missing private EGL"
        Copy-Item -LiteralPath (Join-Path $root "viogpu_egl_$wrongArch.dll") -Destination (Join-Path $fixture $egl)
        Invoke-Probe $glesProbe @('--load-only', ('"{0}"' -f $fixture)) $false
        Write-Host "PASS $arch rejects wrong architecture private EGL"
        $foreign = Join-Path $scratch "foreign-$arch"
        New-Item -ItemType Directory $foreign | Out-Null
        Copy-Item -LiteralPath (Join-Path $root $private) -Destination $foreign
        Invoke-Probe $probe @('--load-foreign', ('"{0}"' -f $root), ('"{0}"' -f (Join-Path $foreign $private))) $false
        Write-Host "PASS $arch rejects same-name private DLL from another package"
        # Load a real Vulkan loader under the public basename before the ICD.
        # Its presence must not be mistaken for the private GL loader.
        $public = Join-Path $foreign 'vulkan-1.dll'
        Copy-Item -LiteralPath (Join-Path $root "viogpu_gl_loader_$arch.dll") -Destination $public
        Invoke-Probe $probe @('--load-foreign', ('"{0}"' -f $root), ('"{0}"' -f $public)) $true
        Write-Host "PASS $arch coexists with an already loaded public Vulkan loader"
    }
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force
}
