[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\Minidump\082526-4437-02.dmp',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58070-signed',
    [string]$SymbolCache = 'C:\Users\Administrator\viogpu-analysis-58066\symbols',
    [string]$OutputPath = 'C:\Users\Administrator\kmt-58070-cached-analysis.txt'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$privateSymbols = Join-Path $PackageRoot 'drivers\viogpu'
if (-not (Test-Path -LiteralPath $privateSymbols -PathType Container)) {
    $privateSymbols = $PackageRoot
}
$symbolPath = "$privateSymbols;$SymbolCache"
$imagePath = "$privateSymbols;C:\Windows\System32;C:\Windows\System32\drivers"
$commands = @(
    '.symopt- 0x40'
    '.reload /f'
    '.bugcheck'
    '!analyze -v'
    '!errrec ffffcb8185393038'
    'lmvm viogpuwddm'
    'kP'
    'q'
) -join '; '
$output = & $debugger -z $DumpPath -y $symbolPath -i $imagePath -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $exitCode
