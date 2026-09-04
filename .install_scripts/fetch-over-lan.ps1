[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Server,
    [Parameter(Mandatory=$true)][int]$Port,
    [Parameter(Mandatory=$true)][string]$Destination,
    [int]$TimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'

# The SSH/socat tunnel to this guest stalls on sustained transfers, but the
# guest and the Android host share a LAN.  Pull the bytes directly from a
# socat file server there instead of pushing them through the tunnel.
$client = New-Object System.Net.Sockets.TcpClient
$client.ReceiveTimeout = $TimeoutSeconds * 1000
try {
    $client.Connect($Server, $Port)
    $stream = $client.GetStream()
    $out = [System.IO.File]::Create($Destination)
    try {
        $buffer = New-Object byte[] 65536
        $total = 0
        while ($true) {
            $read = $stream.Read($buffer, 0, $buffer.Length)
            if ($read -le 0) { break }
            $out.Write($buffer, 0, $read)
            $total += $read
        }
    } finally { $out.Dispose() }
} finally { $client.Dispose() }

$item = Get-Item -LiteralPath $Destination
$hash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLower()
Write-Output "BYTES=$($item.Length)"
Write-Output "SHA256=$hash"
