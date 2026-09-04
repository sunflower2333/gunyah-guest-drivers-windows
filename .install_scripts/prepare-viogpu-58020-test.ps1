[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$outputDirectory = 'C:\DroidVM\viogpu-58020\runtime-logs'
$staleDirectories = @(
    'C:\DroidVM\viogpu-58016\runtime-logs',
    'C:\DroidVM\viogpu-58017\runtime-logs',
    'C:\DroidVM\viogpu-58018\runtime-logs',
    'C:\DroidVM\viogpu-58019\runtime-logs'
)
$removed = @()

foreach ($staleDirectory in $staleDirectories) {
    if (-not (Test-Path -LiteralPath $staleDirectory -PathType Container)) {
        continue
    }
    $staleFiles = @(
        Get-ChildItem -LiteralPath $staleDirectory -File -ErrorAction Stop |
            Where-Object { $_.Extension -in @('.etl', '.json', '.csv', '.xml') }
    )
    $removed += $staleFiles | Select-Object FullName, Length, LastWriteTimeUtc
    $staleFiles | Remove-Item -Force
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
