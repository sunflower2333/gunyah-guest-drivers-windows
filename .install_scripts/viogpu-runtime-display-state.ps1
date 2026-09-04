[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$bootTime = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
$displayDevices = @(
    Get-PnpDevice -PresentOnly |
        Where-Object { $_.Class -eq 'Display' } |
        Select-Object Status, Class, FriendlyName, InstanceId, Problem
)
$videoControllers = @(
    Get-CimInstance Win32_VideoController |
        Select-Object Name, Status, PNPDeviceID, DriverVersion, DriverDate,
            CurrentHorizontalResolution, CurrentVerticalResolution, CurrentRefreshRate,
            CurrentBitsPerPixel, VideoModeDescription, AdapterRAM
)
$desktopProcesses = @(
    Get-Process -Name dwm, explorer, LogonUI, winlogon -ErrorAction SilentlyContinue |
        Select-Object ProcessName, Id, SessionId, StartTime
)

$systemEvents = @(
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $bootTime } -ErrorAction SilentlyContinue |
        Where-Object {
            $_.ProviderName -in @(
                'Display',
                'Microsoft-Windows-Kernel-PnP',
                'Microsoft-Windows-DriverFrameworks-UserMode'
            )
        } |
        Select-Object -First 80 TimeCreated, ProviderName, Id, LevelDisplayName, Message
)

$dxgEvents = @()
foreach ($logName in @(
    'Microsoft-Windows-DxgKrnl/Operational',
    'Microsoft-Windows-DxgKrnl/Admin'
)) {
    try {
        $dxgEvents += Get-WinEvent -FilterHashtable @{ LogName = $logName; StartTime = $bootTime } `
            -ErrorAction Stop |
            Select-Object -First 120 TimeCreated, LogName, Id, LevelDisplayName, Message
    }
    catch {
        $dxgEvents += [pscustomobject]@{
            TimeCreated = Get-Date
            LogName = $logName
            Id = -1
            LevelDisplayName = 'Unavailable'
            Message = $_.Exception.Message
        }
    }
}

$sessionOutput = @(& query.exe session 2>&1)
$pnpOutput = @(& pnputil.exe /enum-devices /class Display /connected 2>&1)

[pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o')
    LastBootUpTime = $bootTime
    DisplayDevices = $displayDevices
    VideoControllers = $videoControllers
    DesktopProcesses = $desktopProcesses
    Sessions = $sessionOutput
    PnpDisplayDevices = $pnpOutput
    SystemEvents = $systemEvents
    DxgKrnlEvents = $dxgEvents
} | ConvertTo-Json -Depth 7
