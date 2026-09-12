$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/accounting-deltas.ps1"
function Assert-That($Condition, $Message) { if (-not $Condition) { throw $Message } }
$a = [pscustomobject]@{ RunningTime = '100'; Timestamp_Sys100NS = '1000' }
$b = [pscustomobject]@{ RunningTime = '150'; Timestamp_Sys100NS = '1200' }
$r = Get-EngineAccountingDelta $a $b
Assert-That ($r.state -eq 'valid' -and $r.percent -eq 25) 'Known 50/200 interval must be 25 percent'
$r = Get-EngineAccountingDelta $b $a
Assert-That ($r.state -eq 'invalid_time' -and $null -eq $r.percent) 'Reversed timestamps must not be usage'
$r = Get-EngineAccountingDelta $null $b
Assert-That ($r.state -eq 'missing_sample' -and $null -eq $r.percent) 'Missing sample must remain unknown'
$b.RunningTime = '90'
$r = Get-EngineAccountingDelta $a $b
Assert-That ($r.state -eq 'counter_reset' -and $null -eq $r.percent) 'Counter reset must not wrap'
$b.RunningTime = '400'
$r = Get-EngineAccountingDelta $a $b
Assert-That ($r.state -eq 'outside_range' -and $r.percent -eq 150) 'Out-of-range usage must not be clamped'
$b.RunningTime = '100'
$r = Get-EngineAccountingDelta $a $b
Assert-That ($r.state -eq 'valid' -and $r.percent -eq 0) 'Idle interval must retain a valid zero'
$r = Get-EngineAccountingDelta $a ([pscustomobject]@{ Timestamp_Sys100NS = '1200' })
Assert-That ($r.state -eq 'missing_sample') 'Missing field must not coerce to zero'
$m = [pscustomobject]@{ DedicatedUsage = '0'; SharedUsage = '1024'; TotalCommitted = '2048' }
$n = [pscustomobject]@{ DedicatedUsage = '0'; SharedUsage = '256'; TotalCommitted = '512' }
$r = Get-MemoryAccountingDelta $m $n
Assert-That ($r.state -eq 'valid' -and $r.shared_delta -eq -768 -and $r.committed_delta -eq -1536) 'Releases require signed deltas'
$r = Get-MemoryAccountingDelta $null $n
Assert-That ($r.state -eq 'missing_sample' -and $null -eq $r.shared_delta) 'Missing memory baseline is unknown'
$r = Get-MemoryAccountingDelta $m ([pscustomobject]@{ SharedUsage = '256' })
Assert-That ($r.state -eq 'partial' -and $r.shared_delta -eq -768 -and $null -eq $r.committed_delta) 'Partial memory data retains unknown fields'
Write-Host 'PASS: 10 accounting delta cases'
