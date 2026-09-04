[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-current-58163',
    [string]$OutputPath = 'C:\Users\Administrator\sea-58163-analysis.txt'
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
    '.ecxr'
    'r'
    'kv'
    'dq ffffbf810f9e2608 L20'
    'db ffffbf810f9e2608 L100'
    'dq fffffe0fddd03c00 L40'
    'uf viogpuwddm!VioGpuResolveNativeControlWindow'
    'dt viogpuwddm!VIOGPU_NATIVE_CONTEXT_OWNER ffffbf8112011d90'
    'dt viogpuwddm!VIOGPU_NATIVE_CONTEXT_REGISTRATION ffffbf810fae7068'
    'dt viogpuwddm!VioGpuAdapter ffffbf8107fbf000'
    'q'
) -join '; '

if (Test-Path -LiteralPath $OutputPath -PathType Leaf) {
    Remove-Item -LiteralPath $OutputPath -Force
}
$output = & $debugger -z $DumpPath -y $symbolPath -i $imagePath -logo $OutputPath -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Add-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Output "DebuggerExitCode=$exitCode"
Get-Content -LiteralPath $OutputPath
exit $exitCode
