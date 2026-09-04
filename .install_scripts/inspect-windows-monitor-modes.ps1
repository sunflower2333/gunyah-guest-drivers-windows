[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$listedModes = @(
    Get-CimInstance -Namespace 'root\wmi' -ClassName WmiMonitorListedSupportedSourceModes -ErrorAction SilentlyContinue
)
$monitors = foreach ($monitor in $listedModes) {
    $modes = foreach ($mode in $monitor.MonitorSourceModes) {
        $horizontalTotal = $mode.HorizontalActivePixels + $mode.HorizontalBlankingPixels
        $verticalTotal = $mode.VerticalActivePixels + $mode.VerticalBlankingPixels
        [pscustomobject]@{
            Width = $mode.HorizontalActivePixels
            Height = $mode.VerticalActivePixels
            PixelClockRate = $mode.PixelClockRate
            RefreshHz = if ($horizontalTotal -ne 0 -and $verticalTotal -ne 0) {
                [Math]::Round($mode.PixelClockRate / ($horizontalTotal * $verticalTotal), 3)
            }
            else {
                $null
            }
        }
    }
    [pscustomobject]@{
        InstanceName = $monitor.InstanceName
        ModeCount = $monitor.NumOfMonitorSourceModes
        Modes = @($modes)
    }
}

$rawSources = @(
    Get-CimInstance -Namespace 'root\wmi' -ClassName WmiMonitorRawEEdidV1Block -ErrorAction SilentlyContinue
)
if ($rawSources.Count -eq 0) {
    $rawSources = @(
        Get-CimInstance -Namespace 'root\wmi' -ClassName WmiMonitorDescriptorMethods -ErrorAction SilentlyContinue |
            ForEach-Object {
                $result = Invoke-CimMethod -InputObject $_ -MethodName WmiGetMonitorRawEEdidV1Block -Arguments @{ BlockId = 0 }
                [pscustomobject]@{
                    InstanceName = $_.InstanceName
                    BlockId = 0
                    EdidV1Block = $result.BlockContent
                }
            }
    )
}

$rawEdids = foreach ($raw in $rawSources) {
    [byte[]]$bytes = $raw.EdidV1Block
    [uint32]$checksum = 0
    foreach ($value in $bytes) {
        $checksum = ($checksum + $value) -band 0xFF
    }
    $horizontalActive = [uint32]$bytes[56] + (([uint32]$bytes[58] -band 0xF0) -shl 4)
    $horizontalBlanking = [uint32]$bytes[57] + (([uint32]$bytes[58] -band 0x0F) -shl 8)
    $verticalActive = [uint32]$bytes[59] + (([uint32]$bytes[61] -band 0xF0) -shl 4)
    $verticalBlanking = [uint32]$bytes[60] + (([uint32]$bytes[61] -band 0x0F) -shl 8)
    [pscustomobject]@{
        InstanceName = $raw.InstanceName
        BlockId = $raw.BlockId
        Length = $bytes.Length
        ChecksumModulo256 = $checksum
        Version = "{0}.{1}" -f $bytes[18], $bytes[19]
        FeatureSupport = ('0x{0:X2}' -f $bytes[24])
        DetailedTiming = [pscustomobject]@{
            PixelClock10KHz = [uint32]$bytes[54] + ([uint32]$bytes[55] -shl 8)
            Width = $horizontalActive
            Height = $verticalActive
            HorizontalBlanking = $horizontalBlanking
            VerticalBlanking = $verticalBlanking
            SyncFlags = ('0x{0:X2}' -f $bytes[71])
        }
        Hex = (($bytes | ForEach-Object { $_.ToString('X2') }) -join '')
    }
}

$configurationRoot = 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Configuration'
$configurations = @()
if (Test-Path -LiteralPath $configurationRoot) {
    $configurations = @(
        Get-ChildItem -LiteralPath $configurationRoot -Recurse -ErrorAction SilentlyContinue |
            ForEach-Object {
                $properties = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue
                if ($null -eq $properties) {
                    return
                }
                $interesting = [ordered]@{}
                foreach ($name in @(
                    'PrimSurfSize.cx',
                    'PrimSurfSize.cy',
                    'ActiveSize.cx',
                    'ActiveSize.cy',
                    'DwmClipBox.left',
                    'DwmClipBox.top',
                    'DwmClipBox.right',
                    'DwmClipBox.bottom'
                )) {
                    if ($null -ne $properties.PSObject.Properties[$name]) {
                        $interesting[$name] = $properties.$name
                    }
                }
                if ($interesting.Count -ne 0) {
                    [pscustomobject]@{
                        Path = $_.Name
                        Values = $interesting
                    }
                }
            }
    )
}

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    Monitors = @($monitors)
    RawEdids = @($rawEdids)
    GraphicsConfigurations = $configurations
} | ConvertTo-Json -Depth 8
