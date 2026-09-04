[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\Minidump\082526-4218-01.dmp',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58068',
    [string]$OutputPath = 'C:\Users\Administrator\kmt-58068-owner.txt'
)
$ErrorActionPreference = 'Stop'
$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$commands = '.reload /f viogpuwddm.sys; dt viogpuwddm!VIOGPU_NATIVE_CONTEXT_OWNER ffffbc8157b78cd0; dt viogpuwddm!VIOGPU_NATIVE_CONTEXT_REGISTRATION ffffbc81574ba068; q'
$symbols = 'C:\Users\Administrator\viogpu-58070-signed\drivers\viogpu'
$output = & $debugger -z $DumpPath -y $symbols -i $symbols -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $exitCode
