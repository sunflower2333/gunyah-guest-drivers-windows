[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-current-58163',
    [string]$OutputPath = 'C:\Users\Administrator\whea-124-context.txt'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe'
$privateSymbols = Join-Path $PackageRoot 'drivers\viogpu'
$imagePath = "$privateSymbols;C:\Windows\System32;C:\Windows\System32\drivers"
$symbolPath = $privateSymbols
$commands = @(
    '.symopt- 0x40'
    '.bugcheck'
    '!errrec ffffbf810f9e2608'
    '!analyze -v'
    '.frame /r 9'
    'r'
    'dv /V /t'
    'uf viogpuwddm!VioGpuResolveNativeControlWindow'
    '.frame /r 10'
    'dv /V /t'
    '.frame /r 11'
    'dv /V /t'
    'dt viogpuwddm!VIOGPU_NATIVE_CONTEXT_OWNER ffffbf8112011d90'
    'dq ffffbf8112011d90 L30'
    'q'
) -join '; '
if (Test-Path -LiteralPath $OutputPath) { Remove-Item -LiteralPath $OutputPath -Force }
$output = & $debugger -z $DumpPath -y $symbolPath -i $imagePath -logo $OutputPath -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Add-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Output "DebuggerExitCode=$exitCode"
Get-Content -LiteralPath $OutputPath
exit $exitCode
