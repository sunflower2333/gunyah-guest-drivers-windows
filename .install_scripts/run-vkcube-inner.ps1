[CmdletBinding()]
param(
    [string]$Exe = 'C:\DroidVM\VulkanTools\vkcube.exe',
    [string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\evidence-vkcube',
    [int]$Frames = 600
)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$stdout = Join-Path $OutputDirectory 'vkcube.stdout.txt'
$exitFile = Join-Path $OutputDirectory 'vkcube.exit.txt'
Remove-Item -LiteralPath $stdout, $exitFile -ErrorAction SilentlyContinue

$header = @(
    "SESSION_ID=$((Get-Process -Id $PID).SessionId)",
    "USER=$env:USERNAME",
    "EXE=$Exe",
    "SHA256=$((Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash.ToLowerInvariant())",
    "FRAMES=$Frames",
    "STARTED=$((Get-Date).ToString('o'))",
    '--- output ---'
)
$lines = @(& $Exe --c $Frames 2>&1 | ForEach-Object { $_.ToString() })
$code = $LASTEXITCODE
($header + $lines + @("FINISHED=$((Get-Date).ToString('o'))")) | Set-Content -LiteralPath $stdout -Encoding UTF8
"$code" | Set-Content -LiteralPath $exitFile -Encoding UTF8
