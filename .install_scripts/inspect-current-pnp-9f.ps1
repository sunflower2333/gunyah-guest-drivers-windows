[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-build-33130811282',
    [string]$OutputPath = 'C:\Users\Administrator\viogpu-58165\pnp-9f-targeted.txt'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$privateSymbols = Join-Path $PackageRoot 'drivers\viogpu'
$symbolCache = 'C:\Users\Administrator\viogpu-symbols'
$imagePath = "$privateSymbols;C:\Windows\System32;C:\Windows\System32\drivers"
$symbolPath = "$privateSymbols;srv*$symbolCache*https://msdl.microsoft.com/download/symbols"

$commands = @(
    '.symopt- 0x40'
    '.reload /f nt'
    '.reload /f viogpuwddm.sys'
    'lmvm viogpuwddm'
    '!lmi viogpuwddm'
    '!irp ffffd685857f6ae0 1'
    '!devstack ffffd685872c3180'
    '!devobj ffffd685872c3180'
    '!object ffffd685836dfc10'
    'dt nt!_KEVENT ffffd685836dfc10'
    '!stacks 2 viogpuwddm'
    '!stacks 2 dxgkrnl'
    '!process 0 7 tu_wddm_kmt_probe_arm64.exe'
    '!thread ffffd685871bc500 1f'
    'q'
) -join '; '

$output = & $debugger -z $DumpPath -y $symbolPath -i $imagePath -c $commands 2>&1
$debuggerExitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $debuggerExitCode
