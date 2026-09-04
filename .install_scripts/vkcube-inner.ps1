[CmdletBinding()]
param(
    [string]$Root = 'C:\DroidVM\VulkanTools',
    [string]$OutputDirectory = 'C:\DroidVM\VulkanTools\evidence',
    [int]$Frames = 900
)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$stdout = Join-Path $OutputDirectory 'vkcube.stdout.txt'
$exitFile = Join-Path $OutputDirectory 'vkcube.exit.txt'
Remove-Item -LiteralPath $stdout, $exitFile -ErrorAction SilentlyContinue

$exe = Join-Path $Root 'vkcube.exe'
Set-Location $Root
$sw = [Diagnostics.Stopwatch]::StartNew()
$lines = @(& $exe --c $Frames 2>&1 | ForEach-Object { $_.ToString() })
$code = $LASTEXITCODE
$sw.Stop()

$header = @(
    "SESSION_ID=$((Get-Process -Id $PID).SessionId)",
    "USER=$env:USERNAME",
    "EXE=$exe",
    "SHA256=$((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant())",
    "FRAMES=$Frames",
    "ELAPSED_SEC=$([math]::Round($sw.Elapsed.TotalSeconds,2))",
    "FPS=$([math]::Round($Frames / [math]::Max($sw.Elapsed.TotalSeconds, 0.001), 1))",
    "RAN_AT=$((Get-Date).ToString('o'))",
    '--- output ---'
)
($header + $lines) | Set-Content -LiteralPath $stdout -Encoding UTF8
"$code" | Set-Content -LiteralPath $exitFile -Encoding UTF8
