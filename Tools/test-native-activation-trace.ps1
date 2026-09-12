Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/read-native-activation-trace.ps1"

function Put([byte[]]$Bytes, [int]$Word, [uint32]$Value) { [BitConverter]::GetBytes($Value).CopyTo($Bytes, $Word * 4) }
function Expect-Reject([byte[]]$Bytes, [uint32]$Epoch, [string]$Name) {
    try { $null = ConvertFrom-NativeActivationTrace -Bytes $Bytes -Epoch $Epoch }
    catch { Write-Host "PASS rejection: $Name"; return }
    throw "Decoder accepted $Name"
}
[byte[]]$empty = [byte[]]::new(4288)
Put $empty 0 0x54434156; Put $empty 1 2; Put $empty 2 4; Put $empty 3 4288; Put $empty 4 0x5023; Put $empty 6 1
$fresh = ConvertFrom-NativeActivationTrace -Bytes $empty -Epoch 4
if ($fresh.RetainedCount -ne 0 -or $null -ne $fresh.FirstFailure) { throw 'Fresh epoch did not decode empty' }
[byte[]]$full = $empty.Clone()
Put $full 6 3; Put $full 10 68; Put $full 11 2; Put $full 12 64
for ($i = 0; $i -lt 64; $i++) { Put $full (48 + $i * 16) ($i + 1); Put $full (53 + $i * 16) 2 }
foreach ($entry in @(@(16,67,2), @(32,68,3))) {
    Put $full $entry[0] $entry[1]; Put $full ($entry[0] + 1) 11
    Put $full ($entry[0] + 2) ([uint32]3221225635); Put $full ($entry[0] + 5) $entry[2]
}
$decoded = ConvertFrom-NativeActivationTrace -Bytes $full -Epoch 4
if ($decoded.FirstFailure.Sequence -ne 67 -or $decoded.LastFailure.Sequence -ne 68 -or
    $decoded.LastFailure.Phase -ne 3 -or $decoded.Entries.Count -ne 64) { throw 'Late failure identities lost' }
Expect-Reject $full 2 'stale start'
[byte[]]$bad = $full.Clone(); Put $bad 2 5; Expect-Reject $bad 5 'uncommitted odd start'
[byte[]]$bad = $full.Clone(); Put $bad 12 65; Expect-Reject $bad 4 'out-of-bounds entry count'
[byte[]]$bad = $full.Clone(); Put $bad 16 69; Expect-Reject $bad 4 'future first failure sequence'
[byte[]]$bad = $full.Clone(); Put $bad 34 0; Expect-Reject $bad 4 'success masquerading as failure'
[byte[]]$bad = $full.Clone(); Put $bad 24 99; Expect-Reject $bad 4 'undefined output from failed query'
[byte[]]$bad = $full.Clone(); Put $bad 48 2; Expect-Reject $bad 4 'unordered query sequence'
Expect-Reject ($full[0..4286]) 4 'truncated snapshot'
Write-Host 'PASS activation decoder: fresh start, bounded success history, late first/last failures, stale/malformed rejection'
