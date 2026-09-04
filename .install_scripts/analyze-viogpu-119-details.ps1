[CmdletBinding()]
param(
    [string]$DumpPath = 'D:\Windows\MEMORY.DMP',
    [string]$PatchArgumentsAddress = 'ffffe3876876f9e0',
    [string]$SymbolCachePath = 'C:\Symbols',
    [string]$PublicSymbolServer = 'https://msdl.microsoft.com/download/symbols',
    [string]$AnalysisRoot = 'C:\DroidVM\viogpu-58018\dump-analysis'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$kdPath = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe'
$privateSymbolPath = Join-Path $AnalysisRoot 'private-symbols'
$privateImagePath = Join-Path $AnalysisRoot 'private-images'
$symbolPath = "$privateSymbolPath;srv*$SymbolCachePath*$PublicSymbolServer"
$imagePath = "$privateImagePath;D:\Windows\System32;D:\Windows\System32\drivers"
$logPath = Join-Path $AnalysisRoot 'kd-119-details.txt'

foreach ($requiredPath in @($kdPath, $DumpPath, $privateSymbolPath, $privateImagePath)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required path does not exist: $requiredPath"
    }
}
if ($PatchArgumentsAddress -notmatch '^[0-9A-Fa-f]{16}$') {
    throw "PatchArgumentsAddress must contain exactly 16 hexadecimal digits: $PatchArgumentsAddress"
}

$commands = @(
    '.symopt+ 0x40',
    '.reload /f nt',
    '.reload /f /i viogpuwddm.sys',
    '.reload /f dxgmms1.sys',
    '.bugcheck',
    'kP',
    '.frame /r 0xc',
    'dv /t /v',
    ".echo ===== DXGKARG_PATCH $PatchArgumentsAddress =====",
    "dq $PatchArgumentsAddress L20",
    "dt viogpuwddm!_DXGKARG_PATCH $PatchArgumentsAddress",
    "dx -r2 *(viogpuwddm!_DXGKARG_PATCH*)0x$PatchArgumentsAddress",
    '.echo ===== PATCH CALL SITE =====',
    'ln viogpuwddm!VioGpuWddmPatch+e24',
    'u viogpuwddm!VioGpuWddmPatch+dc0 L50',
    '.echo ===== PRIVATE TYPES =====',
    'dt viogpuwddm!VIOGPU_WDDM_KMD_DMA_PRIVATE',
    'dt viogpuwddm!VIOGPU_WDDM_PAGING_PRIVATE',
    'dt viogpuwddm!VIOGPU_WDDM_PAGING_DMA_PACKET',
    'q'
)

if (Test-Path -LiteralPath $logPath) {
    Remove-Item -LiteralPath $logPath -Force
}

$arguments = @(
    '-z', $DumpPath,
    '-y', $symbolPath,
    '-i', $imagePath,
    '-logo', $logPath,
    '-c', ($commands -join '; ')
)
& $kdPath @arguments
$debuggerExitCode = $LASTEXITCODE
if ($debuggerExitCode -ne 0) {
    throw "kd.exe exited with code $debuggerExitCode."
}

[pscustomobject]@{
    AnalyzedAt = (Get-Date).ToString('o')
    DumpSha256 = (Get-FileHash -LiteralPath $DumpPath -Algorithm SHA256).Hash
    PatchArgumentsAddress = $PatchArgumentsAddress
    LogPath = $logPath
    LogLength = (Get-Item -LiteralPath $logPath).Length
    DebuggerExitCode = $debuggerExitCode
} | ConvertTo-Json
