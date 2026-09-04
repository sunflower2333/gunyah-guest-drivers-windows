[CmdletBinding()]
param(
    [string]$RuntimeDirectory = 'C:\DroidVM\viogpu-58036\runtime-logs'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$controller = Get-CimInstance Win32_VideoController |
    Where-Object { $_.PNPDeviceID -like 'PCI\VEN_1AF4&DEV_1050*' } |
    Select-Object Name, DriverVersion, Status, CurrentHorizontalResolution,
        CurrentVerticalResolution, CurrentRefreshRate
$operatingSystem = Get-CimInstance Win32_OperatingSystem |
    Select-Object LastBootUpTime, LocalDateTime
$files = @(
    Get-ChildItem -LiteralPath $RuntimeDirectory -File -ErrorAction SilentlyContinue |
        Select-Object Name, Length, LastWriteTimeUtc
)

$csv = Get-ChildItem -LiteralPath $RuntimeDirectory -Filter '*.csv' -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1
$modeLines = @()
if ($null -ne $csv) {
    $modeLines = @(
        Select-String -LiteralPath $csv.FullName -Encoding UTF8 -Pattern @(
            'GetDisplayInfo',
            'SetCustomDisplay',
            'BuildModeList',
            'SetSourceModeAndPath',
            'modes[',
            'DxgkCbAcquirePostDisplayOwnership'
        ) -SimpleMatch |
            Select-Object -First 60 |
            ForEach-Object {
                if ($_.Line.Length -le 1000) { $_.Line } else { $_.Line.Substring(0, 1000) }
            }
    )
}

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    OperatingSystem = $operatingSystem
    Controller = $controller
    RuntimeFiles = $files
    CsvPath = if ($null -ne $csv) { $csv.FullName } else { $null }
    ModeLines = $modeLines
} | ConvertTo-Json -Depth 6
