[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PrivatePdbPath = 'C:\Users\Administrator\viogpuwddm.pdb',
    [string]$PrivateImagePath = 'C:\Users\Administrator\viogpuwddm.sys',
    [string]$OutputDirectory = 'C:\Users\Administrator\viogpu-analysis-58053'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$kdPath = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe'
foreach ($path in @($kdPath, $DumpPath, $PrivatePdbPath, $PrivateImagePath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing $path" }
}
$privateSymbols = Join-Path $OutputDirectory 'private-symbols'
$privateImages = Join-Path $OutputDirectory 'private-images'
$symbolCache = Join-Path $OutputDirectory 'symbols'
$logPath = Join-Path $OutputDirectory 'kd.log'
New-Item -ItemType Directory -Path $privateSymbols,$privateImages,$symbolCache -Force | Out-Null
Copy-Item $PrivatePdbPath (Join-Path $privateSymbols 'viogpuwddm.pdb') -Force
Copy-Item $PrivateImagePath (Join-Path $privateImages 'viogpuwddm.sys') -Force
$symbolPath = "$privateSymbols;srv*$symbolCache*https://msdl.microsoft.com/download/symbols"
$imagePath = "$privateImages;C:\Windows\System32;C:\Windows\System32\drivers"
$commands = '.symopt- 0x40; .bugcheck; .reload /f viogpuwddm.sys; lmvm viogpuwddm; ln fffff80164efc000; !analyze -v; kP; q'
if (Test-Path $logPath) { Remove-Item $logPath -Force }
& $kdPath -z $DumpPath -y $symbolPath -i $imagePath -logo $logPath -c $commands
$exitCode = $LASTEXITCODE
Write-Output "KD_EXIT=$exitCode LOG=$logPath"
if ($exitCode -ne 0) { exit $exitCode }
Get-Content -LiteralPath $logPath
