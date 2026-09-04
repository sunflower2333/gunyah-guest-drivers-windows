$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

Write-Output '=== OS ==='
Get-CimInstance Win32_OperatingSystem |
    Select-Object Caption,Version,LastBootUpTime,OSArchitecture |
    Format-List

Write-Output '=== Virtio GPU ==='
$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI*VEN_1AF4*DEV_1050*'
})
$devices | Select-Object Status,Class,FriendlyName,InstanceId | Format-List
Get-CimInstance Win32_PnPSignedDriver | Where-Object {
    $_.DeviceID -like 'PCI*VEN_1AF4*DEV_1050*'
} | Select-Object DeviceID,DriverVersion,InfName,DriverDate,Manufacturer | Format-List

if ($devices.Count -eq 1) {
    $prop = Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
    $driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($prop.Data)"
    Write-Output "DriverKey=$driverKeyPath"
    $key = Get-Item -LiteralPath $driverKeyPath -ErrorAction SilentlyContinue
    if ($null -ne $key) {
        $key.GetValueNames() |
            Where-Object { $_ -like 'Native*' } |
            Sort-Object |
            ForEach-Object {
                [pscustomobject]@{
                    Name = $_
                    Kind = [string]$key.GetValueKind($_)
                    Value = $key.GetValue($_, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                }
            } | Format-List
    }
}

Write-Output '=== Bugcheck events ==='
Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddHours(-12) } -ErrorAction SilentlyContinue |
    Where-Object { $_.Id -in @(41,1001,6008) } |
    Select-Object TimeCreated,Id,ProviderName,Message |
    Format-List

Write-Output '=== WHEA events ==='
Get-WinEvent -LogName 'Microsoft-Windows-WHEA-Logger/Operational' -MaxEvents 50 -ErrorAction SilentlyContinue |
    Select-Object TimeCreated,Id,LevelDisplayName,ProviderName,Message |
    Format-List

Write-Output '=== Display/DxgKrnl events ==='
Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddHours(-12) } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'Display|DxgKrnl|WHEA|LiveKernel|Kernel-PnP' } |
    Select-Object TimeCreated,Id,ProviderName,Message |
    Format-List

Write-Output '=== Dumps ==='
@('C:\Windows\MEMORY.DMP') + @(
    Get-ChildItem 'C:\Windows\Minidump' -File -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty FullName
) | ForEach-Object {
    if (Test-Path -LiteralPath $_ -PathType Leaf) {
        $f = Get-Item -LiteralPath $_
        [pscustomobject]@{
            Path = $f.FullName
            Length = $f.Length
            LastWriteTimeUtc = $f.LastWriteTimeUtc
            Sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
        }
    }
} | Format-List

Write-Output '=== WER report metadata ==='
Get-ChildItem 'C:\ProgramData\Microsoft\Windows\WER\ReportArchive','C:\ProgramData\Microsoft\Windows\WER\ReportQueue' -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.wer','.xml','.txt' -or $_.Name -match 'Report.wer|metadata' } |
    Sort-Object LastWriteTime |
    Select-Object -Last 40 FullName,Length,LastWriteTime |
    Format-Table -AutoSize
