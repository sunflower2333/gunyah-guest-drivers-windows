[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\Minidump\082526-4437-02.dmp',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58070-signed',
    [string]$OutputPath = 'C:\Users\Administrator\kmt-58070-v-analysis.txt'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$privateSymbols = Join-Path $PackageRoot 'drivers\viogpu'
if (-not (Test-Path -LiteralPath $privateSymbols -PathType Container)) {
    $privateSymbols = $PackageRoot
}
$commands = '.symopt- 0x40; .reload /f viogpuwddm.sys; .bugcheck; !analyze -v; lmvm viogpuwddm; kv; q'
$output = & $debugger -z $DumpPath -y $privateSymbols -i $privateSymbols -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $exitCode
