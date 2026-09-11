Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/read-native-activation-trace.ps1"

function New-TraceBytes([uint32]$Count) {
    $bytes = [byte[]]::new(24 + $Count * 48)
    [uint32[]]$header = @(0x54434156, 1, 7, $Count, 0x8001, 0)
    for ($i = 0; $i -lt 6; $i++) { [BitConverter]::GetBytes($header[$i]).CopyTo($bytes, $i * 4) }
    return ,$bytes
}
function Expect-Rejection([scriptblock]$Body) {
    $failed = $false
    try { & $Body | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw 'Malformed or stale trace accepted' }
}

$empty = ConvertFrom-NativeActivationTrace -Bytes (New-TraceBytes 0) -Epoch 7
if ($empty.Count -ne 0 -or $empty.Epoch -ne 7) { throw 'Empty start trace did not roundtrip' }
$bytes = New-TraceBytes 2
[BitConverter]::GetBytes([uint32]1).CopyTo($bytes, 24)
[BitConverter]::GetBytes([uint32]0x2300).CopyTo($bytes, 40)
[BitConverter]::GetBytes([uint32]1).CopyTo($bytes, 52)
[BitConverter]::GetBytes([Convert]::ToUInt32('C00000A3', 16)).CopyTo($bytes, 76)
$trace = ConvertFrom-NativeActivationTrace -Bytes $bytes -Epoch 7
if ($trace.Entries[0].DriverCaps.WddmVersion -ne '0x2300' -or
    $trace.Entries[0].DriverCaps.Nodes -ne 1 -or $trace.Entries[1].Status -ne '0xC00000A3') {
    throw 'Query completion order, caps, or failure status decoding mismatch'
}
Expect-Rejection { ConvertFrom-NativeActivationTrace -Bytes $bytes -Epoch 8 }
Expect-Rejection { ConvertFrom-NativeActivationTrace -Bytes $bytes[0..($bytes.Length - 2)] -Epoch 7 }
Expect-Rejection { ConvertFrom-NativeActivationTrace -Bytes (New-TraceBytes 65) -Epoch 7 }
$full = ConvertFrom-NativeActivationTrace -Bytes (New-TraceBytes 64) -Epoch 7
if (-not $full.CapacityReached -or $full.Entries.Count -ne 64) { throw 'Bounded capacity decoding mismatch' }
Write-Host 'PASS: exact-start activation trace decoding, order, stale/truncated/overflow rejection'
