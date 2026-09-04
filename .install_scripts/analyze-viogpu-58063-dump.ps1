[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PrivatePdbPath = 'C:\Users\Administrator\viogpu-58063\viogpuwddm.pdb',
    [string]$PrivateImagePath = 'C:\Users\Administrator\viogpu-58063\viogpuwddm.sys',
    [string]$OutputDirectory = 'C:\Users\Administrator\viogpu-analysis-58063',
    [string]$ErrorRecordAddress = 'ffffcf0d48a8f038'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$kdPath = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe'
foreach ($path in @($kdPath, $DumpPath, $PrivatePdbPath, $PrivateImagePath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing required debugger input: $path"
    }
}

$privateSymbols = Join-Path $OutputDirectory 'private-symbols'
$privateImages = Join-Path $OutputDirectory 'private-images'
$symbolCache = Join-Path $OutputDirectory 'symbols'
$logPath = Join-Path $OutputDirectory 'kd.log'
New-Item -ItemType Directory -Path $privateSymbols, $privateImages, $symbolCache -Force | Out-Null
Copy-Item -LiteralPath $PrivatePdbPath -Destination (Join-Path $privateSymbols 'viogpuwddm.pdb') -Force
Copy-Item -LiteralPath $PrivateImagePath -Destination (Join-Path $privateImages 'viogpuwddm.sys') -Force

$symbolPath = "$privateSymbols;srv*$symbolCache*https://msdl.microsoft.com/download/symbols"
$imagePath = "$privateImages;C:\Windows\System32;C:\Windows\System32\drivers"
$commands = @(
    '.symopt- 0x40'
    '.reload /f nt'
    '.reload /f viogpuwddm.sys'
    '.bugcheck'
    "!errrec $ErrorRecordAddress"
    '!analyze -v'
    'lmvm viogpuwddm'
    'r'
    'kP'
    'ub @pc'
    'u @pc'
    '!thread'
    'q'
) -join '; '

if (Test-Path -LiteralPath $logPath) {
    Remove-Item -LiteralPath $logPath -Force
}
& $kdPath -z $DumpPath -y $symbolPath -i $imagePath -logo $logPath -c $commands
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    throw "kd.exe exited with code $exitCode."
}

[pscustomobject]@{
    AnalyzedAt = (Get-Date).ToString('o')
    DumpPath = $DumpPath
    DumpLength = (Get-Item -LiteralPath $DumpPath).Length
    DumpSha256 = (Get-FileHash -LiteralPath $DumpPath -Algorithm SHA256).Hash.ToLowerInvariant()
    ImageSha256 = (Get-FileHash -LiteralPath $PrivateImagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    PdbSha256 = (Get-FileHash -LiteralPath $PrivatePdbPath -Algorithm SHA256).Hash.ToLowerInvariant()
    ErrorRecordAddress = $ErrorRecordAddress
    SymbolPath = $symbolPath
    LogPath = $logPath
    LogLength = (Get-Item -LiteralPath $logPath).Length
    DebuggerExitCode = $exitCode
} | ConvertTo-Json -Depth 4
