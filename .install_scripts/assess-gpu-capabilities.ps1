[CmdletBinding()]
param()
$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

Write-Output "=== DISPLAY ADAPTERS (PnP) ==="
Get-PnpDevice -Class Display -PresentOnly | ForEach-Object {
    "{0} | {1} | {2}" -f $_.FriendlyName, $_.Status, $_.InstanceId
}

Write-Output ""
Write-Output "=== VIDEO CONTROLLERS (WMI) ==="
Get-CimInstance Win32_VideoController | ForEach-Object {
    "Name={0}" -f $_.Name
    "  DriverVersion={0} Status={1} Availability={2}" -f $_.DriverVersion, $_.Status, $_.Availability
    "  VideoModeDescription={0}" -f $_.VideoModeDescription
    "  AdapterRAM={0} VideoProcessor={1}" -f $_.AdapterRAM, $_.VideoProcessor
    "  CurrentHorizontalResolution={0} CurrentVerticalResolution={1}" -f $_.CurrentHorizontalResolution, $_.CurrentVerticalResolution
}

Write-Output ""
Write-Output "=== MONITORS ==="
Get-PnpDevice -Class Monitor -PresentOnly -ErrorAction SilentlyContinue | ForEach-Object {
    "{0} | {1} | {2}" -f $_.FriendlyName, $_.Status, $_.InstanceId
}

Write-Output ""
Write-Output "=== VULKAN ICD REGISTRY ==="
foreach ($k in @('HKLM:\SOFTWARE\Khronos\Vulkan\Drivers','HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers')) {
    if (Test-Path $k) {
        Write-Output "$k :"
        (Get-Item $k).GetValueNames() | ForEach-Object { "   {0} = {1}" -f $_, (Get-ItemPropertyValue $k $_) }
    } else { Write-Output "$k : <absent>" }
}

Write-Output ""
Write-Output "=== D3D UMD REGISTRY (display class key) ==="
$dev = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
if ($dev.Count -eq 1) {
    $p = Get-PnpDeviceProperty -InstanceId $dev[0].InstanceId -KeyName 'DEVPKEY_Device_Driver'
    $k = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($p.Data)"
    foreach ($n in @('UserModeDriverName','UserModeDriverNameWow','OpenGLDriverName','InstalledDisplayDrivers','VgaCompatible','Capabilities')) {
        $v = (Get-Item -LiteralPath $k).GetValue($n, '<unset>')
        "   {0} = {1}" -f $n, ($v -join ',')
    }
}

Write-Output ""
Write-Output "=== DXGI / D3D FEATURE (dxdiag-lite) ==="
Write-Output "SystemInfo OS: $((Get-CimInstance Win32_OperatingSystem).Caption) $((Get-CimInstance Win32_OperatingSystem).Version)"
Write-Output "DWM running: $((Get-Process dwm -ErrorAction SilentlyContinue | Measure-Object).Count -gt 0)"
Write-Output "Session: $((Get-Process -Id $PID).SessionId)"

Write-Output ""
Write-Output "=== SYSTEM32 GPU FILES ==="
foreach ($f in @('vulkan-1.dll','vulkaninfo.exe','viogpud3d.dll','d3d11.dll','dxgi.dll')) {
    $p = Join-Path $env:SystemRoot "System32\$f"
    if (Test-Path $p) { "   {0} present ({1} bytes)" -f $f, (Get-Item $p).Length } else { "   {0} absent" -f $f }
}
