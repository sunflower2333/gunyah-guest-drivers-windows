function Get-EngineAccountingDelta {
    param($Previous, $Current)
    $result = [ordered]@{ state = 'missing_sample'; elapsed_100ns = $null; running_100ns = $null; percent = $null }
    if ($null -eq $Previous -or $null -eq $Current) { return [pscustomobject]$result }
    foreach ($field in @('RunningTime', 'Timestamp_Sys100NS')) {
        if ($null -eq $Previous.$field -or $null -eq $Current.$field) { return [pscustomobject]$result }
    }
    $elapsed = [decimal]$Current.Timestamp_Sys100NS - [decimal]$Previous.Timestamp_Sys100NS
    $running = [decimal]$Current.RunningTime - [decimal]$Previous.RunningTime
    $result.elapsed_100ns = $elapsed
    $result.running_100ns = $running
    if ($elapsed -le 0) { $result.state = 'invalid_time'; return [pscustomobject]$result }
    if ($running -lt 0) { $result.state = 'counter_reset'; return [pscustomobject]$result }
    $result.percent = 100 * $running / $elapsed
    $result.state = if ($result.percent -gt 100) { 'outside_range' } else { 'valid' }
    return [pscustomobject]$result
}

function Get-MemoryAccountingDelta {
    param($Previous, $Current)
    $result = [ordered]@{ state = 'missing_sample'; dedicated_delta = $null; shared_delta = $null; committed_delta = $null }
    if ($null -eq $Previous -or $null -eq $Current) { return [pscustomobject]$result }
    $fields = @{ DedicatedUsage = 'dedicated_delta'; SharedUsage = 'shared_delta'; TotalCommitted = 'committed_delta' }
    $complete = $true
    foreach ($field in $fields.Keys) {
        if ($null -eq $Previous.$field -or $null -eq $Current.$field) { $complete = $false; continue }
        $result[$fields[$field]] = [decimal]$Current.$field - [decimal]$Previous.$field
    }
    $result.state = if ($complete) { 'valid' } else { 'partial' }
    return [pscustomobject]$result
}
