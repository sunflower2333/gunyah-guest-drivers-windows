[CmdletBinding()]
param([string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\evidence-desktop')
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($b.Width, $b.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
$g.Dispose()
$png = Join-Path $OutputDirectory 'desktop.png'
$bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)

# Summarise so a black screen is obvious without transferring the image.
$distinct = @{}
for ($y = 0; $y -lt $bmp.Height; $y += 8) {
    for ($x = 0; $x -lt $bmp.Width; $x += 8) {
        $c = $bmp.GetPixel($x, $y)
        $k = "$($c.R),$($c.G),$($c.B)"
        $distinct[$k] = 1 + ($distinct[$k] | ForEach-Object { $_ })
    }
}
$bmp.Dispose()

$info = @(
    "SESSION_ID=$((Get-Process -Id $PID).SessionId)",
    "SCREEN=$($b.Width)x$($b.Height)",
    "PNG_BYTES=$((Get-Item $png).Length)",
    "DISTINCT_SAMPLED_COLOURS=$($distinct.Keys.Count)",
    "TOP_COLOURS=" + (($distinct.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 5 |
        ForEach-Object { "$($_.Key)x$($_.Value)" }) -join ' ')
)
$info | Set-Content -LiteralPath (Join-Path $OutputDirectory 'desktop.txt') -Encoding UTF8
