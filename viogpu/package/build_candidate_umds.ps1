param(
    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    # Output of the dxvk-umd CI job (viogpu/dxvk/build-umd.ps1): one directory
    # per architecture holding the built UMD and its dxvk-umd.json record.
    [Parameter(Mandatory = $true)]
    [string]$DxvkRoot,

    [string]$DxvkCommit = 'a9fe3eb7a23804bf2bee014021e73ed6f84aefd9',
    [string]$Vkd3dCommit = '376e716e4acdf7a9ded138b0099f2ee8a8863f91'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-CommitSha([string]$Name, [string]$Value) {
    if ($Value -notmatch '^[0-9a-f]{40}$') {
        throw "$Name must be a lowercase 40-character git commit SHA"
    }
}

function Assert-Arm64Pe([string]$Path) {
    Assert-PeMachine $Path 0xaa64
}

function Assert-PeMachine([string]$Path, [int]$Machine) {
    $stream = [IO.File]::OpenRead((Resolve-Path -LiteralPath $Path))
    try {
        $reader = [IO.BinaryReader]::new($stream)
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) {
            throw "$Path does not have a valid MZ header"
        }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadUInt32()
        if ($peOffset -gt $stream.Length - 6) {
            throw "$Path has an invalid PE header offset"
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "$Path does not have a PE signature"
        }
        $actual = $reader.ReadUInt16()
        if ($actual -ne $Machine) {
            throw ("{0} PE machine is {1:X4}, not {2:X4}" -f $Path, $actual, $Machine)
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Assert-Exports([string]$Path, [string[]]$Required) {
    $dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source
    $exports = (& $dumpbin /nologo /exports $Path 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for $Path"
    }
    foreach ($symbol in $Required) {
        if ($exports -notmatch "(?m)\b$([regex]::Escape($symbol))\b") {
            throw "$Path does not export $symbol"
        }
    }
}

function Checkout-ExactCommit(
    [string]$Repository,
    [string]$Commit,
    [string]$Destination
) {
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null

    & git -C $Destination init --quiet
    if ($LASTEXITCODE -ne 0) { throw "git init failed for $Repository" }
    & git -C $Destination remote add origin "https://github.com/$Repository.git"
    if ($LASTEXITCODE -ne 0) { throw "git remote add failed for $Repository" }
    & git -C $Destination fetch --depth 1 origin $Commit
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed for $Repository@$Commit" }
    & git -C $Destination checkout --detach --quiet FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { throw "git checkout failed for $Repository@$Commit" }

    $actual = (& git -C $Destination rev-parse HEAD).Trim()
    if ($actual -ne $Commit) {
        throw "$Repository resolved to $actual instead of pinned $Commit"
    }

    & git -C $Destination submodule update --init --recursive --depth 1
    if ($LASTEXITCODE -ne 0) {
        & git -C $Destination submodule update --init --recursive
        if ($LASTEXITCODE -ne 0) { throw "submodule checkout failed for $Repository@$Commit" }
    }
}

function Invoke-Arm64DeveloperCommand(
    [string]$WorkingDirectory,
    [string]$CommandLine,
    [string]$Name
) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "vswhere.exe not found: $vswhere"
    }
    $installation = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath).Trim()
    if (-not $installation) {
        $installation = (& $vswhere -latest -products * -property installationPath).Trim()
    }
    if (-not $installation) {
        throw 'No Visual Studio installation found'
    }
    $vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) {
        throw "vcvarsall.bat not found: $vcvars"
    }

    $cmdFile = Join-Path $env:RUNNER_TEMP ("droidvm-umd-$Name.cmd")
    @(
        '@echo off',
        "call `"$vcvars`" x64_arm64 || exit /b %errorlevel%",
        "cd /d `"$WorkingDirectory`" || exit /b %errorlevel%",
        $CommandLine,
        'exit /b %errorlevel%'
    ) | Set-Content -LiteralPath $cmdFile -Encoding ascii

    & cmd.exe /d /c $cmdFile
    $exitCode = $LASTEXITCODE
    Remove-Item -LiteralPath $cmdFile -Force -ErrorAction SilentlyContinue
    if ($exitCode -ne 0) {
        throw "$Name ARM64 UMD build failed with exit code $exitCode"
    }
}

Assert-CommitSha 'DxvkCommit' $DxvkCommit
Assert-CommitSha 'Vkd3dCommit' $Vkd3dCommit

$output = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $output) {
    Remove-Item -LiteralPath $output -Recurse -Force
}
New-Item -ItemType Directory -Path $output -Force | Out-Null

$workRoot = Join-Path $env:RUNNER_TEMP 'droidvm-candidate-umds'
New-Item -ItemType Directory -Path $workRoot -Force | Out-Null
$vkd3dRoot = Join-Path $workRoot 'vkd3d-proton'

# DXVK is built, gated and probed by its own CI job, one leg per architecture;
# accept only that job's exact output for the pinned commit. The arm64 leg also
# carries the ARM64X entry, which has no gate or loader of its own.
$dxvkCandidates = [ordered]@{}
foreach ($leg in @(
    @{ Arch = 'arm64'; Name = 'viogpudxvk.dll'; Machine = 'arm64'; Pe = 0xaa64; Role = 'candidate-runtime' },
    @{ Arch = 'arm64'; Name = 'viogpudxvkx.dll'; Machine = 'arm64x'; Pe = 0xaa64; Role = 'candidate-entry' },
    @{ Arch = 'x64'; Name = 'viogpudxvk_x64.dll'; Machine = 'x64'; Pe = 0x8664; Role = 'candidate-runtime' },
    @{ Arch = 'x86'; Name = 'viogpudxvk_x86.dll'; Machine = 'x86'; Pe = 0x14c; Role = 'candidate-runtime' }
)) {
    $recordPath = Join-Path $DxvkRoot "$($leg.Arch)/dxvk-umd.json"
    $record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
    if ($record.schema -ne 1 -or $record.family -cne 'dxvk' -or $record.architecture -cne $leg.Arch -or
        $record.activation -cne 'unregistered-candidate' -or $record.sources.dxvk -cne $DxvkCommit) {
        throw "DXVK job output is not the pinned $($leg.Arch) candidate: $recordPath"
    }
    $dll = Join-Path $DxvkRoot "$($leg.Arch)/$($leg.Name)"
    $entry = $record.files.PSObject.Properties[$leg.Name]
    if (-not $entry -or -not (Test-Path -LiteralPath $dll -PathType Leaf) -or $entry.Value.machine -cne $leg.Machine -or
        $entry.Value.role -cne $leg.Role -or
        (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.Value.sha256) {
        throw "DXVK job output changed or is incomplete: $dll"
    }
    Assert-PeMachine $dll $leg.Pe
    Assert-Exports $dll @('OpenAdapter10', 'OpenAdapter10_2')
    Copy-Item -LiteralPath $dll -Destination (Join-Path $output $leg.Name)
    $dxvkCandidates[$leg.Name] = @{ Machine = $leg.Machine; Record = $record; Runtime = $leg.Role -ceq 'candidate-runtime' }
}

# tools\build-windows-umd.ps1 expects meson and ninja on PATH. The inline DXVK
# build used to install them first, as a side effect of its own script; DXVK
# now builds in the dxvk-umd job, so install the same unpinned tools here.
& python -m pip install --disable-pip-version-check meson ninja
if ($LASTEXITCODE -ne 0) { throw 'meson/ninja installation for the VKD3D candidate failed' }

Checkout-ExactCommit 'sunflower2333/vkd3d-proton' $Vkd3dCommit $vkd3dRoot
Invoke-Arm64DeveloperCommand $vkd3dRoot `
    'pwsh -NoProfile -ExecutionPolicy Bypass -File tools\build-windows-umd.ps1 -Architecture arm64' `
    'vkd3d'

$vkd3dDll = Join-Path $vkd3dRoot 'build-umd-arm64\libs\vkd3d-umd\viogpud3d12.dll'
if (-not (Test-Path -LiteralPath $vkd3dDll -PathType Leaf)) {
    throw "VKD3D build did not produce $vkd3dDll"
}
Assert-Arm64Pe $vkd3dDll
Assert-Exports $vkd3dDll @('OpenAdapter12')
Copy-Item -LiteralPath $vkd3dDll -Destination (Join-Path $output 'viogpud3d12.dll')

$files = [ordered]@{}
foreach ($name in $dxvkCandidates.Keys) {
    $candidate = $dxvkCandidates[$name]
    $files[$name] = [ordered]@{
        family = 'dxvk'
        machine = $candidate.Machine
        role = 'candidate-runtime'
        sha256 = (Get-FileHash -LiteralPath (Join-Path $output $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if ($candidate.Runtime) {
        # Carry the DXVK job's observed admission gate and loader choice into the
        # package receipt: the reason the candidate stays unregistered travels with it.
        $files[$name]['admission'] = [string]$candidate.Record.gates.gate_state
        $files[$name]['vulkan_loader'] = [string]$candidate.Record.vulkan_loader
    }
}
$files['viogpud3d12.dll'] = [ordered]@{
    family = 'vkd3d'
    machine = 'arm64'
    role = 'candidate-runtime'
    sha256 = (Get-FileHash -LiteralPath (Join-Path $output 'viogpud3d12.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
}

$manifest = [ordered]@{
    schema = 1
    activation = 'unregistered-candidate'
    sources = [ordered]@{
        dxvk = $DxvkCommit
        vkd3d = $Vkd3dCommit
    }
    files = $files
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'candidate-sources.json') -Encoding utf8

Write-Host "DXVK candidate: $DxvkCommit"
Write-Host "VKD3D candidate: $Vkd3dCommit"
Write-Host "Candidate UMDs staged flat in $output; activation remains unregistered-candidate."
