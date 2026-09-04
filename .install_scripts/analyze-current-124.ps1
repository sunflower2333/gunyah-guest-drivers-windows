[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PackageRoot = 'C:\Users\Administrator\droidvm-test\kmd-32980523436',
    [string]$OutputPath = 'C:\Users\Administrator\droidvm-test\kmt-current-124-analysis.txt'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$privateSymbols = Join-Path $PackageRoot 'viogpu'
$imagePath = "$privateSymbols;C:\Windows\System32;C:\Windows\System32\drivers"
$commands = @(
    '.symopt- 0x40'
    '.reload /f viogpuwddm.sys'
    '.bugcheck'
    '!analyze -v'
    '.ecxr'
    'r'
    'kP'
    '.frame /r 9'
    'r'
    'dv /V /t'
    '.frame /r 10'
    'r'
    'dv /V /t'
    '.frame /r 11'
    'r'
    'dv /V /t'
    '.frame /r 12'
    'r'
    'dv /V /t'
    'dt viogpuwddm!VIOGPU_NATIVE_CONTEXT_OWNER ffffe381157c7b90'
    'dt viogpuwddm!VioGpuAdapter ffffe381129a7000'
    'dt viogpuwddm!CPciResources ffffe381129a71f8'
    '!pte ffffde79faa00000'
    '!address ffffde79faa00000'
    'dq ffffde79faa00000 L10'
    'q'
)
$output = & $debugger -z $DumpPath -y $privateSymbols -i $imagePath -c ($commands -join '; ') 2>&1
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $LASTEXITCODE
