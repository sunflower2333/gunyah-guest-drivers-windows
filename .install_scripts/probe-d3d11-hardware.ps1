[CmdletBinding()]
param([string]$Log = 'C:\DroidVM\ZinkD3D2\mesa-hw.log')
$ErrorActionPreference = 'Stop'
Remove-Item -LiteralPath $Log -ErrorAction SilentlyContinue
$env:MESA_LOG_FILE = $Log
$env:GALLIUM_DRIVER = 'zink'
$env:MESA_DEBUG = 'verbose'

Add-Type -Namespace D3H -Name Api -MemberDefinition @'
[DllImport("d3d11.dll")]
public static extern int D3D11CreateDevice(IntPtr adapter, int driverType, IntPtr software,
    uint flags, int[] featureLevels, uint numLevels, uint sdkVersion,
    out IntPtr device, out int featureLevel, out IntPtr context);
'@

$levels = @(0xb000, 0xa100, 0xa000)
foreach ($flags in @(0, 2)) {   # 0 = none, 2 = D3D11_CREATE_DEVICE_DEBUG
    $dev=[IntPtr]::Zero; $ctx=[IntPtr]::Zero; $fl=0
    $hr = [D3H.Api]::D3D11CreateDevice([IntPtr]::Zero, 1, [IntPtr]::Zero, $flags, $levels,
                                       [uint32]$levels.Count, 7, [ref]$dev, [ref]$fl, [ref]$ctx)
    "HARDWARE flags=$flags -> hr=0x{0:X8} featureLevel=0x{1:X}" -f $hr, $fl
}

Write-Output "--- mesa log ---"
if (Test-Path -LiteralPath $Log) { Get-Content -LiteralPath $Log -Tail 30 } else { Write-Output "<none written: UMD never entered>" }
