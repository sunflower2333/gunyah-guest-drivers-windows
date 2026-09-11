Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$VendorKey = 'SOFTWARE\Khronos\OpenCL\Vendors'
function Get-OpenClEntries([string]$Payload) {
    @('arm64','x64','x86') | ForEach-Object {
        [pscustomobject]@{ View = $(if ($_ -eq 'x86') {'Registry32'} else {'Registry64'})
            Name = (Join-Path $Payload "$_\viogpucl.dll") }
    }
}
function Get-OpenClSnapshot($Entries, [string]$Hive = 'LocalMachine') {
    foreach ($entry in $Entries) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]$Hive, [Microsoft.Win32.RegistryView]$entry.View)
        $key = $base.OpenSubKey($VendorKey)
        try {
            $present = $null -ne $key -and @($key.GetValueNames()) -contains $entry.Name
            $kind = $null; $value = $null
            if ($present) { $kind = $key.GetValueKind($entry.Name).ToString(); $value = $key.GetValue($entry.Name) }
            [pscustomobject]@{View=$entry.View; Name=$entry.Name; Present=$present; Kind=$kind; Value=$value}
        } finally { if ($key) {$key.Dispose()}; $base.Dispose() }
    }
}
function Set-OpenClSnapshot($Snapshot, [string]$Hive = 'LocalMachine') {
    foreach ($entry in $Snapshot) {
        if ($entry.View -notin @('Registry64','Registry32') -or ![IO.Path]::IsPathRooted($entry.Name) -or
            [IO.Path]::GetFileName($entry.Name) -ne 'viogpucl.dll') { throw 'Invalid vendor registration target' }
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]$Hive, [Microsoft.Win32.RegistryView]$entry.View)
        $key = $base.CreateSubKey($VendorKey)
        try {
            if ($entry.Present) {
                if ($entry.Kind -ne 'DWord') { throw 'OpenCL vendor registration must be DWORD' }
                $key.SetValue($entry.Name, [int]$entry.Value, [Microsoft.Win32.RegistryValueKind]::DWord)
            } else { $key.DeleteValue($entry.Name, $false) }
            $key.Flush()
        } finally { $key.Dispose(); $base.Dispose() }
    }
}
function Test-OpenClSnapshot($Snapshot, [string]$Hive = 'LocalMachine') {
    $current = @(Get-OpenClSnapshot $Snapshot $Hive)
    return (ConvertTo-Json -InputObject @($current) -Compress) -ceq (ConvertTo-Json -InputObject @($Snapshot) -Compress)
}
Export-ModuleMember -Function Get-OpenClEntries,Get-OpenClSnapshot,Set-OpenClSnapshot,Test-OpenClSnapshot
