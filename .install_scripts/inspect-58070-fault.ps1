[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\Minidump\082526-4437-02.dmp',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58070-signed',
    [string]$OutputPath = 'C:\Users\Administrator\kmt-58070-fault.txt'
)
$ErrorActionPreference = 'Stop'
$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$privateSymbols = Join-Path $PackageRoot 'drivers\viogpu'
$commands = '.reload /f viogpuwddm.sys; .frame /r 9; r; ub viogpuwddm!VioGpuResolveNativeControlWindow+0x48; u viogpuwddm!VioGpuResolveNativeControlWindow+0x40 L20; dv /t; q'
$output = & $debugger -z $DumpPath -y $privateSymbols -i $privateSymbols -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $exitCode
