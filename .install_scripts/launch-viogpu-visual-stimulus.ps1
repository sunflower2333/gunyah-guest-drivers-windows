[CmdletBinding()]
param(
    [string]$StimulusPath = 'C:\DroidVM\viogpu-58039\viogpu-visual-stimulus.ps1'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $StimulusPath -PathType Leaf)) {
    throw "Missing visual stimulus script: $StimulusPath"
}

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class InteractiveProcessLauncher
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct STARTUPINFO
    {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX;
        public int dwY;
        public int dwXSize;
        public int dwYSize;
        public int dwXCountChars;
        public int dwYCountChars;
        public int dwFillAttribute;
        public int dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public int dwProcessId;
        public int dwThreadId;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(int access, bool inheritHandle, int processId);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr process, int access, out IntPtr token);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool DuplicateTokenEx(IntPtr existingToken, int desiredAccess, IntPtr attributes,
                                                 int impersonationLevel, int tokenType, out IntPtr newToken);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CreateProcessWithTokenW(IntPtr token, int logonFlags, string applicationName,
                                                        StringBuilder commandLine, int creationFlags,
                                                        IntPtr environment, string currentDirectory,
                                                        ref STARTUPINFO startupInfo,
                                                        out PROCESS_INFORMATION processInformation);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);

    public static int Launch(int explorerProcessId, string commandLine)
    {
        const int PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
        const int TOKEN_DUPLICATE = 0x0002;
        const int TOKEN_QUERY = 0x0008;
        const int MAXIMUM_ALLOWED = 0x02000000;
        const int SECURITY_IMPERSONATION = 2;
        const int TOKEN_PRIMARY = 1;
        const int LOGON_WITH_PROFILE = 1;
        const int CREATE_NEW_CONSOLE = 0x00000010;

        IntPtr process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, explorerProcessId);
        if (process == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess failed");

        IntPtr token = IntPtr.Zero;
        IntPtr primaryToken = IntPtr.Zero;
        PROCESS_INFORMATION processInformation = new PROCESS_INFORMATION();
        try
        {
            if (!OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_QUERY, out token))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcessToken failed");
            if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, IntPtr.Zero, SECURITY_IMPERSONATION,
                                  TOKEN_PRIMARY, out primaryToken))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "DuplicateTokenEx failed");

            STARTUPINFO startupInfo = new STARTUPINFO();
            startupInfo.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            startupInfo.lpDesktop = @"winsta0\default";
            StringBuilder mutableCommandLine = new StringBuilder(commandLine);
            if (!CreateProcessWithTokenW(primaryToken, LOGON_WITH_PROFILE, null, mutableCommandLine,
                                         CREATE_NEW_CONSOLE, IntPtr.Zero, @"C:\Windows\System32",
                                         ref startupInfo, out processInformation))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcessWithTokenW failed");
            return processInformation.dwProcessId;
        }
        finally
        {
            if (processInformation.hThread != IntPtr.Zero) CloseHandle(processInformation.hThread);
            if (processInformation.hProcess != IntPtr.Zero) CloseHandle(processInformation.hProcess);
            if (primaryToken != IntPtr.Zero) CloseHandle(primaryToken);
            if (token != IntPtr.Zero) CloseHandle(token);
            CloseHandle(process);
        }
    }
}
'@

$explorer = @(
    Get-Process explorer -ErrorAction Stop |
        Where-Object { $_.SessionId -ne 0 }
)
if ($explorer.Count -ne 1) {
    throw "Expected one interactive explorer process, found $($explorer.Count)."
}

$powershellPath = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
$commandLine = "`"$powershellPath`" -NoProfile -ExecutionPolicy Bypass -File `"$StimulusPath`""
try {
    $processId = [InteractiveProcessLauncher]::Launch($explorer[0].Id, $commandLine)
} catch {
    $inner = $_.Exception.InnerException
    if ($inner -is [System.ComponentModel.Win32Exception]) {
        throw "Interactive launch failed with Win32 error $($inner.NativeErrorCode): $($inner.Message)"
    }
    throw
}
[pscustomobject]@{
    LaunchedAt = (Get-Date).ToString('o')
    SessionId = $explorer[0].SessionId
    ExplorerProcessId = $explorer[0].Id
    StimulusProcessId = $processId
} | ConvertTo-Json -Depth 3
