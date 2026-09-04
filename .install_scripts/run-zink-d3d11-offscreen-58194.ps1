[CmdletBinding()]
param(
    [string]$BundleRoot = 'C:\Users\USER\viogpu-zink-0809cd35',
    [string]$EvidenceRoot = 'C:\Users\USER\viogpu-zink-0809cd35\evidence',
    [ValidateRange(1, 300)][int]$TimeoutSeconds = 120,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedBundleHashes = [ordered]@{
    'viogpud3d-zink.dll' = '02fe46cc550676b7eda752dc70905b91dc404eded084cb9fb56565c0d87e8991'
    'zink_d3d11_offscreen.exe' = 'c5998734901ed7f92d5e502fc7308b3455bfa212e0b7e11b73ecabbe83c93b99'
}
$expectedDriverVersion = '100.6.101.58194'
$expectedDriverImageHash = '45ef273edf3cd4766a541ecb3952a47165735c074037eb851dc59ec753e270d4'
$expectedDriverUmdHash = '3860bfeb8c9834788535241a2adaf4933fea30f71de2081d2896c24884a77736'
$expectedSignerSubject = 'CN=DroidVM Test'
$successPattern = '^Zink D3D11 offscreen probe: PASS feature_level=0x[0-9A-Fa-f]+ checksum=2088960\s*$'

function Get-OptionalPnpData {
    param(
        [Parameter(Mandatory)][string]$InstanceId,
        [Parameter(Mandatory)][string]$KeyName
    )

    $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $KeyName -ErrorAction SilentlyContinue
    if ($null -eq $property) { return $null }
    $dataProperty = $property.PSObject.Properties['Data']
    if ($null -eq $dataProperty) { return $null }
    return $dataProperty.Value
}

function Assert-ExactBundle {
    if (-not (Test-Path -LiteralPath $BundleRoot -PathType Container)) { throw "Missing bundle directory: $BundleRoot" }
    $files = @(Get-ChildItem -LiteralPath $BundleRoot -File)
    $unexpected = @($files | Where-Object { -not $expectedBundleHashes.Contains($_.Name) })
    if ($files.Count -ne $expectedBundleHashes.Count -or $unexpected.Count -ne 0) {
        throw "Bundle membership mismatch: $($files.Name -join ', ')"
    }
    foreach ($entry in $expectedBundleHashes.GetEnumerator()) {
        $path = Join-Path $BundleRoot $entry.Key
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing bundle file: $path" }
        $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $entry.Value) { throw "Bundle hash mismatch for '$($entry.Key)': expected $($entry.Value), got $actual" }
    }
}

function Get-DriverSnapshot {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
    })
    if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
    $device = $devices[0]
    $driverKeyName = Get-OptionalPnpData -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Driver'
    if ([string]::IsNullOrWhiteSpace([string]$driverKeyName)) { throw 'The virtio-gpu driver key is unavailable.' }
    $driverKey = Get-Item -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$driverKeyName"
    $serviceKey = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm'
    $serviceParameters = Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm\Parameters'
    $imagePath = [Environment]::ExpandEnvironmentVariables([string]$serviceKey.GetValue('ImagePath', '')).Trim('"')
    if ($imagePath.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $imagePath = Join-Path $env:SystemRoot $imagePath.Substring(12)
    }
    $image = Get-Item -LiteralPath $imagePath
    $umd = Get-Item -LiteralPath (Join-Path $image.DirectoryName 'viogpud3d.dll')
    $imageSignature = Get-AuthenticodeSignature -LiteralPath $image.FullName
    $umdSignature = Get-AuthenticodeSignature -LiteralPath $umd.FullName
    $service = Get-CimInstance Win32_SystemDriver -Filter "Name='VioGpuWddm'"
    [pscustomobject]@{
        Status = [string]$device.Status
        ProblemCode = Get-OptionalPnpData -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode'
        ProblemStatus = Get-OptionalPnpData -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemStatus'
        InstanceId = [string]$device.InstanceId
        DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
        InfPath = [string]$driverKey.GetValue('InfPath', '')
        RenderOnly = $driverKey.GetValue('RenderOnly', $null)
        ServiceRenderOnly = $serviceKey.GetValue('RenderOnly', $null)
        ServiceParametersRenderOnly = $serviceParameters.GetValue('RenderOnly', $null)
        ImageSha256 = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        UmdSha256 = (Get-FileHash -LiteralPath $umd.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        ImageSignatureStatus = [string]$imageSignature.Status
        ImageSignerSubject = if ($null -eq $imageSignature.SignerCertificate) { $null } else { $imageSignature.SignerCertificate.Subject }
        UmdSignatureStatus = [string]$umdSignature.Status
        UmdSignerSubject = if ($null -eq $umdSignature.SignerCertificate) { $null } else { $umdSignature.SignerCertificate.Subject }
        ServiceState = [string]$service.State
    }
}

function Assert-ExactDriverState {
    param([Parameter(Mandatory)]$Snapshot)

    if ($Snapshot.Status -ne 'OK' -or
        ($null -ne $Snapshot.ProblemCode -and [int]$Snapshot.ProblemCode -ne 0) -or
        ($null -ne $Snapshot.ProblemStatus -and [int64]$Snapshot.ProblemStatus -ne 0) -or
        $Snapshot.DriverVersion -ne $expectedDriverVersion -or
        $Snapshot.ImageSha256 -ne $expectedDriverImageHash -or
        $Snapshot.UmdSha256 -ne $expectedDriverUmdHash -or
        $null -eq $Snapshot.RenderOnly -or [int]$Snapshot.RenderOnly -ne 1 -or
        $null -ne $Snapshot.ServiceRenderOnly -or
        $null -eq $Snapshot.ServiceParametersRenderOnly -or [int]$Snapshot.ServiceParametersRenderOnly -ne 1 -or
        $Snapshot.ImageSignatureStatus -ne 'Valid' -or
        $Snapshot.ImageSignerSubject -ne $expectedSignerSubject -or
        $Snapshot.UmdSignatureStatus -ne 'Valid' -or
        $Snapshot.UmdSignerSubject -ne $expectedSignerSubject -or
        $Snapshot.ServiceState -ne 'Running') {
        throw "Unexpected virtio-gpu state: $($Snapshot | ConvertTo-Json -Compress)"
    }
}

function Get-DumpSnapshot {
    $files = @()
    if (Test-Path -LiteralPath 'C:\Windows\MEMORY.DMP' -PathType Leaf) {
        $files += Get-Item -LiteralPath 'C:\Windows\MEMORY.DMP'
    }
    if (Test-Path -LiteralPath 'C:\Windows\Minidump' -PathType Container) {
        $files += Get-ChildItem -LiteralPath 'C:\Windows\Minidump' -File -ErrorAction SilentlyContinue
    }
    $snapshot = [ordered]@{}
    foreach ($file in $files) {
        $snapshot[$file.FullName] = [ordered]@{
            Length = $file.Length
            LastWriteTimeUtc = $file.LastWriteTimeUtc.ToString('o')
        }
    }
    return $snapshot
}

function Get-NewOrChangedDumps {
    param(
        [Parameter(Mandatory)]$Before,
        [Parameter(Mandatory)]$After
    )

    @($After.GetEnumerator() | Where-Object {
        -not $Before.Contains($_.Key) -or
        $Before[$_.Key].Length -ne $_.Value.Length -or
        $Before[$_.Key].LastWriteTimeUtc -ne $_.Value.LastWriteTimeUtc
    } | ForEach-Object { $_.Key })
}

Assert-ExactBundle
$driverBefore = Get-DriverSnapshot
Assert-ExactDriverState -Snapshot $driverBefore
if ($ValidateOnly) {
    [ordered]@{
        ValidationOnly = $true
        BundleHashes = $expectedBundleHashes
        Driver = $driverBefore
    } | ConvertTo-Json -Depth 6
    return
}

$dumpsBefore = Get-DumpSnapshot
$startedAt = Get-Date
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = Join-Path $BundleRoot 'zink_d3d11_offscreen.exe'
$startInfo.WorkingDirectory = $BundleRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.EnvironmentVariables['GALLIUM_DRIVER'] = 'zink'
$startInfo.EnvironmentVariables.Remove('LIBGL_ALWAYS_SOFTWARE')
$startInfo.EnvironmentVariables.Remove('D3D_ALWAYS_SOFTWARE')
$process = [Diagnostics.Process]::new()
$process.StartInfo = $startInfo
if (-not $process.Start()) { throw 'Failed to start the Zink offscreen probe.' }
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
$timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
if ($timedOut) {
    $process.Kill()
    $process.WaitForExit()
} else {
    $process.WaitForExit()
}
$stdout = $stdoutTask.Result
$stderr = $stderrTask.Result
$exitCode = $process.ExitCode
$process.Dispose()

$driverAfter = $null
$driverAfterError = $null
try {
    $driverAfter = Get-DriverSnapshot
    Assert-ExactDriverState -Snapshot $driverAfter
} catch {
    $driverAfterError = $_.Exception.Message
}
$dumpsAfter = Get-DumpSnapshot
$dumpChanges = @(Get-NewOrChangedDumps -Before $dumpsBefore -After $dumpsAfter)
$errors = [Collections.Generic.List[string]]::new()
if ($timedOut) { $errors.Add("Probe exceeded ${TimeoutSeconds}s timeout.") }
if ($exitCode -ne 0) { $errors.Add("Probe exited with code $exitCode.") }
if ($stdout -notmatch $successPattern) { $errors.Add('Probe success marker or checksum is missing.') }
if (-not [string]::IsNullOrWhiteSpace($stderr)) { $errors.Add('Probe stderr is not empty.') }
if ($dumpChanges.Count -ne 0) { $errors.Add("New or changed dumps: $($dumpChanges -join ', ')") }
if ($null -ne $driverAfterError) { $errors.Add("Post-probe driver state failed: $driverAfterError") }

New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
$stdout | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'zink-offscreen.stdout.txt') -Encoding UTF8
$stderr | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'zink-offscreen.stderr.txt') -Encoding UTF8
$result = [ordered]@{
    StartedAt = $startedAt.ToString('o')
    CompletedAt = (Get-Date).ToString('o')
    TimedOut = $timedOut
    ExitCode = $exitCode
    Passed = $errors.Count -eq 0
    Errors = @($errors)
    Stdout = $stdout
    Stderr = $stderr
    NewOrChangedDumps = $dumpChanges
    DriverBefore = $driverBefore
    DriverAfter = $driverAfter
    DriverAfterError = $driverAfterError
}
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'zink-offscreen-result.json') -Encoding UTF8
if ($errors.Count -ne 0) { throw "Zink offscreen probe failed: $($errors -join '; ')" }
$result | ConvertTo-Json -Depth 8
