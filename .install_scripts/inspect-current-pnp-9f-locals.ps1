[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-build-33130811282',
    [string]$OutputPath = 'C:\Users\Administrator\viogpu-58165\pnp-9f-locals.txt'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$privateSymbols = Join-Path $PackageRoot 'drivers\viogpu'
$symbolCache = 'C:\Users\Administrator\viogpu-symbols'
$imagePath = "$privateSymbols;C:\Windows\System32;C:\Windows\System32\drivers"
$symbolPath = "$privateSymbols;srv*$symbolCache*https://msdl.microsoft.com/download/symbols"

foreach ($path in @($debugger, $DumpPath, (Join-Path $privateSymbols 'viogpuwddm.pdb'))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required analysis input is missing: $path"
    }
}
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
New-Item -ItemType Directory -Path $symbolCache -Force | Out-Null

$commands = @(
    '.symopt- 0x40'
    '.reload /f nt'
    '.reload /f viogpuwddm.sys'
    '.thread /r /p ffffd68585805080'
    '.frame /r 0xf'
    'dv /t /v'
    '.frame /r 0x10'
    'dv /t /v'
    '.frame /r 0x11'
    'dv /t /v'
    '.frame /r 0x12'
    'dv /t /v'
    'q'
) -join '; '

$output = & $debugger -z $DumpPath -y $symbolPath -i $imagePath -c $commands 2>&1
$debuggerExitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $debuggerExitCode
