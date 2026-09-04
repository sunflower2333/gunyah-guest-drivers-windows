[CmdletBinding()]
param([string]$Umd = 'C:\DroidVM\ZinkD3D2\viogpud3d-zink.dll')
$ErrorActionPreference = 'Stop'

Add-Type -Namespace D3 -Name Api -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern IntPtr LoadLibraryExW(string lib, IntPtr file, uint flags);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern IntPtr GetProcAddress(IntPtr mod, string name);
[DllImport("d3d11.dll")]
public static extern int D3D11CreateDevice(IntPtr adapter, int driverType, IntPtr software,
    uint flags, int[] featureLevels, uint numLevels, uint sdkVersion,
    out IntPtr device, out int featureLevel, out IntPtr context);
'@

$LOAD_APPDIR_AND_SYS = 0x00000200 -bor 0x00000800
$h = [D3.Api]::LoadLibraryExW($Umd, [IntPtr]::Zero, $LOAD_APPDIR_AND_SYS)
if ($h -eq [IntPtr]::Zero) { Write-Output "LoadLibraryExW failed: $([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message)"; exit 1 }
Write-Output "module loaded: 0x$($h.ToString('X'))"
foreach ($e in @('OpenAdapter','OpenAdapter10','OpenAdapter10_2')) {
    $p = [D3.Api]::GetProcAddress($h, $e)
    "   export {0}: {1}" -f $e, $(if ($p -eq [IntPtr]::Zero) { 'ABSENT' } else { '0x' + $p.ToString('X') })
}

# D3D_DRIVER_TYPE: UNKNOWN=0 HARDWARE=1 REFERENCE=2 NULL=3 SOFTWARE=4 WARP=5
# D3D11_SDK_VERSION = 7.  SOFTWARE takes the UMD module; WARP/HARDWARE need NULL.
$levels = @(0xb000, 0xa100, 0xa000)
$cases = @(
    @{ n = 'SOFTWARE + zink UMD'; t = 4; m = $h },
    @{ n = 'WARP (control)';      t = 5; m = [IntPtr]::Zero },
    @{ n = 'HARDWARE (control)';  t = 1; m = [IntPtr]::Zero }
)
foreach ($c in $cases) {
    $dev = [IntPtr]::Zero; $ctx = [IntPtr]::Zero; $fl = 0
    try {
        $hr = [D3.Api]::D3D11CreateDevice([IntPtr]::Zero, $c.t, $c.m, 0, $levels, [uint32]$levels.Count, 7,
                                          [ref]$dev, [ref]$fl, [ref]$ctx)
        "{0,-22} -> hr=0x{1:X8} featureLevel=0x{2:X}" -f $c.n, $hr, $fl
    } catch {
        "{0,-22} -> EXCEPTION {1}" -f $c.n, $_.Exception.Message
    }
}
