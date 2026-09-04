[CmdletBinding()]
param(
    [int]$MaxEventsPerId = 8
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$eventIds = @(41, 1001, 1074, 6005, 6006, 6008)
$events = foreach ($eventId in $eventIds) {
    @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = $eventId } `
            -MaxEvents $MaxEventsPerId -ErrorAction SilentlyContinue) |
        ForEach-Object {
            [pscustomobject]@{
                TimeCreated = $_.TimeCreated.ToString('o')
                Id = $_.Id
                ProviderName = $_.ProviderName
                Level = $_.LevelDisplayName
                Message = $_.Message
            }
        }
}

$dumpPaths = @(
    'C:\Windows\MEMORY.DMP'
    'C:\Windows\Minidump\082726-8500-01.dmp'
)
$dumps = foreach ($dumpPath in $dumpPaths) {
    $item = Get-Item -LiteralPath $dumpPath -ErrorAction SilentlyContinue
    if ($null -ne $item) {
        [pscustomobject]@{
            Path = $item.FullName
            Length = $item.Length
            LastWriteTime = $item.LastWriteTime.ToString('o')
        }
    }
}

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    Events = @($events | Sort-Object TimeCreated -Descending)
    Dumps = @($dumps)
} | ConvertTo-Json -Depth 6
