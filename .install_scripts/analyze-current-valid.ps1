[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PackageRoot = 'C:\Users\Administrator\droidvm-test\kmd-32980523436',
    [string]$OutputPath = 'C:\Users\Administrator\droidvm-test\current-valid-analysis.txt'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$privateSymbols = Join-Path $PackageRoot 'viogpu'
$imagePath = "$privateSymbols;C:\Windows\System32;C:\Windows\System32\drivers"
$commands = @(
    '.symopt- 0x40'
    ".reload /f viogpuwddm.sys"
    '.bugcheck'
    '!analyze -v'
    '.ecxr'
    'r'
    'kP'
    'lmvm viogpuwddm'
    '!thread'
    '.frame /r 0n0'
    'r'
    'dv /V /t'
    '.frame /r 0n1'
    'r'
    'dv /V /t'
    '.frame /r 0n2'
    'r'
    'dv /V /t'
    '.frame /r 0n3'
    'r'
    'dv /V /t'
    '.frame /r 0n4'
    'r'
    'dv /V /t'
    '.frame /r 0n5'
    'r'
    'dv /V /t'
    '.frame /r 0n6'
    'r'
    'dv /V /t'
    '.frame /r 0n7'
    'r'
    'dv /V /t'
    '.frame /r 0n8'
    'r'
    'dv /V /t'
    '.frame /r 0n9'
    'r'
    'dv /V /t'
    '.frame /r 0n10'
    'r'
    'dv /V /t'
    '.frame /r 0n11'
    'r'
    'dv /V /t'
    '.frame /r 0n12'
    'r'
    'dv /V /t'
    '?? this'
    '?? &this->m_PciResources'
    'dt viogpuwddm!VioGpuAdapter this'
    'dt viogpuwddm!CPciResources &this->m_PciResources'
    'dt viogpuwddm!CPciBar &this->m_PciResources.m_Bars'
    '?? this->m_PciResources.m_HostVisibleBar'
    '?? this->m_PciResources.m_HostVisibleOffset'
    '?? this->m_PciResources.m_HostVisibleSize'
    '?? this->m_PciResources.m_HostVisibleMappedVA'
    '?? this->m_PciResources.m_HostVisibleMappedOffset'
    '?? this->m_PciResources.m_HostVisibleMappedSize'
    'q'
) -join '; '

$output = & $debugger -z $DumpPath -y $privateSymbols -i $imagePath -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $exitCode
