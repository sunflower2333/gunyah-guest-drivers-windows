[CmdletBinding()]
param([string]$Out = 'C:\DroidVM\VulkanTools\evidence\desktop.png')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null

$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($b.Width, $b.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)

# Sample a grid; report how many pixels are not pure black.
$nonzero = 0; $total = 0; $first = $null
for ($y = 0; $y -lt $bmp.Height; $y += [math]::Max(1, [int]($bmp.Height / 40))) {
    for ($x = 0; $x -lt $bmp.Width; $x += [math]::Max(1, [int]($bmp.Width / 40))) {
        $p = $bmp.GetPixel($x, $y); $total++
        if ($null -eq $first) { $first = '{0:X2}{1:X2}{2:X2}' -f $p.R, $p.G, $p.B }
        if ($p.R -ne 0 -or $p.G -ne 0 -or $p.B -ne 0) { $nonzero++ }
    }
}
$stats = @(
    "session=$((Get-Process -Id $PID).SessionId)",
    "virtual_screen=$($b.Width)x$($b.Height) at $($b.X),$($b.Y)",
    "sampled=$total nonblack=$nonzero first_pixel=$first",
    "saved=$Out size=$((Get-Item $Out).Length)"
)
$stats | Set-Content -LiteralPath ($Out + '.txt') -Encoding UTF8
Write-Output "session=$((Get-Process -Id $PID).SessionId)"
Write-Output "virtual_screen=$($b.Width)x$($b.Height) at $($b.X),$($b.Y)"
Write-Output "sampled=$total nonblack=$nonzero first_pixel=$first"
Write-Output "saved=$Out size=$((Get-Item $Out).Length)"
$bmp.Dispose()
