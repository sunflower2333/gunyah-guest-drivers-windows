[CmdletBinding()]
param(
    [string]$OutputPath = 'C:\DroidVM\viogpu-58043\viogpu-interactive-screen.png',
    [string]$MetadataPath = 'C:\DroidVM\viogpu-58043\viogpu-interactive-screen.json'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

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

    $samples = @()
    foreach ($point in @(
        @(0, 0),
        @([int]($bounds.Width / 2), [int]($bounds.Height / 2)),
        @($bounds.Width - 1, $bounds.Height - 1)
    )) {
        $color = $bitmap.GetPixel($point[0], $point[1])
        $samples += [pscustomobject]@{
            X = $point[0]
            Y = $point[1]
            Argb = ('0x{0:X8}' -f $color.ToArgb())
        }
    }

    [pscustomobject]@{
        CapturedAt = (Get-Date).ToString('o')
        ProcessId = $PID
        SessionId = (Get-Process -Id $PID).SessionId
        Bounds = [pscustomobject]@{
            X = $bounds.X
            Y = $bounds.Y
            Width = $bounds.Width
            Height = $bounds.Height
        }
        Samples = $samples
        OutputPath = $OutputPath
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $MetadataPath -Encoding UTF8
}
finally {
    $graphics.Dispose()
    $bitmap.Dispose()
}
