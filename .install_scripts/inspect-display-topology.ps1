$ErrorActionPreference='Continue'
Add-Type -AssemblyName System.Windows.Forms
Write-Output "=== SCREENS (Win32 desktop) ==="
[System.Windows.Forms.Screen]::AllScreens | ForEach-Object {
    "{0} primary={1} bounds={2}" -f $_.DeviceName, $_.Primary, $_.Bounds
}
Write-Output ""
Write-Output "=== VIDEO CONTROLLERS ==="
Get-CimInstance Win32_VideoController | ForEach-Object {
    "{0} | avail={1} | mode={2} | {3}x{4}" -f $_.Name, $_.Availability, $_.VideoModeDescription, $_.CurrentHorizontalResolution, $_.CurrentVerticalResolution
}
Write-Output ""
Write-Output "=== DESKTOP MONITORS (WMI) ==="
Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorBasicDisplayParams -ErrorAction SilentlyContinue | ForEach-Object {
    "{0} active={1}" -f $_.InstanceName, $_.Active
}
Write-Output ""
Write-Output "=== DWM / composition ==="
"DWM procs: " + (Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Id)/session$($_.SessionId)" }) -join ' '
Write-Output ""
Write-Output "=== VIOGPU DEVICE INSTANCES ==="
Get-PnpDevice -Class Display | ForEach-Object { "{0} | {1} | {2}" -f $_.FriendlyName, $_.Status, $_.InstanceId }
