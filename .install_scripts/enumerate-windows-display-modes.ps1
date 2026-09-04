[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class DroidVmDisplayModes
{
    public const int ENUM_CURRENT_SETTINGS = -1;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion;
        public short dmDriverVersion;
        public short dmSize;
        public short dmDriverExtra;
        public int dmFields;
        public int dmPositionX;
        public int dmPositionY;
        public int dmDisplayOrientation;
        public int dmDisplayFixedOutput;
        public short dmColor;
        public short dmDuplex;
        public short dmYResolution;
        public short dmTTOption;
        public short dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel;
        public int dmPelsWidth;
        public int dmPelsHeight;
        public int dmDisplayFlags;
        public int dmDisplayFrequency;
        public int dmICMMethod;
        public int dmICMIntent;
        public int dmMediaType;
        public int dmDitherType;
        public int dmReserved1;
        public int dmReserved2;
        public int dmPanningWidth;
        public int dmPanningHeight;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DEVMODE devMode);
}
'@

function New-DevMode {
    $mode = New-Object DroidVmDisplayModes+DEVMODE
    $mode.dmSize = [System.Runtime.InteropServices.Marshal]::SizeOf($mode)
    return $mode
}

$current = New-DevMode
if (-not [DroidVmDisplayModes]::EnumDisplaySettings($null, [DroidVmDisplayModes]::ENUM_CURRENT_SETTINGS, [ref]$current)) {
    throw 'EnumDisplaySettings failed for the current display mode.'
}

$modes = @()
for ($index = 0; ; ++$index) {
    $mode = New-DevMode
    if (-not [DroidVmDisplayModes]::EnumDisplaySettings($null, $index, [ref]$mode)) {
        break
    }
    $modes += [pscustomobject]@{
        Width = $mode.dmPelsWidth
        Height = $mode.dmPelsHeight
        BitsPerPixel = $mode.dmBitsPerPel
        RefreshHz = $mode.dmDisplayFrequency
    }
}

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    Current = [pscustomobject]@{
        Width = $current.dmPelsWidth
        Height = $current.dmPelsHeight
        BitsPerPixel = $current.dmBitsPerPel
        RefreshHz = $current.dmDisplayFrequency
    }
    Modes = @($modes | Sort-Object Width, Height, BitsPerPixel, RefreshHz -Unique)
} | ConvertTo-Json -Depth 5
