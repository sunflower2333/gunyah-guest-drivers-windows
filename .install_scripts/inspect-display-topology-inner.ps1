[CmdletBinding()]
param([string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\evidence-topology')
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\DroidVM\agent-scripts\inspect-display-topology.ps1 *>&1 |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'topology.txt') -Encoding UTF8
