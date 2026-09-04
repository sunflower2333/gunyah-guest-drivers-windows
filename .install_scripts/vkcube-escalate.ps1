[CmdletBinding()]
param(
    [string]$Root = 'C:\DroidVM\VulkanTools',
    [int]$Frames = 100,
    [string]$Log = 'C:\DroidVM\VulkanTools\evidence\escalate.txt'
)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path (Split-Path $Log) | Out-Null
$exe = Join-Path $Root 'vkcube.exe'
$sw = [Diagnostics.Stopwatch]::StartNew()
$out = & $exe --c $Frames 2>&1 | ForEach-Object { $_.ToString() }
$code = $LASTEXITCODE
$sw.Stop()
$line = "frames={0} elapsed={1:N2}s fps={2:N1} exit={3} out='{4}'" -f `
    $Frames, $sw.Elapsed.TotalSeconds, ($Frames / [math]::Max($sw.Elapsed.TotalSeconds, 0.001)), $code, ($out -join ' | ')
Add-Content -LiteralPath $Log -Value $line
Write-Output $line
