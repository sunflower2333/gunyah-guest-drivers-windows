[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-current-58163',
    [string]$OutputPath = 'C:\Users\Administrator\whea-124-adapter.txt'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe'
$privateSymbols = Join-Path $PackageRoot 'drivers\viogpu'
$imagePath = "$privateSymbols;C:\Windows\System32;C:\Windows\System32\drivers"
$commands = @(
    '.symopt- 0x40'
    '.frame /r 12'
    'dv /V /t'
    'r'
    'dt viogpuwddm!VioGpuAdapter @x0'
    'dt viogpuwddm!VioGpuAdapter ffffbf810c92a7d0'
    'dt viogpuwddm!CPciResources ffffbf810c92aa28'
    'dt viogpuwddm!CPciBar ffffbf810c92aa68'
    'dt viogpuwddm!CPciBar ffffbf810c92aa88'
    'dt viogpuwddm!CPciBar ffffbf810c92aaa8'
    'dt viogpuwddm!CPciBar ffffbf810c92aac8'
    'dt viogpuwddm!CPciBar ffffbf810c92aae8'
    'dt viogpuwddm!CPciBar ffffbf810c92ab08'
    'dq ffffbf810c92aa28 L70'
    '.frame /r 13'
    'dv /V /t'
    'r'
    'q'
) -join '; '
if (Test-Path -LiteralPath $OutputPath) { Remove-Item -LiteralPath $OutputPath -Force }
$output = & $debugger -z $DumpPath -y $privateSymbols -i $imagePath -logo $OutputPath -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Add-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Output "DebuggerExitCode=$exitCode"
Get-Content -LiteralPath $OutputPath
exit $exitCode
