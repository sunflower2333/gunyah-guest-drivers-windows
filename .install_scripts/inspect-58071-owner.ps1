[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\Minidump\082526-4250-01.dmp',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-58071-signed',
    [string]$OutputPath = 'C:\Users\Administrator\kmt-58071-owner.txt'
)
$ErrorActionPreference = 'Stop'
$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$symbols = Join-Path $PackageRoot 'drivers\viogpu'
$commands = '.reload /f viogpuwddm.sys; dt viogpuwddm!VIOGPU_NATIVE_CONTEXT_OWNER ffffd80eb8730610; dt viogpuwddm!VIOGPU_NATIVE_CONTEXT_REGISTRATION ffffd80ebab6e068; q'
$output = & $debugger -z $DumpPath -y $symbols -i $symbols -c $commands 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $exitCode
