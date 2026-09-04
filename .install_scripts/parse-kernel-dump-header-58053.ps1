[CmdletBinding()]
param(
    [string]$DumpPath = 'C:\Windows\MEMORY.DMP'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-U32([byte[]]$Bytes, [int]$Offset) {
    [BitConverter]::ToUInt32($Bytes, $Offset)
}

function Read-U64([byte[]]$Bytes, [int]$Offset) {
    [BitConverter]::ToUInt64($Bytes, $Offset)
}

$stream = [IO.File]::Open($DumpPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
try {
    $header = New-Object byte[] 0x1000
    $read = $stream.Read($header, 0, $header.Length)
    if ($read -lt 0x80) {
        throw "Dump header is only $read bytes."
    }
} finally {
    $stream.Dispose()
}

$signature = [Text.Encoding]::ASCII.GetString($header, 0, 4)
$validDump = [Text.Encoding]::ASCII.GetString($header, 4, 4)
$machine = Read-U32 $header 0x30
$processors = Read-U32 $header 0x34
$bugCheck = Read-U32 $header 0x38
$p1 = Read-U64 $header 0x40
$p2 = Read-U64 $header 0x48
$p3 = Read-U64 $header 0x50
$p4 = Read-U64 $header 0x58
$dtb = Read-U64 $header 0x10
$loadedModules = Read-U64 $header 0x20
$activeProcesses = Read-U64 $header 0x28

[pscustomobject]@{
    DumpPath = $DumpPath
    Signature = $signature
    ValidDump = $validDump
    MachineImageType = ('0x{0:X8}' -f $machine)
    NumberProcessors = $processors
    BugCheckCode = ('0x{0:X8}' -f $bugCheck)
    BugCheckParameter1 = ('0x{0:X16}' -f $p1)
    BugCheckParameter2 = ('0x{0:X16}' -f $p2)
    BugCheckParameter3 = ('0x{0:X16}' -f $p3)
    BugCheckParameter4 = ('0x{0:X16}' -f $p4)
    DirectoryTableBase = ('0x{0:X16}' -f $dtb)
    PsLoadedModuleList = ('0x{0:X16}' -f $loadedModules)
    PsActiveProcessHead = ('0x{0:X16}' -f $activeProcesses)
    FileLength = (Get-Item -LiteralPath $DumpPath).Length
} | Format-List
