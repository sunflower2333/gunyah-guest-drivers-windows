[CmdletBinding()]
param(
    [string]$DumpPath,
    [string]$PrivatePdbPath,
    [string]$PrivateImagePath,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$kdPath = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe'
$privateSymbols = Join-Path $OutputDirectory 'private-symbols'
$privateImages = Join-Path $OutputDirectory 'private-images'
$symbolCache = Join-Path $OutputDirectory 'symbols'
$logPath = Join-Path $OutputDirectory 'kd-fast.log'
New-Item -ItemType Directory -Path $privateSymbols,$privateImages,$symbolCache -Force | Out-Null
Copy-Item -LiteralPath $PrivatePdbPath -Destination (Join-Path $privateSymbols 'viogpuwddm.pdb') -Force
Copy-Item -LiteralPath $PrivateImagePath -Destination (Join-Path $privateImages 'viogpuwddm.sys') -Force
$symbolPath = "$privateSymbols;$symbolCache"
$imagePath = "$privateImages;C:\Windows\System32;C:\Windows\System32\drivers"
$commands = @(
    '.symopt- 0x40'
    '.reload /f viogpuwddm.sys'
    '.bugcheck'
    '!analyze -v'
    'lmvm viogpuwddm'
    'r'
    'kP'
    'ub @pc'
    'u @pc'
    '!thread'
    'q'
) -join '; '
& $kdPath -z $DumpPath -y $symbolPath -i $imagePath -logo $logPath -c $commands
if ($LASTEXITCODE -ne 0) { throw "kd.exe exited with code $LASTEXITCODE." }
[pscustomobject]@{
    DumpPath = $DumpPath
    LogPath = $logPath
    LogLength = (Get-Item -LiteralPath $logPath).Length
} | ConvertTo-Json
