[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PackageRoot = 'C:\Users\Administrator\droidvm-test\kmd-32980523436',
    [string]$OutputPath = 'C:\Users\Administrator\droidvm-test\kmt-58110-private-analysis.txt',
    [string]$ControlAddress = '0'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$privateSymbols = Join-Path $PackageRoot 'viogpu'
$imagePath = "$privateSymbols;C:\Windows\System32;C:\Windows\System32\drivers"
$symbolPath = $privateSymbols

$commands = @(
    '.symopt- 0x40'
    '.reload /f viogpuwddm.sys'
    '.bugcheck'
    '!analyze -v'
    '.ecxr'
    'r'
    '.frame 9'
    'r'
    'dv /V /t'
    'dt viogpuwddm!VIOGPU_NATIVE_CONTEXT_OWNER ffffdb853c963e10'
    'dq ffffdb853c963e10 L20'
    '.frame /r 0n11'
    'dv /V /t'
    'r'
    '.frame /r 0n12'
    'dv /V /t'
    'r'
    'dt viogpuwddm!VioGpuAdapter 0xffffdb853df0d770'
    'dt viogpuwddm!CPciResources 0xffffdb853df0d9c8'
    'dt viogpuwddm!CPciBar 0xffffdb853df0da08'
    'dt viogpuwddm!CPciBar 0xffffdb853df0da28'
    'dt viogpuwddm!CPciBar 0xffffdb853df0da48'
    'dt viogpuwddm!CPciBar 0xffffdb853df0da68'
    'dt viogpuwddm!CPciBar 0xffffdb853df0da88'
    'dt viogpuwddm!CPciBar 0xffffdb853df0daa8'
    '!pte ffffd77dc8000000'
    '!address ffffd77dc8000000'
    'lmvm viogpuwddm'
    'ln viogpuwddm!VioGpuResolveNativeControlWindow+0x48'
    'u viogpuwddm!VioGpuResolveNativeControlWindow L40'
    'u viogpuwddm!VioGpuSeedNativeControlResponse L50'
    'kP'
    $(if ($ControlAddress -ne '0') { "dq $ControlAddress L10" })
    'q'
) | Where-Object { $_ -is [string] -and $_.Length -gt 0 }

$output = & $debugger -z $DumpPath -y $symbolPath -i $imagePath -c ($commands -join '; ') 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $exitCode
