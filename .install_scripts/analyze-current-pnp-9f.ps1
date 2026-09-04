[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP',
    [string]$PackageRoot = 'C:\Users\Administrator\viogpu-build-33135644735',
    [string]$OutputPath = 'C:\Users\Administrator\viogpu-58165\pnp-9f-analysis.txt'
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
    '.reload /f'
    '.bugcheck'
    '!analyze -v'
    '!thread ffffd685871bc500 1f'
    'kv'
    '!locks'
    'dt nt!_TRIAGE_9F_PNP fffff88dea2a1910'
    'dq fffff88dea2a1910 L10'
    '!blackboxpnp'
    '!pnptriage'
    '!poaction'
    'lmvm viogpuwddm'
    'q'
) -join '; '

$output = & $debugger -z $DumpPath -y $symbolPath -i $imagePath -c $commands 2>&1
$debuggerExitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Get-Content -LiteralPath $OutputPath
exit $debuggerExitCode
