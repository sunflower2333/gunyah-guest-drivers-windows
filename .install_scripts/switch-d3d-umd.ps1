[CmdletBinding()]
param(
    [ValidateSet('Zink','Restore')]
    [string]$Mode = 'Zink',
    [string]$ZinkSource = 'C:\DroidVM\ZinkD3D\viogpud3d-zink.dll',
    [string]$BackupFile = 'C:\DroidVM\ZinkD3D\umd-backup.json'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$dev = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
if ($dev.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($dev.Count)." }
$instance = $dev[0].InstanceId
$prop = Get-PnpDeviceProperty -InstanceId $instance -KeyName 'DEVPKEY_Device_Driver'
$key = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($prop.Data)"

$current = (Get-Item -LiteralPath $key).GetValue('UserModeDriverName', $null)
Write-Output "CURRENT_UMD=$($current -join ';')"

if ($Mode -eq 'Restore') {
    if (-not (Test-Path -LiteralPath $BackupFile)) { throw "No backup at $BackupFile" }
    $backup = Get-Content -LiteralPath $BackupFile -Raw | ConvertFrom-Json
    Set-ItemProperty -LiteralPath $key -Name 'UserModeDriverName' -Value ([string[]]$backup.UserModeDriverName) -Type MultiString
    Write-Output "RESTORED_UMD=$(($backup.UserModeDriverName) -join ';')"
} else {
    if (-not (Test-Path -LiteralPath $ZinkSource)) { throw "Missing Zink UMD: $ZinkSource" }
    if (-not (Test-Path -LiteralPath $BackupFile)) {
        @{ UserModeDriverName = @($current) } | ConvertTo-Json -Depth 4 |
            Set-Content -LiteralPath $BackupFile -Encoding UTF8
        Write-Output "BACKUP_WRITTEN=$BackupFile"
    } else {
        Write-Output "BACKUP_EXISTS=$BackupFile"
    }

    # The D3D runtime loads the UMD by the exact path recorded here.  Place it
    # beside the other system DLLs so the loader can always resolve it.
    $target = Join-Path $env:SystemRoot 'System32\viogpud3d-zink.dll'
    Copy-Item -LiteralPath $ZinkSource -Destination $target -Force
    Write-Output "COPIED=$target ($((Get-Item $target).Length) bytes)"

    # One entry per D3D runtime version slot, matching the shim's arity.
    $count = if ($null -eq $current) { 3 } else { @($current).Count }
    $value = @(1..$count | ForEach-Object { $target })
    Set-ItemProperty -LiteralPath $key -Name 'UserModeDriverName' -Value ([string[]]$value) -Type MultiString
    Write-Output "NEW_UMD=$($value -join ';')"
}

Write-Output "--- restarting device ---"
Disable-PnpDevice -InstanceId $instance -Confirm:$false
Start-Sleep -Seconds 3
Enable-PnpDevice -InstanceId $instance -Confirm:$false
Start-Sleep -Seconds 10
$after = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })[0]
Write-Output "DEVICE=$($after.Status)/$($after.Problem)"
Write-Output "UMD_NOW=$((Get-Item -LiteralPath $key).GetValue('UserModeDriverName', $null) -join ';')"
