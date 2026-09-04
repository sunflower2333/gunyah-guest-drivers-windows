[CmdletBinding()]
param(
    [string]$OutputPath = 'C:\DroidVM\TurnipRuns\evidence\win32-interactive-visible.png',
    [string]$MetadataPath = 'C:\DroidVM\TurnipRuns\evidence\win32-interactive-visible.json',
    [ValidateRange(5, 120)]
    [int]$WaitSeconds = 60
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

foreach ($path in @($OutputPath, $MetadataPath)) {
    if (Test-Path -LiteralPath $path) {
        throw "Refusing to overwrite interactive capture evidence: $path"
    }
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
$probe = $null
do {
    $probe = Get-Process -Name 'tu_wddm_win32_probe_arm64' -ErrorAction SilentlyContinue |
        Where-Object { $_.SessionId -ne 0 } |
        Select-Object -First 1
    if ($null -eq $probe) {
        Start-Sleep -Milliseconds 20
    }
} while ($null -eq $probe -and [DateTime]::UtcNow -lt $deadline)

if ($null -eq $probe) {
    throw 'Timed out waiting for an interactive Turnip Win32 probe process.'
}

Start-Sleep -Milliseconds 50
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bitmap = [System.Drawing.Bitmap]::new(
    $bounds.Width,
    $bounds.Height,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
try {
    $graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $bitmap.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $graphics.Dispose()
    $bitmap.Dispose()
}

$result = [ordered]@{
    CapturedAt = (Get-Date).ToString('o')
    CaptureProcessId = $PID
    CaptureSessionId = (Get-Process -Id $PID).SessionId
    ProbeProcessId = $probe.Id
    ProbeSessionId = $probe.SessionId
    Width = $bounds.Width
    Height = $bounds.Height
    OutputPath = $OutputPath
    OutputSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $OutputPath).Hash.ToLowerInvariant()
}
$result | ConvertTo-Json | Set-Content -LiteralPath $MetadataPath -Encoding UTF8
