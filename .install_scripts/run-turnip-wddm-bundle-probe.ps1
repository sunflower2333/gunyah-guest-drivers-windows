param(
    [Parameter(Mandatory = $true)]
    [string]$BundleRoot,

    [Parameter(Mandatory = $true)]
    [ValidateSet('Lifecycle', 'Compute', 'Graphics', 'Win32')]
    [string]$Probe,

    [string]$OutputRoot = 'C:\DroidVM\TurnipRuns\evidence',

    [ValidateRange(10, 1800)]
    [int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-BundleHashes {
    param([Parameter(Mandatory = $true)][string]$Root)

    $manifestPath = Join-Path $Root 'SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Missing bundle hash manifest: $manifestPath"
    }

    $count = 0
    foreach ($line in Get-Content -LiteralPath $manifestPath) {
        if ($line -notmatch '^([0-9a-f]{64})  ([^\\/]+)$') {
            throw "Malformed SHA256SUMS entry: $line"
        }
        $expected = $Matches[1]
        $name = $Matches[2]
        $path = Join-Path $Root $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "SHA256SUMS references a missing file: $name"
        }
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
        if ($actual -ne $expected) {
            throw "Bundle hash mismatch for $name"
        }
        $count = $count + 1
    }
    return $count
}

$probeTable = @{
    Lifecycle = @{
        Executable = 'tu_wddm_vulkan_probe_arm64.exe'
        Arguments = @()
    }
    Compute = @{
        Executable = 'tu_wddm_vulkan_compute_probe_arm64.exe'
        Arguments = @('tu_wddm_compute.spv')
    }
    Graphics = @{
        Executable = 'tu_wddm_vulkan_graphics_probe_arm64.exe'
        Arguments = @('tu_wddm_graphics.vert.spv', 'tu_wddm_graphics.frag.spv')
    }
    Win32 = @{
        Executable = 'tu_wddm_win32_probe_arm64.exe'
        Arguments = @()
    }
}

$BundleRoot = (Resolve-Path -LiteralPath $BundleRoot).Path
$hashCount = Assert-BundleHashes -Root $BundleRoot
$probeSpec = $probeTable[$Probe]
$executable = Join-Path $BundleRoot $probeSpec.Executable
$arguments = @($probeSpec.Arguments | ForEach-Object { Join-Path $BundleRoot $_ })
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Missing probe executable: $executable"
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$prefix = Join-Path $OutputRoot ("{0}-{1}" -f $Probe.ToLowerInvariant(), $stamp)
$stdoutPath = "$prefix.stdout.txt"
$stderrPath = "$prefix.stderr.txt"
$resultPath = "$prefix.result.json"
$startLocal = Get-Date
$exitCode = $null

Push-Location -LiteralPath $BundleRoot
try {
    & $executable @arguments 1> $stdoutPath 2> $stderrPath
    $exitCode = $LASTEXITCODE
} finally {
    Pop-Location
}

$result = [ordered]@{
    Probe = $Probe
    BundleRoot = $BundleRoot
    BundleHashCount = $hashCount
    Started = $startLocal.ToString('o')
    Finished = (Get-Date).ToString('o')
    TimeoutSeconds = $TimeoutSeconds
    TimeoutEnforcedByCaller = $true
    ExitCode = $exitCode
    StdoutPath = $stdoutPath
    StderrPath = $stderrPath
    Stdout = if (Test-Path -LiteralPath $stdoutPath) {
        Get-Content -LiteralPath $stdoutPath -Raw
    } else { '' }
    Stderr = if (Test-Path -LiteralPath $stderrPath) {
        Get-Content -LiteralPath $stderrPath -Raw
    } else { '' }
}
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$result | ConvertTo-Json -Depth 8
if ($exitCode -ne 0) {
    exit $exitCode
}
