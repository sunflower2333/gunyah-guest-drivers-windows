[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EtlPath,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $EtlPath -PathType Leaf)) {
    throw "ETL file does not exist: $EtlPath"
}

$directory = Split-Path -Parent $EtlPath
$stem = [System.IO.Path]::GetFileNameWithoutExtension($EtlPath)
$csvPath = Join-Path $directory "$stem.csv"
$summaryPath = Join-Path $directory "$stem-summary.xml"
$analysisPath = Join-Path $directory "$stem-analysis.json"

& tracerpt.exe $EtlPath -o $csvPath -of CSV -summary $summaryPath -y | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "tracerpt failed with exit code $LASTEXITCODE."
}

$lines = @(Get-Content -LiteralPath $csvPath -Encoding UTF8)
$patterns = [ordered]@{
    DdiPresent = 'DdiPresent '
    DdiRender = 'DdiRender '
    DdiRenderKm = 'DdiRenderKm '
    DdiPatch = 'DdiPatch '
    DdiSubmitCommand = 'DdiSubmitCommand '
    DdiBuildPagingBuffer = 'DdiBuildPagingBuffer '
    DdiSetVidPnSourceAddress = 'DdiSetVidPnSourceAddress '
    DdiSetVidPnSourceVisibility = 'DdiSetVidPnSourceVisibility '
    DdiCreateAllocation = 'DdiCreateAllocation '
    DdiDestroyAllocation = 'DdiDestroyAllocation '
    DdiQueryAdapterInfo = 'DdiQueryAdapterInfo '
    OpenAdapter = 'OpenAdapter'
    AllocationBusy = 'STATUS_GRAPHICS_ALLOCATION_BUSY'
    InsufficientDmaBuffer = 'STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER'
    DeviceNotReady = 'STATUS_DEVICE_NOT_READY'
}

$counts = [ordered]@{}
foreach ($entry in $patterns.GetEnumerator()) {
    $counts[$entry.Key] = @($lines | Select-String -SimpleMatch $entry.Value).Count
}

$interestingPattern = 'DdiPresent|DdiRender|DdiPatch|DdiSubmitCommand|DdiBuildPagingBuffer|' +
    'DdiSetVidPnSource|DdiCreateAllocation|DdiDestroyAllocation|DdiQueryAdapterInfo|' +
    'OpenAdapter|STATUS_|invalid|failed|error'
$interestingLines = @(
    $lines |
        Select-String -Pattern $interestingPattern |
        Select-Object -First 500 |
        ForEach-Object { $_.Line }
)

$result = [pscustomobject]@{
    AnalyzedAt = (Get-Date).ToString('o')
    EtlPath = $EtlPath
    EtlLength = (Get-Item -LiteralPath $EtlPath).Length
    CsvPath = $csvPath
    CsvLineCount = $lines.Count
    SummaryPath = $summaryPath
    Counts = $counts
    InterestingLines = $interestingLines
}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $analysisPath -Encoding UTF8
if (-not $Quiet) {
    $result | ConvertTo-Json -Depth 6
}
