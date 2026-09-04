[CmdletBinding()]
param([string]$OutputDirectory = 'C:\DroidVM\TurnipRuns\final-report')
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$out = Join-Path $OutputDirectory 'report.txt'
$lines = New-Object System.Collections.Generic.List[string]

function Add-Line { param([string]$s) $lines.Add($s) }

Add-Line "COLLECTED=$((Get-Date).ToString('o'))"
Add-Line "COMPUTER=$env:COMPUTERNAME"
$os = Get-CimInstance Win32_OperatingSystem
Add-Line "OS=$($os.Caption) $($os.Version) ARM64"
Add-Line ""

Add-Line "== GPU DEVICE =="
$dev = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })[0]
Add-Line "FriendlyName=$($dev.FriendlyName)"
Add-Line "Status=$($dev.Status) Problem=$($dev.Problem)"
Add-Line "InstanceId=$($dev.InstanceId)"
$p = Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$k = Get-Item -LiteralPath "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($p.Data)"
Add-Line "DriverVersion=$($k.GetValue('DriverVersion'))"
Add-Line "RenderOnly(device)=$($k.GetValue('RenderOnly','<unset>'))"
$svc = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Services\VioGpuWddm\Parameters' -Name RenderOnly -ErrorAction SilentlyContinue
Add-Line "RenderOnly(service)=$($svc.RenderOnly)"
Add-Line "Service=$((Get-Service VioGpuWddm).Status)"
$img = [Environment]::ExpandEnvironmentVariables((Get-Item 'Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\VioGpuWddm').GetValue('ImagePath')).Trim('"')
if ($img.StartsWith('\SystemRoot\')) { $img = Join-Path $env:SystemRoot $img.Substring(12) }
Add-Line "SysSha256=$((Get-FileHash $img -Algorithm SHA256).Hash.ToLower())"
Add-Line ""

Add-Line "== VULKAN (system loader) =="
$vi = & 'C:\DroidVM\VulkanTools\vulkaninfo.exe' --summary 2>&1
foreach ($l in $vi) {
    $t = $l.ToString()
    if ($t -match 'deviceName|driverID|driverName|apiVersion|deviceType|Vulkan Instance Version|vendorID|deviceID|driverVersion') {
        Add-Line ("  " + $t.Trim())
    }
}
$icd = 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
if (Test-Path $icd) {
    (Get-Item $icd).GetValueNames() | ForEach-Object { Add-Line "  ICD: $_ = $(Get-ItemPropertyValue $icd $_)" }
}
Add-Line "  vulkaninfo.sha256=$((Get-FileHash 'C:\DroidVM\VulkanTools\vulkaninfo.exe' -Algorithm SHA256).Hash.ToLower())"
Add-Line "  vkcube.sha256=$((Get-FileHash 'C:\DroidVM\VulkanTools\vkcube.exe' -Algorithm SHA256).Hash.ToLower())"
Add-Line ""

Add-Line "== VKCUBE (last run) =="
$vk = 'C:\DroidVM\TurnipRuns\evidence-vkcube\vkcube.stdout.txt'
if (Test-Path $vk) { Get-Content $vk | ForEach-Object { Add-Line ("  " + $_) } }
$vke = 'C:\DroidVM\TurnipRuns\evidence-vkcube\vkcube.exit.txt'
if (Test-Path $vke) { Add-Line "  EXITCODE=$((Get-Content $vke -Raw).Trim())" }
Add-Line ""

Add-Line "== NATIVE CONTEXT SUITE (last run) =="
$s = 'C:\DroidVM\TurnipRuns\evidence-restored.json'
if (Test-Path $s) {
    $j = Get-Content $s -Raw | ConvertFrom-Json
    Add-Line "  AllPassed=$($j.AllPassed) $($j.PassedProbes)/$($j.TotalProbes) Driver=$($j.DriverVersion)"
    $j.Results | ForEach-Object { Add-Line "  $($_.Name): $($_.Verdict)" }
}
Add-Line ""

Add-Line "== DIRECTX (dxdiag) =="
$dx = Join-Path $OutputDirectory 'dxdiag.txt'
Remove-Item $dx -ErrorAction SilentlyContinue
Start-Process dxdiag.exe -ArgumentList @('/whql:off','/t',$dx) -Wait -NoNewWindow
if (Test-Path $dx) {
    $keep = 'Card name|Device Type|Display Memory|Driver Version|DDI Version|Feature Levels|Driver Model|Current Mode|DirectX Version'
    Select-String -Path $dx -Pattern $keep | ForEach-Object { Add-Line ("  " + $_.Line.Trim()) }
}

$lines | Set-Content -LiteralPath $out -Encoding UTF8
Write-Output "WROTE=$out LINES=$($lines.Count)"
