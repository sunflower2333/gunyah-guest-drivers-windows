[CmdletBinding()]
param(
    [string]$CsvPath = 'C:\DroidVM\viogpu-58036\runtime-logs\viogpu-display-restart-20260824-064538022.csv'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$stream = [System.IO.File]::OpenRead($CsvPath)
try {
    $buffer = New-Object byte[] 256
    $count = $stream.Read($buffer, 0, $buffer.Length)
}
finally {
    $stream.Dispose()
}

$bytes = if ($count -eq $buffer.Length) { $buffer } else { $buffer[0..($count - 1)] }
$encodings = [ordered]@{
    Utf8 = [System.Text.Encoding]::UTF8
    Utf16Le = [System.Text.Encoding]::Unicode
    Utf16Be = [System.Text.Encoding]::BigEndianUnicode
    Utf32Le = [System.Text.Encoding]::UTF32
    Ascii = [System.Text.Encoding]::ASCII
}
$decoded = [ordered]@{}
foreach ($entry in $encodings.GetEnumerator()) {
    $decoded[$entry.Key] = $entry.Value.GetString($bytes).Replace("`0", '<NUL>').Replace("`r", '<CR>').Replace("`n", '<LF>')
}

[pscustomobject]@{
    Path = $CsvPath
    Length = (Get-Item -LiteralPath $CsvPath).Length
    PrefixHex = (($bytes | ForEach-Object { $_.ToString('X2') }) -join ' ')
    Decoded = $decoded
} | ConvertTo-Json -Depth 4
