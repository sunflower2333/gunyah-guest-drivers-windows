[CmdletBinding()]
param([Parameter(Mandatory)][string]$DumpPath, [string]$OutputPath = 'C:\Users\Administrator\kmt-dump-analysis.txt', [string]$DebuggerCommand = 'ln fffff8000a89acb0; x viogpuwddm!*Validate*; lmvm viogpuwddm; q')
$ErrorActionPreference = 'Stop'
$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$symbolPath = 'srv*C:\symbols*https://msdl.microsoft.com/download/symbols'
$command = $DebuggerCommand
$output = & $debugger -z $DumpPath -y $symbolPath -c $command 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $exitCode
