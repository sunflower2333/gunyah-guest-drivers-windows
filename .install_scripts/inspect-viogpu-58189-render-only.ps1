[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedVersion = '100.6.101.58189'
$expectedInf = 'oem25.inf'
$expectedSysHash = 'e77c64561ce642085e6c85ac962cf5613f6ba124942df53ac7bb0836aefd252c'
$expectedUmdHash = 'a3f5d72f65ce6957661770ef7892027a569f18470c926accaeee44658ea6404c'
$expectedSigner = 'CN=DroidVM Test'

$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
})
if ($devices.Count -ne 1) {
    throw "Expected one present virtio-gpu device, found $($devices.Count)."
}

$device = $devices[0]
$driverProperty = Get-PnpDeviceProperty `
    -InstanceId $device.InstanceId `
    -KeyName 'DEVPKEY_Device_Driver'
$driverKeyPath = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($driverProperty.Data)"
$driverKey = Get-Item -LiteralPath $driverKeyPath
$service = Get-CimInstance Win32_SystemDriver -Filter "Name='VioGpuWddm'"
if ($null -eq $service) {
    throw 'VioGpuWddm service is missing.'
}

$imagePath = [string]$service.PathName
if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
    $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
}
$imagePath = [Environment]::ExpandEnvironmentVariables($imagePath).Trim('"')
$image = Get-Item -LiteralPath $imagePath
$umdPath = Join-Path $image.DirectoryName 'viogpud3d.dll'
$signature = Get-AuthenticodeSignature -LiteralPath $image.FullName

$snapshot = [ordered]@{
    CollectedAt = (Get-Date).ToString('o')
    BootTime = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
    VioGpu = [ordered]@{
        Status = [string]$device.Status
        ProblemCode = if ($null -ne $device.PSObject.Properties['ProblemCode']) {
            $device.ProblemCode
        } else {
            $null
        }
        InstanceId = [string]$device.InstanceId
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        RenderOnly = $driverKey.GetValue('RenderOnly', $null)
        ServiceState = [string]$service.State
        ServiceStatus = [string]$service.Status
        ImagePath = $image.FullName
        ImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        UmdPath = $umdPath
        UmdSha256 = (Get-FileHash -LiteralPath $umdPath -Algorithm SHA256).Hash.ToLowerInvariant()
        SignatureStatus = [string]$signature.Status
        SignerSubject = if ($null -eq $signature.SignerCertificate) {
            $null
        } else {
            $signature.SignerCertificate.Subject
        }
    }
    DisplayPnp = @(Get-PnpDevice -PresentOnly -Class Display | ForEach-Object {
        [ordered]@{
            Status = [string]$_.Status
            ProblemCode = if ($null -ne $_.PSObject.Properties['ProblemCode']) {
                $_.ProblemCode
            } else {
                $null
            }
            FriendlyName = [string]$_.FriendlyName
            InstanceId = [string]$_.InstanceId
        }
    })
    VideoControllers = @(Get-CimInstance Win32_VideoController | ForEach-Object {
        [ordered]@{
            Name = [string]$_.Name
            PnpDeviceId = [string]$_.PNPDeviceID
            Status = [string]$_.Status
            CurrentHorizontalResolution = $_.CurrentHorizontalResolution
            CurrentVerticalResolution = $_.CurrentVerticalResolution
            CurrentRefreshRate = $_.CurrentRefreshRate
        }
    })
    DesktopMonitors = @(Get-CimInstance Win32_DesktopMonitor | ForEach-Object {
        [ordered]@{
            Name = [string]$_.Name
            PnpDeviceId = [string]$_.PNPDeviceID
            Status = [string]$_.Status
            ScreenWidth = $_.ScreenWidth
            ScreenHeight = $_.ScreenHeight
        }
    })
}

$vioGpu = $snapshot.VioGpu
if ($vioGpu.Status -ne 'OK' -or
    ($null -ne $vioGpu.ProblemCode -and [int]$vioGpu.ProblemCode -ne 0) -or
    $vioGpu.DriverVersion -ne $expectedVersion -or
    $vioGpu.InfPath -ne $expectedInf -or
    $null -eq $vioGpu.RenderOnly -or
    [int]$vioGpu.RenderOnly -ne 1 -or
    $vioGpu.ServiceState -ne 'Running' -or
    $vioGpu.ImageSha256 -ne $expectedSysHash -or
    $vioGpu.UmdSha256 -ne $expectedUmdHash -or
    $vioGpu.SignatureStatus -ne 'Valid' -or
    $vioGpu.SignerSubject -ne $expectedSigner) {
    throw "Post-boot VioGPU state is not exact: $($snapshot | ConvertTo-Json -Depth 8 -Compress)"
}

$snapshot | ConvertTo-Json -Depth 8
