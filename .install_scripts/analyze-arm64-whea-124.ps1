[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-current-58163',
    [string]$OutputPath = 'C:\Users\Administrator\whea-124-analysis.txt'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\kd.exe'
$privateSymbols = Join-Path $PackageRoot 'drivers\viogpu'
$symbolCache = 'C:\DroidVM\symbols'
$imagePath = "$privateSymbols;C:\Windows\System32;C:\Windows\System32\drivers"
$symbolPath = "$privateSymbols;srv*$symbolCache*https://msdl.microsoft.com/download/symbols"

foreach ($path in @($debugger, $DumpPath, (Join-Path $privateSymbols 'viogpuwddm.pdb'), (Join-Path $privateSymbols 'viogpuwddm.sys'))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing debugger input: $path"
    }
}
New-Item -ItemType Directory -Path $symbolCache -Force | Out-Null

$commands = @(
    '.symopt- 0x40'
    '.reload /f'
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

$output = & $debugger -z $DumpPath -y $symbolPath -i $imagePath -logo $OutputPath -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Add-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Output "DebuggerExitCode=$exitCode"
Get-Content -LiteralPath $OutputPath
exit $exitCode
