$ErrorActionPreference = 'Continue'
$src = @"
using System;
using System.Text;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential)] public struct LUID { public int Low; public int High; }
[StructLayout(LayoutKind.Sequential)] public struct D3DKMT_ADAPTERINFO { public uint hAdapter; public LUID AdapterLuid; public uint NumOfSources; public int bPrecise; }
[StructLayout(LayoutKind.Sequential)] public struct D3DKMT_ENUMADAPTERS2 { public uint NumAdapters; public IntPtr pAdapters; }

public static class K {
    [DllImport("gdi32.dll")] public static extern int D3DKMTEnumAdapters2(ref D3DKMT_ENUMADAPTERS2 p);
    [DllImport("gdi32.dll")] public static extern int D3DKMTCreateDevice(IntPtr p);
    [DllImport("gdi32.dll")] public static extern int D3DKMTCreateContext(IntPtr p);
    [DllImport("gdi32.dll")] public static extern int D3DKMTDestroyContext(IntPtr p);
    [DllImport("gdi32.dll")] public static extern int D3DKMTDestroyDevice(IntPtr p);
    static void Zero(IntPtr p, int n) { for (int i = 0; i < n; i += 4) Marshal.WriteInt32(p, i, 0); }

    public static string Run() {
        StringBuilder sb = new StringBuilder();
        D3DKMT_ENUMADAPTERS2 e = new D3DKMT_ENUMADAPTERS2();
        D3DKMTEnumAdapters2(ref e);
        int sz = Marshal.SizeOf(typeof(D3DKMT_ADAPTERINFO));
        e.pAdapters = Marshal.AllocHGlobal(sz * (int)e.NumAdapters);
        D3DKMTEnumAdapters2(ref e);

        for (int a = 0; a < (int)e.NumAdapters; a++) {
            D3DKMT_ADAPTERINFO ai = (D3DKMT_ADAPTERINFO)Marshal.PtrToStructure(IntPtr.Add(e.pAdapters, a*sz), typeof(D3DKMT_ADAPTERINFO));
            sb.AppendLine();
            sb.AppendLine(String.Format("=== adapter[{0}] hAdapter=0x{1:X}", a, ai.hAdapter));

            IntPtr cd = Marshal.AllocHGlobal(64); Zero(cd, 64);
            Marshal.WriteInt32(cd, 0, (int)ai.hAdapter);
            int st = D3DKMTCreateDevice(cd);
            uint hDevice = (uint)Marshal.ReadInt32(cd, 12);
            sb.AppendLine(String.Format("  CreateDevice status=0x{0:X8} hDevice=0x{1:X}", st, hDevice));
            if (st != 0) { Marshal.FreeHGlobal(cd); continue; }
            sb.AppendLine(String.Format("    dev pCommandBuffer=0x{0:X} CommandBufferSize={1} AllocListSize={2} PatchListSize={3}",
                Marshal.ReadInt64(cd,16), Marshal.ReadInt32(cd,24), Marshal.ReadInt32(cd,40), Marshal.ReadInt32(cd,56)));

            IntPtr cc = Marshal.AllocHGlobal(96); Zero(cc, 96);
            Marshal.WriteInt32(cc, 0, (int)hDevice);
            Marshal.WriteInt32(cc, 4, 0);   // NodeOrdinal
            Marshal.WriteInt32(cc, 8, 1);   // EngineAffinity
            Marshal.WriteInt32(cc, 28, 6);  // ClientHint = DX11
            int cs = D3DKMTCreateContext(cc);
            uint hCtx = (uint)Marshal.ReadInt32(cc, 32);
            sb.AppendLine(String.Format("  CreateContext status=0x{0:X8} hContext=0x{1:X}", cs, hCtx));
            if (cs == 0) {
                sb.AppendLine(String.Format("    ctx pCommandBuffer=0x{0:X} CommandBufferSize={1}", Marshal.ReadInt64(cc,40), Marshal.ReadInt32(cc,48)));
                sb.AppendLine(String.Format("    ctx pAllocationList=0x{0:X} AllocationListSize={1}", Marshal.ReadInt64(cc,56), Marshal.ReadInt32(cc,64)));
                sb.AppendLine(String.Format("    ctx pPatchLocationList=0x{0:X} PatchLocationListSize={1}", Marshal.ReadInt64(cc,72), Marshal.ReadInt32(cc,80)));
                sb.AppendLine(String.Format("    ctx CommandBufferGPUVA=0x{0:X}", Marshal.ReadInt64(cc,88)));
                IntPtr dc = Marshal.AllocHGlobal(8); Zero(dc,8); Marshal.WriteInt32(dc,0,(int)hCtx);
                D3DKMTDestroyContext(dc); Marshal.FreeHGlobal(dc);
            }
            IntPtr dd = Marshal.AllocHGlobal(8); Zero(dd,8); Marshal.WriteInt32(dd,0,(int)hDevice);
            D3DKMTDestroyDevice(dd); Marshal.FreeHGlobal(dd);
            Marshal.FreeHGlobal(cc); Marshal.FreeHGlobal(cd);
        }
        return sb.ToString();
    }
}
"@
Add-Type -TypeDefinition $src
Write-Output ([K]::Run())
