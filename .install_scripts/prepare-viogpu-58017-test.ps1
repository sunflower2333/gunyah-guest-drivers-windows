[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$staleDirectory = 'C:\DroidVM\viogpu-58016\runtime-logs'
$outputDirectory = 'C:\DroidVM\viogpu-58017\runtime-logs'
$removed = @()

if (Test-Path -LiteralPath $staleDirectory -PathType Container) {
    $removed = @(
        Get-ChildItem -LiteralPath $staleDirectory -File -ErrorAction Stop |
            Where-Object { $_.Extension -in @('.etl', '.json', '.csv', '.xml') } |
            Select-Object FullName, Length, LastWriteTimeUtc
    )
    Get-ChildItem -LiteralPath $staleDirectory -File -ErrorAction Stop |
        Where-Object { $_.Extension -in @('.etl', '.json', '.csv', '.xml') } |
        Remove-Item -Force
}

if (Test-Path -LiteralPath $outputDirectory) {
    $existing = @(Get-ChildItem -LiteralPath $outputDirectory -Force)
    if ($existing.Count -ne 0) {
        throw "Refusing to mix a new capture with $($existing.Count) existing item(s) in '$outputDirectory'."
    }
}
else {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

[pscustomobject]@{
    PreparedAt = (Get-Date).ToString('o')
    RemovedStaleLogs = $removed
    OutputDirectory = $outputDirectory
} | ConvertTo-Json -Depth 4
