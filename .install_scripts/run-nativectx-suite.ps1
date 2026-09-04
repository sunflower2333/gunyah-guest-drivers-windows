[CmdletBinding()]
param(
    [string]$BundleRoot = 'C:\DroidVM\TurnipRuns\run-33322445949',
    [string]$OutputPath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-Node {
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*'
    })
    if ($devices.Count -ne 1) { throw "Expected one present virtio-gpu device, found $($devices.Count)." }
    return $devices[0]
}

function Invoke-Probe {
    param([string]$Name, [string]$Exe, [string[]]$Arguments)

    $exePath = Join-Path $BundleRoot $Exe
    if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) { throw "Missing probe: $exePath" }

    Push-Location $BundleRoot
    try {
        $lines = @(& $exePath @Arguments 2>&1 | ForEach-Object { $_.ToString() })
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
    }

    $verdict = @($lines | Where-Object { $_ -match 'passed|failed|no adapter' })
    [pscustomobject]@{
        Name = $Name
        Exe = $Exe
        Sha256 = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash.ToLowerInvariant()
        ExitCode = $code
        Passed = ($code -eq 0)
        Verdict = if ($verdict.Count -gt 0) { $verdict[-1] } else { '' }
        LineCount = $lines.Count
    }
}

$startedAt = Get-Date
$before = Get-Node
$driverKeyProperty = Get-PnpDeviceProperty -InstanceId $before.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$driverKey = Get-Item -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($driverKeyProperty.Data)"

$results = @()
$results += Invoke-Probe -Name 'kmt-lifecycle' -Exe 'tu_wddm_kmt_probe_arm64.exe' -Arguments @()
$results += Invoke-Probe -Name 'kmt-submit-nop' -Exe 'tu_wddm_kmt_probe_arm64.exe' -Arguments @('--submit-nop')
$results += Invoke-Probe -Name 'vulkan-lifecycle' -Exe 'tu_wddm_vulkan_probe_arm64.exe' -Arguments @()
$results += Invoke-Probe -Name 'vulkan-compute' -Exe 'tu_wddm_vulkan_compute_probe_arm64.exe' -Arguments @('tu_wddm_compute.spv')
$results += Invoke-Probe -Name 'vulkan-graphics' -Exe 'tu_wddm_vulkan_graphics_probe_arm64.exe' -Arguments @('tu_wddm_graphics.vert.spv', 'tu_wddm_graphics.frag.spv')
$results += Invoke-Probe -Name 'kmt-post-health' -Exe 'tu_wddm_kmt_probe_arm64.exe' -Arguments @()

$after = Get-Node
$summary = [ordered]@{
    StartedAt = $startedAt.ToString('o')
    CompletedAt = (Get-Date).ToString('o')
    ComputerName = $env:COMPUTERNAME
    BundleRoot = $BundleRoot
    DriverVersion = [string]$driverKey.GetValue('DriverVersion', '')
    StatusBefore = [string]$before.Status
    StatusAfter = [string]$after.Status
    ProblemAfter = [string]$after.Problem
    ServiceState = [string](Get-Service VioGpuWddm).Status
    TotalProbes = $results.Count
    PassedProbes = @($results | Where-Object { $_.Passed }).Count
    AllPassed = (@($results | Where-Object { -not $_.Passed }).Count -eq 0)
    Results = $results
}

if ($OutputPath -ne '') {
    $summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
}
$summary | ConvertTo-Json -Depth 6
