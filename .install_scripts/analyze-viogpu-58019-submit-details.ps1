[CmdletBinding()]
param(
    [string]$DumpPath = 'D:\Windows\MEMORY.DMP',
    [string]$SubmitArgumentsAddress = 'ffff948e12819720',
    [string]$AdapterAddress = 'ffffd204a131a000',
    [string]$SymbolCachePath = 'C:\Symbols',
    [string]$PublicSymbolServer = 'https://msdl.microsoft.com/download/symbols',
    [string]$AnalysisRoot = 'C:\DroidVM\viogpu-58019\dump-analysis'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$kdPath = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe'
$privateSymbolPath = Join-Path $AnalysisRoot 'private-symbols'
$privateImagePath = Join-Path $AnalysisRoot 'private-images'
$symbolPath = "$privateSymbolPath;srv*$SymbolCachePath*$PublicSymbolServer"
$imagePath = "$privateImagePath;D:\Windows\System32;D:\Windows\System32\drivers"
$logPath = Join-Path $AnalysisRoot 'kd-submit-details.txt'

foreach ($requiredPath in @($kdPath, $DumpPath, $privateSymbolPath, $privateImagePath)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required path does not exist: $requiredPath"
    }
}
foreach ($address in @($SubmitArgumentsAddress, $AdapterAddress)) {
    if ($address -notmatch '^[0-9A-Fa-f]{16}$') {
        throw "Address must contain exactly 16 hexadecimal digits: $address"
    }
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
    ".echo ===== DXGKARG_SUBMITCOMMAND $SubmitArgumentsAddress =====",
    "dq $SubmitArgumentsAddress L20",
    "dt viogpuwddm!_DXGKARG_SUBMITCOMMAND $SubmitArgumentsAddress",
    "dx -r2 *(viogpuwddm!_DXGKARG_SUBMITCOMMAND*)0x$SubmitArgumentsAddress",
    ".echo ===== VioGpuDod $AdapterAddress =====",
    "dt viogpuwddm!VioGpuDod $AdapterAddress m_HardwareResetState m_HardwareRundownCompleted m_pHWDevice m_NativeSubmittedFence m_NativeCompletedFence m_NativeFenceHead m_NativeFenceCount",
    '.echo ===== SUBMIT CALL SITE =====',
    'ln viogpuwddm!VioGpuWddmSubmitCommand+150',
    'u viogpuwddm!VioGpuWddmSubmitCommand L100',
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
    SubmitArgumentsAddress = $SubmitArgumentsAddress
    AdapterAddress = $AdapterAddress
    LogPath = $logPath
    LogLength = (Get-Item -LiteralPath $logPath).Length
    DebuggerExitCode = $debuggerExitCode
} | ConvertTo-Json
