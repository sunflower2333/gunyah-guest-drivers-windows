[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [Parameter(Mandatory = $true)]
    [string]$MetadataPath,
    [ValidateRange(5, 180)]
    [int]$WaitSeconds = 90,
    [ValidateRange(1, 16)]
    [int]$SampleStride = 4,
    [ValidateRange(64, 100000)]
    [int]$MinimumTargetSamples = 256
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

foreach ($path in @($OutputPath, $MetadataPath)) {
    if (Test-Path -LiteralPath $path) {
        throw "Refusing to overwrite interactive capture evidence: $path"
    }
}
$outputDirectory = Split-Path -Parent $OutputPath
$metadataDirectory = Split-Path -Parent $MetadataPath
foreach ($directory in @($outputDirectory, $metadataDirectory)) {
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        throw "Interactive capture directory does not exist: $directory"
    }
}

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class DroidVmWindowCaptureNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetWindowRect(IntPtr window, out RECT rectangle);
}
'@

function Test-CloseColor {
    param(
        [System.Drawing.Color]$Color,
        [int]$Red,
        [int]$Green,
        [int]$Blue,
        [int]$Tolerance = 20
    )

    return [Math]::Abs([int]$Color.R - $Red) -le $Tolerance -and
        [Math]::Abs([int]$Color.G - $Green) -le $Tolerance -and
        [Math]::Abs([int]$Color.B - $Blue) -le $Tolerance
}

function Measure-ProbeColors {
    param([System.Drawing.Bitmap]$Bitmap)

    [int64]$sampleCount = 0
    [int64]$firstFrameSamples = 0
    [int64]$secondFrameSamples = 0
    for ($y = 0; $y -lt $Bitmap.Height; $y += $SampleStride) {
        for ($x = 0; $x -lt $Bitmap.Width; $x += $SampleStride) {
            $color = $Bitmap.GetPixel($x, $y)
            $sampleCount++
            if (Test-CloseColor $color 32 128 223) {
                $firstFrameSamples++
            }
            if (Test-CloseColor $color 191 64 223) {
                $secondFrameSamples++
            }
        }
    }
    [pscustomobject]@{
        SampleCount = $sampleCount
        FirstFrameSamples = $firstFrameSamples
        SecondFrameSamples = $secondFrameSamples
        TargetSamples = $firstFrameSamples + $secondFrameSamples
    }
}

$deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
$lastError = $null
do {
    $probes = @(Get-Process -Name 'tu_wddm_win32_probe_arm64' -ErrorAction SilentlyContinue |
        Where-Object { $_.SessionId -ne 0 })
    foreach ($probe in $probes) {
        $bitmap = $null
        $graphics = $null
        try {
            $probe.Refresh()
            $window = $probe.MainWindowHandle
            if ($window -eq [IntPtr]::Zero) {
                continue
            }

            $rectangle = [DroidVmWindowCaptureNative+RECT]::new()
            if (-not [DroidVmWindowCaptureNative]::GetWindowRect($window, [ref]$rectangle)) {
                continue
            }
            $width = $rectangle.Right - $rectangle.Left
            $height = $rectangle.Bottom - $rectangle.Top
            if ($width -lt 64 -or $height -lt 64) {
                continue
            }

            $bitmap = [System.Drawing.Bitmap]::new(
                $width,
                $height,
                [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
            )
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            $graphics.CopyFromScreen(
                [System.Drawing.Point]::new($rectangle.Left, $rectangle.Top),
                [System.Drawing.Point]::Empty,
                [System.Drawing.Size]::new($width, $height)
            )
            $measurement = Measure-ProbeColors $bitmap
            if ($measurement.TargetSamples -lt $MinimumTargetSamples) {
                continue
            }

            $bitmap.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
            $result = [ordered]@{
                CapturedAt = (Get-Date).ToString('o')
                CaptureProcessId = $PID
                CaptureSessionId = (Get-Process -Id $PID).SessionId
                ProbeProcessId = $probe.Id
                ProbeSessionId = $probe.SessionId
                WindowHandle = ('0x{0:X}' -f $window.ToInt64())
                WindowTitle = $probe.MainWindowTitle
                Left = $rectangle.Left
                Top = $rectangle.Top
                Width = $width
                Height = $height
                SampleStride = $SampleStride
                SampleCount = $measurement.SampleCount
                FirstFrameSamples = $measurement.FirstFrameSamples
                SecondFrameSamples = $measurement.SecondFrameSamples
                TargetSamples = $measurement.TargetSamples
                MinimumTargetSamples = $MinimumTargetSamples
                OutputPath = $OutputPath
                OutputSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $OutputPath).Hash.ToLowerInvariant()
            }
            [IO.File]::WriteAllText($MetadataPath, ($result | ConvertTo-Json -Depth 5))
            $result | ConvertTo-Json -Depth 5
            exit 0
        } catch {
            $lastError = $_.Exception.Message
        } finally {
            if ($null -ne $graphics) {
                $graphics.Dispose()
            }
            if ($null -ne $bitmap) {
                $bitmap.Dispose()
            }
        }
    }
    Start-Sleep -Milliseconds 10
} while ([DateTime]::UtcNow -lt $deadline)

$suffix = if ([string]::IsNullOrEmpty($lastError)) { '' } else { " Last capture error: $lastError" }
throw "Timed out waiting for visible Turnip Win32 clear colors.$suffix"
