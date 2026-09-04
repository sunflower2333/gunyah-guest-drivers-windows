$ErrorActionPreference = 'Continue'
$src = @"
using System;
using System.Text;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
public struct DXGI_ADAPTER_DESC {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string Description;
    public uint VendorId, DeviceId, SubSysId, Revision;
    public UIntPtr DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
    public long AdapterLuid;
}

[ComImport, Guid("2411e7e1-12ac-4ccf-bd14-9798e8534dc0"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IDXGIAdapter {
    void SetPrivateData(ref Guid n, uint s, IntPtr d);
    void SetPrivateDataInterface(ref Guid n, IntPtr u);
    void GetPrivateData(ref Guid n, ref uint s, IntPtr d);
    void GetParent(ref Guid r, out IntPtr p);
    [PreserveSig] int EnumOutputs(uint o, out IntPtr pp);
    [PreserveSig] int GetDesc(out DXGI_ADAPTER_DESC d);
    [PreserveSig] int CheckInterfaceSupport(ref Guid n, out long v);
}

[ComImport, Guid("7b7166ec-21c7-44ae-b21a-c9ae321ae369"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IDXGIFactory {
    void SetPrivateData(ref Guid n, uint s, IntPtr d);
    void SetPrivateDataInterface(ref Guid n, IntPtr u);
    void GetPrivateData(ref Guid n, ref uint s, IntPtr d);
    void GetParent(ref Guid r, out IntPtr p);
    [PreserveSig] int EnumAdapters(uint a, out IDXGIAdapter pp);
    [PreserveSig] int MakeWindowAssociation(IntPtr h, uint f);
    [PreserveSig] int GetWindowAssociation(out IntPtr h);
    [PreserveSig] int CreateSwapChain(IntPtr dev, IntPtr desc, out IntPtr sc);
    [PreserveSig] int CreateSoftwareAdapter(IntPtr mod, out IDXGIAdapter pp);
}

public static class Api {
    [DllImport("dxgi.dll")] public static extern int CreateDXGIFactory(ref Guid riid, out IDXGIFactory f);
    [DllImport("d3d11.dll")] public static extern int D3D11CreateDevice(
        IDXGIAdapter pAdapter, uint DriverType, IntPtr Software, uint Flags,
        uint[] pFeatureLevels, uint FeatureLevels, uint SDKVersion,
        out IntPtr ppDevice, out uint pFeatureLevel, out IntPtr ppContext);
    [DllImport("kernel32", CharSet=CharSet.Unicode)] public static extern IntPtr LoadLibraryW(string p);

    public static string Run() {
        StringBuilder sb = new StringBuilder();
        Guid iid = new Guid("7b7166ec-21c7-44ae-b21a-c9ae321ae369");
        IDXGIFactory fac;
        int hr = CreateDXGIFactory(ref iid, out fac);
        sb.AppendLine(String.Format("CreateDXGIFactory hr=0x{0:X8}", hr));
        if (hr != 0) return sb.ToString();

        uint[] levels = new uint[] { 0xB000, 0xA100, 0xA000, 0x9300 };
        Guid iidD3D10 = new Guid("9b7e4c00-342c-4106-a19f-4f2704f689f0"); // __uuidof(ID3D10Device)
        Guid iidD3D11 = new Guid("db6f6ddb-ac77-4e88-8253-819df9bbf140"); // ID3D11Device

        for (uint i = 0; ; i++) {
            IDXGIAdapter ad;
            if (fac.EnumAdapters(i, out ad) != 0) break;
            DXGI_ADAPTER_DESC d;
            ad.GetDesc(out d);
            sb.AppendLine();
            sb.AppendLine(String.Format("=== DXGI adapter[{0}] '{1}' VEN={2:X4} DEV={3:X4} LUID=0x{4:X} VRAM={5}",
                i, d.Description.Trim('\0').Trim(), d.VendorId, d.DeviceId, d.AdapterLuid, (ulong)d.DedicatedVideoMemory));

            long ver;
            int cs10 = ad.CheckInterfaceSupport(ref iidD3D10, out ver);
            sb.AppendLine(String.Format("    CheckInterfaceSupport(ID3D10Device) hr=0x{0:X8} UMDVersion=0x{1:X}", cs10, ver));

            IntPtr dev, ctx; uint fl;
            int r = D3D11CreateDevice(ad, 0u, IntPtr.Zero, 0u, levels, (uint)levels.Length, 7u, out dev, out fl, out ctx);
            sb.AppendLine(String.Format("    D3D11CreateDevice(UNKNOWN, explicit adapter) hr=0x{0:X8} featureLevel=0x{1:X}", r, fl));
            if (r == 0) {
                sb.AppendLine("    *** DEVICE CREATED ***");
            }
        }
        return sb.ToString();
    }
}
"@
Add-Type -TypeDefinition $src
Write-Output ([Api]::Run())

Write-Output "=== modules matching viogpu/zink/warp ==="
[System.Diagnostics.Process]::GetCurrentProcess().Refresh()
$m = [System.Diagnostics.Process]::GetCurrentProcess().Modules | Where-Object { $_.ModuleName -match 'viogpu|zink|warp' }
if ($m) { foreach ($x in $m) { Write-Output ("  {0}  {1}" -f $x.ModuleName, $x.FileName) } } else { Write-Output "  <none>" }
