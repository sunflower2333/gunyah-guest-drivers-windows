param(
    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [string]$DxvkCommit = 'a98c19fbc8020ef7f7b47c79ccffe2fcb3c9ad74',
    [string]$Vkd3dCommit = 'ac1debbbcba455e2faa10e3a4990711c813b6e63'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-CommitSha([string]$Name, [string]$Value) {
    if ($Value -notmatch '^[0-9a-f]{40}$') {
        throw "$Name must be a lowercase 40-character git commit SHA"
    }
}

function Assert-Arm64Pe([string]$Path) {
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
        if ($reader.ReadUInt16() -ne 0xaa64) {
            throw "$Path PE machine is not ARM64 (AA64)"
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
$dxvkRoot = Join-Path $workRoot 'dxvk'
$vkd3dRoot = Join-Path $workRoot 'vkd3d-proton'
$dxvkOutput = Join-Path $workRoot 'dxvk-package'

Checkout-ExactCommit 'sunflower2333/dxvk' $DxvkCommit $dxvkRoot
Invoke-Arm64DeveloperCommand $dxvkRoot `
    "pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\build-native-umd.ps1 -OutputDirectory `"$dxvkOutput`"" `
    'dxvk'

$dxvkDll = Join-Path $dxvkOutput 'viogpudxvk.dll'
if (-not (Test-Path -LiteralPath $dxvkDll -PathType Leaf)) {
    throw "DXVK build did not produce $dxvkDll"
}
Assert-Arm64Pe $dxvkDll
Assert-Exports $dxvkDll @('OpenAdapter10', 'OpenAdapter10_2')
Copy-Item -LiteralPath $dxvkDll -Destination (Join-Path $output 'viogpudxvk.dll')

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
foreach ($entry in @(
    @{ Name = 'viogpudxvk.dll'; Family = 'dxvk' },
    @{ Name = 'viogpud3d12.dll'; Family = 'vkd3d' }
)) {
    $path = Join-Path $output $entry.Name
    $files[$entry.Name] = [ordered]@{
        family = $entry.Family
        machine = 'arm64'
        role = 'candidate-runtime'
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
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
