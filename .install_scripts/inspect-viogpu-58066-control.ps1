[CmdletBinding()]
param(
    [string]$DumpPath,
    [string]$PrivatePdbPath,
    [string]$PrivateImagePath,
    [string]$OutputDirectory,
    [string]$OwnerAddress = 'ffffe40f87f3da70'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$kdPath = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe'
$privateSymbols = Join-Path $OutputDirectory 'private-symbols'
$privateImages = Join-Path $OutputDirectory 'private-images'
$logPath = Join-Path $OutputDirectory 'kd-control.log'
New-Item -ItemType Directory -Path $privateSymbols,$privateImages -Force | Out-Null
Copy-Item -LiteralPath $PrivatePdbPath -Destination (Join-Path $privateSymbols 'viogpuwddm.pdb') -Force
Copy-Item -LiteralPath $PrivateImagePath -Destination (Join-Path $privateImages 'viogpuwddm.sys') -Force
$symbolPath = $privateSymbols
$imagePath = "$privateImages;C:\Windows\System32;C:\Windows\System32\drivers"
$commands = @(
    '.symopt- 0x40'
    '.reload /f viogpuwddm.sys'
    '.frame /r 9'
    'r'
    'dv /V /t'
    'u @pc-0x60 @pc+0x80'
    "dt viogpuwddm!VIOGPU_NATIVE_CONTEXT_OWNER $OwnerAddress"
    "dq $OwnerAddress L30"
    "!pte poi($OwnerAddress+0x40)"
    'q'
) -join '; '
& $kdPath -z $DumpPath -y $symbolPath -i $imagePath -logo $logPath -c $commands
if ($LASTEXITCODE -ne 0) { throw "kd.exe exited with code $LASTEXITCODE." }
[pscustomobject]@{ LogPath = $logPath; LogLength = (Get-Item -LiteralPath $logPath).Length } | ConvertTo-Json
