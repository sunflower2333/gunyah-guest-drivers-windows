param([string]$RegistryPath)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function ConvertFrom-NativeActivationTrace {
    param([byte[]]$Bytes, [uint32]$Epoch)
    if ($Bytes.Length -lt 24 -or $Bytes.Length -gt (24 + 64 * 48)) {
        throw 'Invalid NativeActivationTrace extent'
    }
    $header = @(for ($i = 0; $i -lt 6; $i++) { [BitConverter]::ToUInt32($Bytes, $i * 4) })
    if ($header[0] -ne 0x54434156 -or $header[1] -ne 1 -or $header[2] -ne $Epoch -or
        $Epoch -eq 0 -or $header[3] -gt 64 -or $header[5] -gt 1 -or
        $Bytes.Length -ne (24 + $header[3] * 48)) {
        throw 'Invalid or stale NativeActivationTrace header'
    }
    $entries = @(for ($i = 0; $i -lt $header[3]; $i++) {
        $offset = 24 + $i * 48
        $words = @(for ($j = 0; $j -lt 12; $j++) { [BitConverter]::ToUInt32($Bytes, $offset + $j * 4) })
        $entry = [ordered]@{
            CompletionIndex = $i
            Type = $words[0]
            Status = ('0x{0:X8}' -f $words[1])
            InputSize = $words[2]
            OutputSize = $words[3]
            Values = @($words[4..11])
        }
        if ($words[0] -eq 1 -and $words[1] -eq 0) {
            $entry.DriverCaps = [ordered]@{
                WddmVersion = ('0x{0:X4}' -f $words[4])
                SchedulingCaps = ('0x{0:X8}' -f $words[5])
                MemoryManagementCaps = ('0x{0:X8}' -f $words[6])
                Nodes = $words[7]
                GraphicsPreemption = $words[8]
                ComputePreemption = $words[9]
                PerEngineTdr = $words[10]
            }
        }
        [pscustomobject]$entry
    })
    [pscustomobject][ordered]@{
        Version = $header[1]
        Epoch = $header[2]
        Count = $header[3]
        CapacityReached = ($header[3] -eq 64)
        DdiVersion = ('0x{0:X4}' -f $header[4])
        RenderOnly = $header[5]
        Entries = $entries
    }
}

if ($RegistryPath) {
    # Caller supplies the exact active display adapter's driver key. Read-only;
    # no device enumeration, driver activation, registry mutation or restart.
    $key = Get-Item -LiteralPath $RegistryPath
    [uint32]$before = $key.GetValue('NativeActivationEpoch')
    [byte[]]$bytes = $key.GetValue('NativeActivationTrace')
    [uint32]$after = $key.GetValue('NativeActivationEpoch')
    if ($before -ne $after) { throw 'Adapter start changed during trace read' }
    ConvertFrom-NativeActivationTrace -Bytes $bytes -Epoch $after | ConvertTo-Json -Depth 8
}
