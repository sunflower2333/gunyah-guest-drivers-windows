[CmdletBinding()]
param(
    [string]$DumpPath = 'D:\Windows\MEMORY.DMP',
    [string]$PrivatePdbPath = 'C:\Users\Administrator\viogpuwddm-58018-layout.pdb',
    [string]$PrivateImagePath = 'D:\Windows\System32\drivers\viogpuwddm.sys',
    [string]$SymbolCachePath = 'C:\Symbols',
    [string]$PublicSymbolServer = 'https://msdl.microsoft.com/download/symbols',
    [string]$OutputDirectory = 'C:\DroidVM\viogpu-58018\dump-analysis',
    [switch]$UseLayoutEquivalentPrivatePdb
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$kdPath = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe'
$privateSymbolDirectory = Join-Path $OutputDirectory 'private-symbols'
$privateImageDirectory = Join-Path $OutputDirectory 'private-images'
$logPath = Join-Path $OutputDirectory 'kd-analysis.txt'

foreach ($requiredPath in @(
        $kdPath,
        $DumpPath,
        $PrivatePdbPath,
        $PrivateImagePath,
        'D:\Windows\System32\ntoskrnl.exe'
    )) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required file does not exist: $requiredPath"
    }
}

New-Item -ItemType Directory -Path $privateSymbolDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $privateImageDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $SymbolCachePath -Force | Out-Null
Copy-Item -LiteralPath $PrivatePdbPath -Destination (Join-Path $privateSymbolDirectory 'viogpuwddm.pdb') -Force
Copy-Item -LiteralPath $PrivateImagePath -Destination (Join-Path $privateImageDirectory 'viogpuwddm.sys') -Force
Copy-Item -LiteralPath 'D:\Windows\System32\ntoskrnl.exe' -Destination (Join-Path $privateImageDirectory 'ntoskrnl.exe') -Force
Copy-Item -LiteralPath 'D:\Windows\System32\ntoskrnl.exe' -Destination (Join-Path $privateImageDirectory 'ntkrnlmp.exe') -Force

$symbolPath = "$privateSymbolDirectory;srv*$SymbolCachePath*$PublicSymbolServer"
$imagePath = "$privateImageDirectory;D:\Windows\System32;D:\Windows\System32\drivers"
$commands = @(
    '.symopt- 0x40',
    '.reload /f nt',
    $(if ($UseLayoutEquivalentPrivatePdb) { '.symopt+ 0x40' }),
    $(if ($UseLayoutEquivalentPrivatePdb) { '.reload /f /i viogpuwddm.sys' } else { '.reload /f viogpuwddm.sys' }),
    '.bugcheck',
    '!analyze -v',
    'lmvm viogpuwddm',
    'kP',
    '!thread',
    'q'
) -join '; '

if (Test-Path -LiteralPath $logPath) {
    Remove-Item -LiteralPath $logPath -Force
}

$arguments = @(
    '-z', $DumpPath,
    '-y', $symbolPath,
    '-i', $imagePath,
    '-logo', $logPath,
    '-c', $commands
)
& $kdPath @arguments
$debuggerExitCode = $LASTEXITCODE
if ($debuggerExitCode -ne 0) {
    throw "kd.exe exited with code $debuggerExitCode."
}

[pscustomobject]@{
    AnalyzedAt = (Get-Date).ToString('o')
    Dump = Get-Item -LiteralPath $DumpPath | Select-Object FullName, Length, LastWriteTimeUtc
    DumpSha256 = (Get-FileHash -LiteralPath $DumpPath -Algorithm SHA256).Hash
    PdbSha256 = (Get-FileHash -LiteralPath $PrivatePdbPath -Algorithm SHA256).Hash
    ImageSha256 = (Get-FileHash -LiteralPath $PrivateImagePath -Algorithm SHA256).Hash
    SymbolPath = $symbolPath
    DownloadedKernelPdbs = @(
        Get-ChildItem -LiteralPath $SymbolCachePath -Recurse -File -Filter 'ntkrnlmp.pdb' -ErrorAction SilentlyContinue |
            Select-Object FullName, Length, LastWriteTimeUtc
    )
    LogPath = $logPath
    LogLength = (Get-Item -LiteralPath $logPath).Length
    DebuggerExitCode = $debuggerExitCode
} | ConvertTo-Json -Depth 4
