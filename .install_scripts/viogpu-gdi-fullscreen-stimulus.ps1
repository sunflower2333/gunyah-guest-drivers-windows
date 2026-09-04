[CmdletBinding()]
param(
    [ValidateRange(1, 600)]
    [int]$DurationSeconds = 120,
    [string]$StatePath = 'C:\DroidVM\viogpu-58043\viogpu-gdi-stimulus-state.json'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class DesktopDcStimulus
{
    [StructLayout(LayoutKind.Sequential)]
    private struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetDC(IntPtr window);

    [DllImport("user32.dll")]
    private static extern int ReleaseDC(IntPtr window, IntPtr dc);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int FillRect(IntPtr dc, ref RECT rect, IntPtr brush);

    [DllImport("gdi32.dll", SetLastError = true)]
    private static extern IntPtr CreateSolidBrush(uint colorRef);

    [DllImport("gdi32.dll")]
    private static extern bool DeleteObject(IntPtr obj);

    public static void Fill(int width, int height, uint colorRef)
    {
        IntPtr dc = GetDC(IntPtr.Zero);
        if (dc == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "GetDC failed");
        IntPtr brush = CreateSolidBrush(colorRef);
        if (brush == IntPtr.Zero)
        {
            ReleaseDC(IntPtr.Zero, dc);
            throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateSolidBrush failed");
        }
        try
        {
            RECT rect = new RECT { Left = 0, Top = 0, Right = width, Bottom = height };
            if (FillRect(dc, ref rect, brush) == 0)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "FillRect failed");
        }
        finally
        {
            DeleteObject(brush);
            ReleaseDC(IntPtr.Zero, dc);
        }
    }
}
'@

Add-Type -AssemblyName System.Windows.Forms
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$state = [ordered]@{
    ProcessId = $PID
    SessionId = (Get-Process -Id $PID).SessionId
    StartedAt = (Get-Date).ToString('o')
    Width = $bounds.Width
    Height = $bounds.Height
    FillCount = 0
    Error = $null
}
$colors = @(0x00FF00FF, 0x0000FF00, 0x00FF8000, 0x0000FFFF)

try {
    $deadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        [DesktopDcStimulus]::Fill($bounds.Width, $bounds.Height, $colors[$state.FillCount % $colors.Count])
        ++$state.FillCount
        if (($state.FillCount % 10) -eq 1) {
            $state | ConvertTo-Json | Set-Content -LiteralPath $StatePath -Encoding UTF8
        }
        Start-Sleep -Milliseconds 100
    }
}
catch {
    $state.Error = $_ | Out-String
    throw
}
finally {
    $state | ConvertTo-Json | Set-Content -LiteralPath $StatePath -Encoding UTF8
}
