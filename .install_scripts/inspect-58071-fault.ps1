[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\Minidump\082526-4250-01.dmp',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58071-signed',
    [string]$OutputPath = 'C:\Users\Administrator\kmt-58071-fault.txt'
)
$ErrorActionPreference = 'Stop'
$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$symbols = Join-Path $PackageRoot 'drivers\viogpu'
$commands = @(
    '.symopt- 0x40'
    '.reload /f viogpuwddm.sys'
    '.bugcheck'
    '!analyze -v'
    '.frame /r 9'
    'r'
    'ub viogpuwddm!VioGpuResolveNativeControlWindow+0x48'
    'u viogpuwddm!VioGpuResolveNativeControlWindow+0x40 L20'
    'dv /t'
    'q'
) -join '; '
$output = & $debugger -z $DumpPath -y $symbols -i $symbols -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $exitCode
