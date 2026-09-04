[CmdletBinding()]
param([string]$SoftwareDll = 'C:\DroidVM\ZinkD3D\viogpud3d-zink.dll')
$ErrorActionPreference = 'Continue'

Add-Type -Namespace D3D -Name Api -MemberDefinition @'
[DllImport("d3d11.dll", CallingConvention = CallingConvention.StdCall)]
public static extern int D3D11CreateDevice(
    IntPtr pAdapter, int DriverType, IntPtr Software, uint Flags,
    int[] pFeatureLevels, uint FeatureLevels, uint SDKVersion,
    out IntPtr ppDevice, out int pFeatureLevel, out IntPtr ppImmediateContext);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern IntPtr LoadLibraryW(string lpLibFileName);
'@

# D3D_DRIVER_TYPE: 0 UNKNOWN, 1 HARDWARE, 2 REFERENCE, 3 NULL, 4 SOFTWARE, 5 WARP
$levels = @(0xb000, 0xa100, 0xa000)   # 11_0, 10_1, 10_0
$SDK = 7

function Try-Create {
    param([string]$Label, [int]$Type, [IntPtr]$Module)
    $dev = [IntPtr]::Zero; $ctx = [IntPtr]::Zero; $fl = 0
    $hr = [D3D.Api]::D3D11CreateDevice([IntPtr]::Zero, $Type, $Module, 0, $levels, [uint32]$levels.Count, [uint32]$SDK, [ref]$dev, [ref]$fl, [ref]$ctx)
    "{0,-22} hr=0x{1:X8} featureLevel=0x{2:X}" -f $Label, $hr, $fl
}

Try-Create -Label 'WARP (control)'   -Type 5 -Module ([IntPtr]::Zero)
Try-Create -Label 'HARDWARE'         -Type 1 -Module ([IntPtr]::Zero)

$mod = [D3D.Api]::LoadLibraryW($SoftwareDll)
"LoadLibrary($SoftwareDll) -> 0x{0:X}" -f $mod.ToInt64()
if ($mod -ne [IntPtr]::Zero) {
    Try-Create -Label 'SOFTWARE (zink)' -Type 4 -Module $mod
}
