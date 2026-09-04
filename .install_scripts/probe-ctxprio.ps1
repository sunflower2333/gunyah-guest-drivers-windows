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
    [DllImport("gdi32.dll")] public static extern int D3DKMTSetContextSchedulingPriority(IntPtr p);
    [DllImport("gdi32.dll")] public static extern int D3DKMTGetContextSchedulingPriority(IntPtr p);
    [DllImport("gdi32.dll")] public static extern int D3DKMTDestroyContext(IntPtr p);
    [DllImport("gdi32.dll")] public static extern int D3DKMTDestroyDevice(IntPtr p);
    static void Zero(IntPtr p,int n){for(int i=0;i<n;i+=4)Marshal.WriteInt32(p,i,0);}
    public static string Run() {
        StringBuilder sb=new StringBuilder();
        D3DKMT_ENUMADAPTERS2 e=new D3DKMT_ENUMADAPTERS2();
        D3DKMTEnumAdapters2(ref e);
        int sz=Marshal.SizeOf(typeof(D3DKMT_ADAPTERINFO));
        e.pAdapters=Marshal.AllocHGlobal(sz*(int)e.NumAdapters);
        D3DKMTEnumAdapters2(ref e);
        for (int a=0; a<(int)e.NumAdapters && a<2; a++) {
            D3DKMT_ADAPTERINFO ai=(D3DKMT_ADAPTERINFO)Marshal.PtrToStructure(IntPtr.Add(e.pAdapters,a*sz),typeof(D3DKMT_ADAPTERINFO));
            sb.AppendLine();
            sb.AppendLine(String.Format("=== adapter[{0}] hAdapter=0x{1:X}", a, ai.hAdapter));
            IntPtr cd=Marshal.AllocHGlobal(64); Zero(cd,64);
            Marshal.WriteInt32(cd,0,(int)ai.hAdapter);
            if (D3DKMTCreateDevice(cd)!=0) { sb.AppendLine("  CreateDevice failed"); continue; }
            uint hDev=(uint)Marshal.ReadInt32(cd,12);
            IntPtr cc=Marshal.AllocHGlobal(96); Zero(cc,96);
            Marshal.WriteInt32(cc,0,(int)hDev);
            Marshal.WriteInt32(cc,8,1);    // EngineAffinity
            Marshal.WriteInt32(cc,28,6);   // ClientHint DX11
            int cs=D3DKMTCreateContext(cc);
            uint hCtx=(uint)Marshal.ReadInt32(cc,32);
            sb.AppendLine(String.Format("  CreateContext=0x{0:X8} hContext=0x{1:X}", cs, hCtx));
            if (cs==0) {
                // D3DKMT_SETCONTEXTSCHEDULINGPRIORITY { D3DKMT_HANDLE hContext; INT Priority; }
                IntPtr sp=Marshal.AllocHGlobal(8); Zero(sp,8);
                Marshal.WriteInt32(sp,0,(int)hCtx);
                Marshal.WriteInt32(sp,4,1);
                int ss=D3DKMTSetContextSchedulingPriority(sp);
                sb.AppendLine(String.Format("  SetContextSchedulingPriority(1) = 0x{0:X8}{1}", ss, ss==unchecked((int)0xC00000BB)?"   <<< STATUS_NOT_SUPPORTED":""));
                Zero(sp,8); Marshal.WriteInt32(sp,0,(int)hCtx);
                int gs=D3DKMTGetContextSchedulingPriority(sp);
                sb.AppendLine(String.Format("  GetContextSchedulingPriority   = 0x{0:X8} priority={1}", gs, Marshal.ReadInt32(sp,4)));
                Marshal.FreeHGlobal(sp);
                IntPtr dc=Marshal.AllocHGlobal(8); Zero(dc,8); Marshal.WriteInt32(dc,0,(int)hCtx);
                D3DKMTDestroyContext(dc); Marshal.FreeHGlobal(dc);
            }
            IntPtr dd=Marshal.AllocHGlobal(8); Zero(dd,8); Marshal.WriteInt32(dd,0,(int)hDev);
            D3DKMTDestroyDevice(dd); Marshal.FreeHGlobal(dd);
            Marshal.FreeHGlobal(cc); Marshal.FreeHGlobal(cd);
        }
        return sb.ToString();
    }
}
"@
Add-Type -TypeDefinition $src
Write-Output ([K]::Run())
