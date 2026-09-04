$ErrorActionPreference='SilentlyContinue'
Write-Output "=== Win32_VideoController ==="
Get-CimInstance Win32_VideoController | ForEach-Object {
  "{0} | mode={1} | status={2} | availability={3}" -f $_.Name, $_.VideoModeDescription, $_.Status, $_.Availability
}
Write-Output ""
Write-Output "=== Win32_DesktopMonitor ==="
Get-CimInstance Win32_DesktopMonitor | ForEach-Object { "{0} | {1} | {2}" -f $_.Name, $_.ScreenWidth, $_.ScreenHeight }
Write-Output ""
Write-Output "=== EnumDisplayDevices ==="
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
public struct DISPLAY_DEVICE {
  public int cb;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string DeviceName;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceString;
  public int StateFlags;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceID;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceKey;
}
public static class U {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern bool EnumDisplayDevices(string dev, uint num, ref DISPLAY_DEVICE dd, uint flags);
}
"@
for ($i=0; $i -lt 6; $i++) {
  $dd = New-Object DISPLAY_DEVICE; $dd.cb = [System.Runtime.InteropServices.Marshal]::SizeOf($dd)
  if (-not [U]::EnumDisplayDevices($null, [uint32]$i, [ref]$dd, 0)) { break }
  $flags = @()
  if ($dd.StateFlags -band 0x1) { $flags += 'ACTIVE' }
  if ($dd.StateFlags -band 0x4) { $flags += 'PRIMARY' }
  if ($dd.StateFlags -band 0x8) { $flags += 'MIRRORING' }
  "{0} | {1} | {2}" -f $dd.DeviceName, $dd.DeviceString, ($flags -join ',')
}
