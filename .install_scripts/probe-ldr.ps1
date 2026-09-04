$ErrorActionPreference = 'Continue'
$src = @"
using System;
using System.Text;
using System.Collections.Generic;
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

[StructLayout(LayoutKind.Sequential)]
public struct UNICODE_STRING { public ushort Length; public ushort MaximumLength; public IntPtr Buffer; }

[StructLayout(LayoutKind.Sequential)]
public struct LDR_DLL_NOTIFICATION_DATA {
    public uint Flags;
    public IntPtr FullDllName;   // PCUNICODE_STRING
    public IntPtr BaseDllName;
    public IntPtr DllBase;
    public uint SizeOfImage;
}

public static class Api {
    public delegate void LdrCallback(uint reason, IntPtr data, IntPtr ctx);

    [DllImport("ntdll.dll")] public static extern int LdrRegisterDllNotification(uint flags, LdrCallback cb, IntPtr ctx, out IntPtr cookie);
    [DllImport("ntdll.dll")] public static extern int LdrUnregisterDllNotification(IntPtr cookie);
    [DllImport("dxgi.dll")] public static extern int CreateDXGIFactory(ref Guid riid, out IDXGIFactory f);
    [DllImport("d3d11.dll")] public static extern int D3D11CreateDevice(
        IDXGIAdapter pAdapter, uint DriverType, IntPtr Software, uint Flags,
        uint[] pFeatureLevels, uint FeatureLevels, uint SDKVersion,
        out IntPtr ppDevice, out uint pFeatureLevel, out IntPtr ppContext);

    static List<string> events = new List<string>();
    static LdrCallback keepAlive;

    static string ReadUS(IntPtr p) {
        if (p == IntPtr.Zero) return "<null>";
        UNICODE_STRING u = (UNICODE_STRING)Marshal.PtrToStructure(p, typeof(UNICODE_STRING));
        if (u.Buffer == IntPtr.Zero) return "<null>";
        return Marshal.PtrToStringUni(u.Buffer, u.Length / 2);
    }

    static void OnLdr(uint reason, IntPtr data, IntPtr ctx) {
        try {
            LDR_DLL_NOTIFICATION_DATA d = (LDR_DLL_NOTIFICATION_DATA)Marshal.PtrToStructure(data, typeof(LDR_DLL_NOTIFICATION_DATA));
            string full = ReadUS(d.FullDllName);
            events.Add(String.Format("  [{0}] {1}", reason == 1 ? "LOAD  " : "UNLOAD", full));
        } catch (Exception ex) { events.Add("  <callback error " + ex.Message + ">"); }
    }

    public static string Run() {
        StringBuilder sb = new StringBuilder();
        keepAlive = new LdrCallback(OnLdr);
        IntPtr cookie;
        int rs = LdrRegisterDllNotification(0, keepAlive, IntPtr.Zero, out cookie);
        sb.AppendLine(String.Format("LdrRegisterDllNotification status=0x{0:X8}", rs));

        Guid iid = new Guid("7b7166ec-21c7-44ae-b21a-c9ae321ae369");
        IDXGIFactory fac;
        int hr = CreateDXGIFactory(ref iid, out fac);
        if (hr != 0) { sb.AppendLine("factory failed"); return sb.ToString(); }

        uint[] levels = new uint[] { 0xB000, 0xA100, 0xA000, 0x9300 };
        IDXGIAdapter ad;
        if (fac.EnumAdapters(0, out ad) != 0) { sb.AppendLine("no adapter 0"); return sb.ToString(); }
        DXGI_ADAPTER_DESC d; ad.GetDesc(out d);
        sb.AppendLine("Target: " + d.Description.Trim('\0').Trim());

        events.Clear();
        events.Add("--- loader events during D3D11CreateDevice ---");
        IntPtr dev, cx; uint fl;
        int r = D3D11CreateDevice(ad, 0u, IntPtr.Zero, 0u, levels, (uint)levels.Length, 7u, out dev, out fl, out cx);
        sb.AppendLine(String.Format("D3D11CreateDevice hr=0x{0:X8} featureLevel=0x{1:X}", r, fl));
        foreach (string s in events) sb.AppendLine(s);

        LdrUnregisterDllNotification(cookie);
        return sb.ToString();
    }
}
"@
Add-Type -TypeDefinition $src
Write-Output ([Api]::Run())
