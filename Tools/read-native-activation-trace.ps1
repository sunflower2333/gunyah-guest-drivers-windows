param([string]$RegistryPath)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function ConvertFrom-NativeActivationTrace {
    param([byte[]]$Bytes, [uint32]$Epoch)
    if ($Bytes.Length -ne 4288) { throw 'Invalid activation trace extent (version 2 requires 4288 bytes)' }
    $h = @(for ($i = 0; $i -lt 16; $i++) { [BitConverter]::ToUInt32($Bytes, $i * 4) })
    if ($h[0] -ne 0x54434156 -or $h[1] -ne 2 -or $h[2] -ne $Epoch -or $h[3] -ne 4288 -or
        $Epoch -eq 0 -or ($Epoch -band 1) -ne 0 -or $h[5] -gt 1 -or $h[6] -lt 1 -or $h[6] -gt 6 -or
        $h[12] -gt 64 -or $h[12] -ne [Math]::Min([uint64]64, [uint64]$h[10]) -or
        $h[11] -gt $h[10] -or $h[13] -gt 1) { throw 'Invalid, incomplete or stale activation trace header' }
    function Read-Entry([int]$Offset) {
        $w = @(for ($j = 0; $j -lt 16; $j++) { [BitConverter]::ToUInt32($Bytes, $Offset + $j * 4) })
        if ($w[0] -eq 0) {
            if (@($w | Where-Object { $_ -ne 0 }).Count -ne 0) { throw 'Malformed empty query entry' }
            return $null
        }
        if ($w[0] -gt $h[10] -or $w[5] -lt 1 -or $w[5] -gt 6 -or $w[6] -gt 31) { throw 'Invalid query identity/state' }
        $failed = ($w[2] -band [uint32]2147483648) -ne 0
        if ($failed -and @($w[8..15] | Where-Object { $_ -ne 0 }).Count -ne 0) { throw 'Failed query contains undefined output values' }
        [pscustomobject][ordered]@{
            Sequence = $w[0]; Type = $w[1]; Status = ('0x{0:X8}' -f $w[2]); Failed = $failed
            Kind = $(switch ($w[1]) { 65537 { 'ChildRelations' } 65538 { 'ChildStatus' } 65539 { 'ChildDescriptor' } default { 'QueryAdapterInfo' } })
            InputSize = $w[3]; OutputSize = $w[4]; Phase = $w[5]
            DriverActive = ($w[6] -band 1) -ne 0; HardwareInitialized = ($w[6] -band 2) -ne 0
            ResetRequested = ($w[6] -band 4) -ne 0; HardwareReferenceAcquired = ($w[6] -band 8) -ne 0
            HardwarePresent = ($w[6] -band 16) -ne 0
            SampledReadinessMask = ('0x{0:X8}' -f $w[7]); Values = @($w[8..15])
        }
    }
    $first = Read-Entry 64
    $last = Read-Entry 128
    if ($h[11] -eq 0) {
        if ($null -ne $first -or $null -ne $last) { throw 'Failure entry without failure count' }
    } elseif ($null -eq $first -or $null -eq $last -or -not $first.Failed -or -not $last.Failed -or
              $last.Sequence -lt $first.Sequence) { throw 'Invalid first/last failure identity' }
    $entries = @(for ($i = 0; $i -lt $h[12]; $i++) {
        $e = Read-Entry (192 + $i * 64)
        if ($null -eq $e -or $e.Sequence -ne ($i + 1)) { throw 'Nonsequential bounded trace' }
        $e
    })
    [pscustomobject][ordered]@{
        Version = $h[1]; Epoch = $h[2]; DdiVersion = ('0x{0:X4}' -f $h[4]); RenderOnly = $h[5]
        Phase = $h[6]; StartStage = ('0x{0:X4}' -f $h[7]); StartStatus = ('0x{0:X8}' -f $h[8]); StartDetail = $h[9]
        FirstStartFailureStage = ('0x{0:X4}' -f $h[14]); FirstStartFailureStatus = ('0x{0:X8}' -f $h[15])
        TotalQueries = $h[10]; FailureCount = $h[11]; RetainedCount = $h[12]; CounterOverflow = $h[13] -ne 0
        FirstFailure = $first; LastFailure = $last; Entries = $entries
        Note = 'Completion snapshots; sampled readiness may predate this query. A refused optional query is not automatically the activation cause.'
    }
}

if ($RegistryPath) {
    $key = Get-Item -LiteralPath $RegistryPath
    $before = $key.GetValue('NativeActivationEpoch')
    $writeBefore = $key.GetValue('NativeActivationWriteStatus')
    $bytes = $key.GetValue('NativeActivationTrace')
    $writeAfter = $key.GetValue('NativeActivationWriteStatus')
    $after = $key.GetValue('NativeActivationEpoch')
    if ($null -eq $before -or $null -eq $after -or $before -ne $after -or
        $null -eq $writeBefore -or $null -eq $writeAfter -or $writeBefore -ne 0 -or $writeAfter -ne 0) {
        throw 'Activation trace start changed, or registry recording is unavailable/failed'
    }
    ConvertFrom-NativeActivationTrace -Bytes $bytes -Epoch $after | ConvertTo-Json -Depth 8
}
