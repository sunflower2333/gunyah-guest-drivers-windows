[CmdletBinding()]
param([string]$Root = 'C:\Windows\Minidump')
$ErrorActionPreference = 'Stop'

function Read-ModuleList([System.IO.BinaryReader]$br, [UInt32]$rva) {
    $br.BaseStream.Position = $rva
    $count = $br.ReadUInt32()
    $items = @()
    for ($i = 0; $i -lt $count; $i++) {
        $base = $br.ReadUInt64()
        $size = $br.ReadUInt32()
        [void]$br.ReadUInt32()
        [void]$br.ReadUInt32()
        $nameRva = $br.ReadUInt32()
        $br.BaseStream.Position += 52
        $br.BaseStream.Position += 8
        $br.BaseStream.Position += 8
        $br.BaseStream.Position += 8
        $br.BaseStream.Position += 8
        $saved = $br.BaseStream.Position
        $br.BaseStream.Position = $nameRva
        $nameBytes = $br.ReadUInt32()
        $name = [Text.Encoding]::Unicode.GetString($br.ReadBytes($nameBytes))
        $br.BaseStream.Position = $saved
        $items += [pscustomobject]@{ Name=$name; Base=('0x{0:X16}' -f $base); Size=('0x{0:X}' -f $size); BaseValue=$base; SizeValue=$size }
    }
    return $items
}

foreach ($file in Get-ChildItem -LiteralPath $Root -Filter '*.dmp' -File | Sort-Object LastWriteTime -Descending | Select-Object -First 8) {
    Write-Output "reading $($file.FullName)"
    $fs = [IO.File]::OpenRead($file.FullName)
    $br = [IO.BinaryReader]::new($fs)
    try {
        [void]$br.ReadUInt32()
        [void]$br.ReadUInt32()
        $streamCount = $br.ReadUInt32()
        $dirRva = $br.ReadUInt32()
        Write-Output "header streams=$streamCount dir=$dirRva"
        $dirs = @()
        $br.BaseStream.Position = $dirRva
        for ($i=0; $i -lt $streamCount; $i++) {
            $dirs += [pscustomobject]@{ Type=$br.ReadUInt32(); Size=$br.ReadUInt32(); Rva=$br.ReadUInt32() }
        }
        $modDir = $dirs | Where-Object Type -eq 4 | Select-Object -First 1
        if ($null -eq $modDir) { Write-Output "no module stream types=$((($dirs | Select-Object -ExpandProperty Type) -join ','))"; continue }
        $mods = Read-ModuleList $br $modDir.Rva
        Write-Output "===== $($file.Name) ($($file.LastWriteTime.ToString('o'))) ====="
        $mods | Where-Object { $_.Name -match 'viogpu|dxg|win32k|ntoskrnl' } | Format-Table Name,Base,Size -AutoSize
        foreach ($m in $mods) {
            if ($m.Name -match 'viogpu') { Write-Output "viogpu base=$($m.Base) size=$($m.Size)" }
        }
    } finally { $br.Dispose(); $fs.Dispose() }
}
