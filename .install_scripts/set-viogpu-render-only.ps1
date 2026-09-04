[CmdletBinding()]
param(
    [ValidateSet('Display','RenderOnly')]
    [string]$Mode = 'Display',
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# viogpudo.cpp:
#   SetRenderOnly(VioGpuWddmIsRenderOnlyRegistration() || !NT_SUCCESS(read) || !!value)
# so the adapter is a full display device only when BOTH the service Parameters
# value and the device driver-key value are present and zero.
$want = if ($Mode -eq 'Display') { 0 } else { 1 }

$dev = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
if ($dev.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($dev.Count)." }
$instance = $dev[0].InstanceId
$prop = Get-PnpDeviceProperty -InstanceId $instance -KeyName 'DEVPKEY_Device_Driver'
$driverKey = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($prop.Data)"
$serviceParams = 'HKLM:\SYSTEM\CurrentControlSet\Services\VioGpuWddm\Parameters'

Write-Output "BEFORE service=$((Get-ItemProperty -Path $serviceParams -Name RenderOnly -ErrorAction SilentlyContinue).RenderOnly)"
Write-Output "BEFORE device =$((Get-Item -LiteralPath $driverKey).GetValue('RenderOnly','<unset>'))"

if (-not (Test-Path $serviceParams)) { New-Item -Path $serviceParams -Force | Out-Null }
Set-ItemProperty -Path $serviceParams -Name 'RenderOnly' -Value $want -Type DWord
Set-ItemProperty -LiteralPath $driverKey -Name 'RenderOnly' -Value $want -Type DWord

Write-Output "AFTER  service=$((Get-ItemProperty -Path $serviceParams -Name RenderOnly).RenderOnly)"
Write-Output "AFTER  device =$((Get-Item -LiteralPath $driverKey).GetValue('RenderOnly'))"

if (-not $NoRestart) {
    Write-Output "--- restarting device ---"
    Disable-PnpDevice -InstanceId $instance -Confirm:$false
    Start-Sleep -Seconds 3
    Enable-PnpDevice -InstanceId $instance -Confirm:$false
    Start-Sleep -Seconds 12
    $after = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })[0]
    Write-Output "DEVICE=$($after.Status)/$($after.Problem)"
    $vc = Get-CimInstance Win32_VideoController | Where-Object { $_.Name -like '*VirtIO GPU*' }
    Write-Output "MODE=$($vc.VideoModeDescription) RES=$($vc.CurrentHorizontalResolution)x$($vc.CurrentVerticalResolution)"
}
