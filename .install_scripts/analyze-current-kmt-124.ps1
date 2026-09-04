[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\Minidump\082526-4000-01.dmp',
    [string]$OutputPath = 'C:\Users\Administrator\kmt-124-analysis.txt',
    [string]$ErrorRecordAddress = 'ffffcf830f0ed298'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$symbolCache = 'C:\Users\Administrator\viogpu-analysis-58066\symbols'
$privateSymbols = 'C:\Users\Administrator\viogpu-58068'
$imagePath = "$privateSymbols;C:\Windows\System32;C:\Windows\System32\drivers"
$symbolPath = "$privateSymbols;$symbolCache"
$commands = @(
    '.symopt- 0x40'
    '.reload /f'
    '.bugcheck'
    '!analyze -v'
    "!errpkt $ErrorRecordAddress"
    '!sysinfo machineid'
    '!sysinfo cpuinfo'
    'lmvm viogpuwddm'
    'q'
) -join '; '

$output = & $debugger -z $DumpPath -y $symbolPath -i $imagePath -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $exitCode
