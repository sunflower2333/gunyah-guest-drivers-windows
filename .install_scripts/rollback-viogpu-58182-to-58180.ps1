[CmdletBinding()]
param(
    [string]$RollbackRoot = 'C:\Users\USER\viogpu-rollback-pre-58182-27a4b80c',
    [string]$EvidenceRoot = 'C:\Users\USER\viogpu-58182-27a4b80c\evidence',
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedCurrentVersion = '100.6.101.58182'
$expectedCurrentImageHash = 'a6c1009691958bec6df629752f12beb190a95cc8e0d138548247bd8e0ade7229'
$expectedCurrentUmdHash = 'bd862478c4dbf81da0204f2004fb96f2f1d005ab38bdfcde3d794fedfd28bb3e'
$expectedRollbackVersion = '100.6.101.58180'

function Get-GpuSnapshot {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
    })
    if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
    $device = $devices[0]
    $property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
    $driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($property.Data)"
    $driverKey = Get-Item -LiteralPath $driverKeyPath
    $service = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $imagePath = [Environment]::ExpandEnvironmentVariables([string]$service.GetValue('ImagePath', '')).Trim('"')
    if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
    }
    $image = Get-Item -LiteralPath $imagePath
    $umdPath = Join-Path $image.DirectoryName 'viogpud3d.dll'
    [pscustomobject]@{
        Status = [string]$device.Status
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        ImagePath = $image.FullName
        ImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        UmdPath = $umdPath
        UmdSha256 = if (Test-Path -LiteralPath $umdPath -PathType Leaf) { (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
    }
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Rollback must run elevated.' }
if (-not (Test-Path -LiteralPath $RollbackRoot -PathType Container)) { throw "Missing rollback directory: $RollbackRoot" }

$resolvedRoot = (Resolve-Path -LiteralPath $RollbackRoot).Path.TrimEnd('\')
$manifestPath = Join-Path $resolvedRoot 'rollback-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Missing rollback manifest: $manifestPath" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$files = @($manifest.Files)
if ($files.Count -eq 0) { throw 'Rollback manifest contains no files.' }

$verifiedFiles = @()
foreach ($entry in $files) {
    $relativePath = [string]$entry.Path
    if ([string]::IsNullOrWhiteSpace($relativePath) -or [IO.Path]::IsPathRooted($relativePath)) {
        throw "Unsafe rollback path in manifest: '$relativePath'"
    }
    $path = [IO.Path]::GetFullPath((Join-Path $resolvedRoot $relativePath))
    if (-not $path.StartsWith($resolvedRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Rollback path escapes root: '$relativePath'"
    }
    $file = Get-Item -LiteralPath $path
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($file.Length -ne [long]$entry.Length -or $hash -ne [string]$entry.Sha256) {
        throw "Rollback manifest mismatch for '$relativePath'"
    }
    $verifiedFiles += $file
}

$infFiles = @($verifiedFiles | Where-Object { $_.Extension -ieq '.inf' })
if ($infFiles.Count -ne 1) { throw "Expected one rollback INF, found $($infFiles.Count)." }
$infPath = $infFiles[0].FullName
$infText = Get-Content -LiteralPath $infPath -Raw
if ($infText -notmatch 'DriverVer\s*=\s*[^,]+,100\.6\.101\.58180') { throw "Rollback INF is not version $expectedRollbackVersion" }
if ($infText -notmatch 'PCI\\VEN_1AF4&DEV_1050' -or $infText -match '(?i)netkvm|rdmapool|droidvmpool|DRVM0001') { throw 'Rollback INF does not describe only the expected virtio-gpu device.' }

$before = Get-GpuSnapshot
if ($before.DriverVersion -ne $expectedCurrentVersion -or $before.ImageSha256 -ne $expectedCurrentImageHash -or $before.UmdSha256 -ne $expectedCurrentUmdHash) {
    throw "Unexpected pre-rollback GPU state: $($before | ConvertTo-Json -Compress)"
}
if ($ValidateOnly) {
    [ordered]@{
        ValidationOnly = $true
        RollbackVersion = $expectedRollbackVersion
        VerifiedFileCount = $verifiedFiles.Count
        RollbackInf = $infPath
        Before = $before
    } | ConvertTo-Json -Depth 6
    return
}

$rollbackStartedAt = Get-Date
$rollbackOutput = @(& pnputil.exe /add-driver $infPath /install 2>&1)
$rollbackExitCode = $LASTEXITCODE
if ($rollbackExitCode -notin @(0, 3010)) { throw "pnputil rollback failed ($rollbackExitCode): $($rollbackOutput -join [Environment]::NewLine)" }
Start-Sleep -Seconds 10
$after = Get-GpuSnapshot
$result = [ordered]@{
    RollbackStartedAt = $rollbackStartedAt.ToString('o')
    RollbackCompletedAt = (Get-Date).ToString('o')
    RollbackVersion = $expectedRollbackVersion
    PnpUtilExitCode = $rollbackExitCode
    PnpUtilOutput = $rollbackOutput
    RebootRequired = $rollbackExitCode -eq 3010
    VerifiedFileCount = $verifiedFiles.Count
    Before = $before
    After = $after
}
New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'rollback-result.json') -Encoding UTF8
$result | ConvertTo-Json -Depth 8
