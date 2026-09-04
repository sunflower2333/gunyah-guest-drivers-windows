[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\Minidump\082526-4437-02.dmp',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58070-signed',
    [string]$OutputPath = 'C:\Users\Administrator\kmt-58070-owner.txt'
)
$ErrorActionPreference = 'Stop'
$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$privateSymbols = Join-Path $PackageRoot 'drivers\viogpu'
$commands = '.reload /f viogpuwddm.sys; dt viogpuwddm!CPciResources ffffcb818397c9d8; dt viogpuwddm!CPciBar ffffcb818397ca18; dt viogpuwddm!CPciBar ffffcb818397ca50; dt viogpuwddm!CPciBar ffffcb818397ca88; dt viogpuwddm!CPciBar ffffcb818397cac0; dt viogpuwddm!CPciBar ffffcb818397caf8; q'
$output = & $debugger -z $DumpPath -y $privateSymbols -i $privateSymbols -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $exitCode
