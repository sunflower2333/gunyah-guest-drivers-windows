param(
    [ValidateRange(640,8192)][int]$Width = 3040,
    [ValidateRange(480,8192)][int]$Height = 1904,
    [ValidateRange(20,360)][int]$Hz = 165,
    [ValidateRange(5,30)][int]$HoldSeconds = 5,
    [switch]$Apply,
    [switch]$Keep
)
$ErrorActionPreference = 'Stop'
if ((Get-Process -Id $PID).SessionId -eq 0) { throw 'Must run in the interactive console session' }
if ($Keep -and -not $Apply) { throw '-Keep requires -Apply' }
Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
public static class VioGpuDisplayModeTest {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    static extern bool EnumDisplaySettings(string device,int mode,IntPtr settings);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    static extern int ChangeDisplaySettingsEx(string device,IntPtr settings,IntPtr hwnd,uint flags,IntPtr param);
    static byte[] Read(int index) {
        IntPtr p=Marshal.AllocHGlobal(220);
        try {
            Marshal.Copy(new byte[220],0,p,220); Marshal.WriteInt16(p,68,220);
            if(!EnumDisplaySettings(null,index,p)) return null;
            var result=new byte[220]; Marshal.Copy(p,result,0,220); return result;
        } finally {Marshal.FreeHGlobal(p);}
    }
    static int Value(byte[] mode,int offset) {return BitConverter.ToInt32(mode,offset);}
    static bool Matches(byte[] m,int w,int h,int hz) {
        return m!=null && Value(m,172)==w && Value(m,176)==h && Value(m,184)==hz && Value(m,168)==32;
    }
    static void Report(string label,byte[] mode) {
        if(mode==null) Console.WriteLine(label+"=NONE");
        else Console.WriteLine("{0}={1}x{2}@{3} BPP={4}",label,Value(mode,172),Value(mode,176),Value(mode,184),Value(mode,168));
    }
    static void Change(byte[] mode,uint flags,string phase) {
        IntPtr p=Marshal.AllocHGlobal(220);
        try {
            Marshal.Copy(mode,0,p,220);
            Console.WriteLine("BEGIN={0} FLAGS={1} UTC={2:O}",phase,flags,DateTime.UtcNow);
            var timer=Stopwatch.StartNew();
            int result=ChangeDisplaySettingsEx(null,p,IntPtr.Zero,flags,IntPtr.Zero);
            timer.Stop();
            Console.WriteLine("END={0} RESULT={1} ELAPSED_MS={2}",phase,result,timer.ElapsedMilliseconds);
            Report("AFTER_"+phase,Read(-1));
            if(result!=0) throw new Exception("ChangeDisplaySettingsEx phase="+phase+" flags="+flags+" result="+result);
        } finally {Marshal.FreeHGlobal(p);}
    }
    public static void Run(int w,int h,int hz,int seconds,bool apply,bool keep) {
        byte[] before=Read(-1),target=null;
        if(before==null) throw new Exception("No active display");
        Report("BEFORE",before);
        for(int i=0;i<256;i++) {
            byte[] mode=Read(i); if(mode==null) break;
            if(Matches(mode,w,h,hz)) {target=mode; Console.WriteLine("TARGET_INDEX="+i); break;}
        }
        if(target==null) throw new Exception("Requested mode is not enumerated; refusing invented timing");
        Change(target,2,"TEST");
        if(!apply) {Console.WriteLine("TEST_ONLY=1"); return;}
        bool changed=false,validated=false;
        try {
            Change(target,0,"APPLY"); changed=true;
            if(!Matches(Read(-1),w,h,hz)) throw new Exception("Selected mode does not match request");
            System.Threading.Thread.Sleep(seconds*1000);
            if(!Matches(Read(-1),w,h,hz)) throw new Exception("Mode changed during hold");
            if(keep) Change(target,1,"SAVE");
            validated=true;
        } finally {
            if(changed && (!keep || !validated)) {
                Change(before,0,"RESTORE");
                if(!Matches(Read(-1),Value(before,172),Value(before,176),Value(before,184)))
                    throw new Exception("Original display mode restore did not stick");
            }
        }
    }
}
'@
[VioGpuDisplayModeTest]::Run($Width,$Height,$Hz,$HoldSeconds,[bool]$Apply,[bool]$Keep)
