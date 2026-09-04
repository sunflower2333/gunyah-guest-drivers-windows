[CmdletBinding()]
param(
    [string]$BundleRoot = 'C:\DroidVM\TurnipRuns\run-33322445949',
    [string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\evidence-win32'
)

$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$stdout = Join-Path $OutputDirectory 'win32-probe.stdout.txt'
$exitFile = Join-Path $OutputDirectory 'win32-probe.exit.txt'
Remove-Item -LiteralPath $stdout, $exitFile -ErrorAction SilentlyContinue

$exe = Join-Path $BundleRoot 'tu_wddm_win32_probe_arm64.exe'
Set-Location $BundleRoot
$lines = @(& $exe 2>&1 | ForEach-Object { $_.ToString() })
$code = $LASTEXITCODE

$header = @(
    "SESSION_ID=$((Get-Process -Id $PID).SessionId)",
    "USER=$env:USERNAME",
    "EXE=$exe",
    "SHA256=$((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant())",
    "RAN_AT=$((Get-Date).ToString('o'))",
    '--- output ---'
)
($header + $lines) | Set-Content -LiteralPath $stdout -Encoding UTF8
"$code" | Set-Content -LiteralPath $exitFile -Encoding UTF8
