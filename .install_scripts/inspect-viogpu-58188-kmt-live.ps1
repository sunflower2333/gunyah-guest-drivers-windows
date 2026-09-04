[CmdletBinding()]
param(
    [string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\evidence\viogpu-58188-d7fe1b39-kmt-retry900s'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$processNames = @(
    'tu_wddm_kmt_probe_arm64.exe',
    'tracerpt.exe',
    'logman.exe',
    'taskkill.exe'
)
$processes = @(Get-CimInstance Win32_Process | Where-Object {
    $processNames -contains $_.Name
} | Select-Object ProcessId,ParentProcessId,Name,CreationDate,CommandLine)

$files = @()
if (Test-Path -LiteralPath $OutputDirectory -PathType Container) {
    $files = @(Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            RelativePath = $_.FullName.Substring($OutputDirectory.Length).TrimStart('\')
            Length = $_.Length
            LastWriteTime = $_.LastWriteTime.ToString('o')
        }
    })
}

[ordered]@{
    CollectedAt = (Get-Date).ToString('o')
    OutputDirectoryExists = Test-Path -LiteralPath $OutputDirectory -PathType Container
    Processes = $processes
    Files = $files
} | ConvertTo-Json -Depth 8
