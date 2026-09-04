[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$OutputDirectory = 'C:\Users\Administrator\viogpu-analysis-58063',
    [string]$TrapFrame = 'ffffe30e956b2e50',
    [string]$OwnerAddress = 'ffffcf0d49e43a90'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$kdPath = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe'
$privateSymbols = Join-Path $OutputDirectory 'private-symbols'
$privateImages = Join-Path $OutputDirectory 'private-images'
$symbolCache = Join-Path $OutputDirectory 'symbols'
$logPath = Join-Path $OutputDirectory 'kd-trap.log'
foreach ($path in @($kdPath, $DumpPath, $privateSymbols, $privateImages, $symbolCache)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing required debugger input: $path"
    }
}

$symbolPath = "$privateSymbols;srv*$symbolCache*https://msdl.microsoft.com/download/symbols"
$imagePath = "$privateImages;C:\Windows\System32;C:\Windows\System32\drivers"
$commands = @(
    '.symopt- 0x40'
    '.reload /f nt'
    '.reload /f viogpuwddm.sys'
    ".trap $TrapFrame"
    'r'
    'kP'
    'ub @pc'
    'u @pc'
    'dv /V /t'
    'dt viogpuwddm!VIOGPU_NATIVE_CONTEXT_OWNER'
    "dt viogpuwddm!VIOGPU_NATIVE_CONTEXT_OWNER $OwnerAddress"
    "dq $OwnerAddress L12"
    ".formats poi($OwnerAddress+0x40)"
    "!pte poi($OwnerAddress+0x40)"
    '!pte @x9'
    'db @x9 L20'
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
    TrapFrame = $TrapFrame
    OwnerAddress = $OwnerAddress
    LogPath = $logPath
    LogLength = (Get-Item -LiteralPath $logPath).Length
    DebuggerExitCode = $exitCode
} | ConvertTo-Json
