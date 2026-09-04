[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-current-58163',
    [string]$OutputPath = 'C:\Users\Administrator\whea-124-local-analysis.txt'
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
    '!analyze -v'
    '.ecxr'
    'r'
    'kP'
    'lmvm viogpuwddm'
    '!sysinfo machineid'
    '!sysinfo cpuinfo'
    'q'
) -join '; '

if (Test-Path -LiteralPath $OutputPath) {
    Remove-Item -LiteralPath $OutputPath -Force
}
$output = & $debugger -z $DumpPath -y $symbolPath -i $imagePath -logo $OutputPath -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Add-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Output "DebuggerExitCode=$exitCode"
Get-Content -LiteralPath $OutputPath
exit $exitCode
