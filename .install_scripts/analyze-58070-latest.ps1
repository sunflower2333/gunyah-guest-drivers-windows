[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\Minidump\082526-4437-02.dmp',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58070-signed',
    [string]$OutputPath = 'C:\Users\Administrator\kmt-58070-dump-analysis.txt'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$privateSymbols = Join-Path $PackageRoot 'drivers\viogpu'
$symbolCache = 'C:\Users\Administrator\viogpu-analysis-58070\symbols'
$imagePath = "$privateSymbols;C:\Windows\System32;C:\Windows\System32\drivers"
$symbolPath = "$privateSymbols;$symbolCache;srv*$symbolCache*https://msdl.microsoft.com/download/symbols"
$commands = @(
    '.symopt- 0x40'
    '.reload /f'
    '.bugcheck'
    '!analyze -v'
    '!sysinfo machineid'
    '!sysinfo cpuinfo'
    'lmvm viogpuwddm'
    'kP'
    'q'
) -join '; '

$output = & $debugger -z $DumpPath -y $symbolPath -i $imagePath -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $exitCode
