[CmdletBinding()]
param(
    [int]$SettleSeconds = 12
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-Node {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
    })
    if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
    return $devices[0]
}

$before = Get-Node
Write-Output "BEFORE Status=$($before.Status) Problem=$($before.Problem) Instance=$($before.InstanceId)"

Disable-PnpDevice -InstanceId $before.InstanceId -Confirm:$false
Start-Sleep -Seconds 4
$disabled = Get-Node
Write-Output "DISABLED Status=$($disabled.Status) Problem=$($disabled.Problem)"

Enable-PnpDevice -InstanceId $before.InstanceId -Confirm:$false
Start-Sleep -Seconds $SettleSeconds

$after = Get-Node
Write-Output "AFTER Status=$($after.Status) Problem=$($after.Problem)"
Write-Output "SERVICE=$((Get-Service VioGpuWddm).Status)"
