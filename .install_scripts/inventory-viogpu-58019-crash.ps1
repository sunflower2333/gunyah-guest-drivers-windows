[CmdletBinding()]
param(
    [char]$DriveLetter = 'D'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = "${DriveLetter}:\"
$dumpPath = Join-Path $root 'Windows\MEMORY.DMP'
$runtimeRoot = Join-Path $root 'DroidVM\viogpu-58019'
$driverStoreRoot = Join-Path $root 'Windows\System32\DriverStore\FileRepository'
$systemDriverPath = Join-Path $root 'Windows\System32\drivers\viogpuwddm.sys'

$runtimeArtifacts = @()
if (Test-Path -LiteralPath $runtimeRoot -PathType Container) {
    $runtimeArtifacts = @(
        Get-ChildItem -LiteralPath $runtimeRoot -File -Recurse -ErrorAction Stop |
            Select-Object FullName, Length, CreationTimeUtc, LastWriteTimeUtc
    )
}

$driverPackages = @(
    Get-ChildItem -LiteralPath $driverStoreRoot -Directory -Filter 'viogpuwddm.inf_arm64_*' -ErrorAction SilentlyContinue |
        ForEach-Object {
            [pscustomobject]@{
                Directory = $_.FullName
                Files = @(
                    Get-ChildItem -LiteralPath $_.FullName -File |
                        ForEach-Object {
                            [pscustomobject]@{
                                Name = $_.Name
                                Length = $_.Length
                                Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
                            }
                        }
                )
            }
    }
)
$systemDriver = $null
if (Test-Path -LiteralPath $systemDriverPath -PathType Leaf) {
    $systemDriver = [pscustomobject]@{
        Path = $systemDriverPath
        Length = (Get-Item -LiteralPath $systemDriverPath).Length
        Sha256 = (Get-FileHash -LiteralPath $systemDriverPath -Algorithm SHA256).Hash
    }
}

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    Dump = Get-Item -LiteralPath $dumpPath | Select-Object FullName, Length, CreationTimeUtc, LastWriteTimeUtc
    DumpSha256 = (Get-FileHash -LiteralPath $dumpPath -Algorithm SHA256).Hash
    RuntimeArtifacts = $runtimeArtifacts
    SystemDriver = $systemDriver
    DriverPackages = $driverPackages
} | ConvertTo-Json -Depth 6
