[CmdletBinding()]
param(
    [switch]$RequirePackagingTools
)

$ErrorActionPreference = 'Stop'

function Add-UniquePath {
    param(
        [System.Collections.Generic.List[string]]$List,
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }
    $fullPath = [IO.Path]::GetFullPath($Path.TrimEnd('\'))
    if ((Test-Path -LiteralPath $fullPath -PathType Container) -and -not $List.Contains($fullPath)) {
        $List.Add($fullPath)
    }
}

function Find-Tool {
    param(
        [string]$Root,
        [string]$Name
    )

    $matches = @(Get-ChildItem -LiteralPath $Root -Recurse -Filter $Name -File -ErrorAction SilentlyContinue)
    if ($matches.Count -eq 0) {
        return $null
    }
    $matches |
        Sort-Object @{ Expression = {
            if ($_.FullName -match '\\x64\\') { 0 }
            elseif ($_.FullName -match '\\x86\\') { 1 }
            elseif ($_.FullName -match '\\arm64\\') { 2 }
            else { 3 }
        } }, FullName |
        Select-Object -First 1
}

$roots = [System.Collections.Generic.List[string]]::new()
Add-UniquePath $roots (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10')
Add-UniquePath $roots (Join-Path ${env:ProgramFiles} 'Windows Kits\10')
Add-UniquePath $roots $env:KitsRoot10
Add-UniquePath $roots $env:WindowsSdkDir

if ($roots.Count -eq 0) {
    throw 'No preinstalled Windows Kits\10 root was found'
}

$kits = foreach ($root in $roots) {
    $includeRoot = Join-Path $root 'Include'
    $libRoot = Join-Path $root 'lib'
    $binRoot = Join-Path $root 'bin'
    if (-not (Test-Path -LiteralPath $includeRoot -PathType Container)) {
        continue
    }

    $includes = @(Get-ChildItem -LiteralPath $includeRoot -Directory -ErrorAction SilentlyContinue)
    foreach ($include in $includes) {
        try { $version = [version]$include.Name } catch { continue }
        $header = Join-Path $include.FullName 'km\ntddk.h'
        if (-not (Test-Path -LiteralPath $header -PathType Leaf)) {
            continue
        }

        $versionLibRoot = Join-Path $libRoot $include.Name
        $kernelLib = $null
        $directKernelLib = Join-Path $versionLibRoot 'km\arm64\ntoskrnl.lib'
        if (Test-Path -LiteralPath $directKernelLib -PathType Leaf) {
            $kernelLib = Get-Item -LiteralPath $directKernelLib
        }
        else {
            $kernelLib = @(Get-ChildItem -LiteralPath $versionLibRoot -Recurse -Filter 'ntoskrnl.lib' -File -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '\\km\\arm64\\' } |
                Sort-Object FullName |
                Select-Object -First 1)
            if ($kernelLib.Count -eq 0) {
                $kernelLib = $null
            }
            else {
                $kernelLib = $kernelLib[0]
            }
        }
        if (-not $kernelLib) {
            continue
        }

        $versionBinRoot = Join-Path $binRoot $include.Name
        $tracewpp = Find-Tool $versionBinRoot 'tracewpp.exe'
        if (-not $tracewpp) {
            continue
        }

        $infverif = Find-Tool $versionBinRoot 'InfVerif.exe'
        if (-not $infverif) {
            $infverif = Find-Tool (Join-Path $root 'Tools') 'InfVerif.exe'
        }
        if (-not $infverif) {
            continue
        }

        $signtool = Find-Tool $versionBinRoot 'signtool.exe'
        $inf2cat = Find-Tool $versionBinRoot 'inf2cat.exe'
        $stampinf = Find-Tool $versionBinRoot 'stampinf.exe'
        if ($RequirePackagingTools -and (-not $signtool -or -not $inf2cat -or -not $stampinf)) {
            continue
        }

        [pscustomobject]@{
            Version = $version
            Name = $include.Name
            Root = $root
            Bin = $versionBinRoot
            Header = $header
            KernelLib = $kernelLib.FullName
            Tracewpp = $tracewpp.FullName
            InfVerif = $infverif.FullName
            SignTool = if ($signtool) { $signtool.FullName } else { '' }
            Inf2Cat = if ($inf2cat) { $inf2cat.FullName } else { '' }
            StampInf = if ($stampinf) { $stampinf.FullName } else { '' }
        }
    }
}

$kit = @($kits | Sort-Object Version -Descending | Select-Object -First 1)
if ($kit.Count -eq 0) {
    Write-Host 'No complete Kit candidate. Candidate roots and per-version probes:'
    foreach ($root in $roots) {
        $includeRoot = Join-Path $root 'Include'
        $libRoot = Join-Path $root 'lib'
        $binRoot = Join-Path $root 'bin'
        Write-Host "  root=$root include=$([bool](Test-Path -LiteralPath $includeRoot -PathType Container))"
        foreach ($include in @(Get-ChildItem -LiteralPath $includeRoot -Directory -ErrorAction SilentlyContinue)) {
            try { $version = [version]$include.Name } catch { continue }
            $header = Join-Path $include.FullName 'km\ntddk.h'
            $versionLibRoot = Join-Path $libRoot $include.Name
            $versionBinRoot = Join-Path $binRoot $include.Name
            $kernel = @(Get-ChildItem -LiteralPath $versionLibRoot -Recurse -Filter 'ntoskrnl.lib' -File -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '\\km\\arm64\\' })
            $trace = @(Get-ChildItem -LiteralPath $versionBinRoot -Recurse -Filter 'tracewpp.exe' -File -ErrorAction SilentlyContinue)
            $inf = @(Get-ChildItem -LiteralPath $versionBinRoot -Recurse -Filter 'InfVerif.exe' -File -ErrorAction SilentlyContinue)
            Write-Host "    version=$($include.Name) header=$([bool](Test-Path -LiteralPath $header -PathType Leaf)) kernel=$($kernel.Count) trace=$($trace.Count) infverif=$($inf.Count)"
        }
    }
    $mode = if ($RequirePackagingTools) { ' and packaging tools' } else { '' }
    throw "No complete preinstalled ARM64 Windows SDK/WDK$mode was found"
}
$kit = $kit[0]

Write-Host "Using preinstalled Windows SDK/WDK $($kit.Name) from $($kit.Root)"
Write-Host "  tracewpp: $($kit.Tracewpp)"
Write-Host "  ntoskrnl: $($kit.KernelLib)"
Write-Host "  InfVerif: $($kit.InfVerif)"

$envFile = $env:GITHUB_ENV
if ([string]::IsNullOrWhiteSpace($envFile)) {
    throw 'GITHUB_ENV is not set; this script must run inside a GitHub Actions job'
}
$values = [ordered]@{
    DROIDVM_KIT_ROOT = $kit.Root
    DROIDVM_KIT_BIN = $kit.Bin
    DROIDVM_KIT_VERSION = $kit.Name
    INFVERIF_PATH = $kit.InfVerif
    TRACEWPP_PATH = $kit.Tracewpp
}
if ($RequirePackagingTools) {
    $values.SIGNTOOL_PATH = $kit.SignTool
    $values.INF2CAT_PATH = $kit.Inf2Cat
    $values.STAMPINF_PATH = $kit.StampInf
}
foreach ($entry in $values.GetEnumerator()) {
    "$($entry.Key)=$($entry.Value)" | Out-File -FilePath $envFile -Encoding utf8 -Append
}
