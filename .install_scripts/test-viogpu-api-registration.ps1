# SPDX-License-Identifier: MIT
[CmdletBinding()]
param([switch]$RegistryFixture)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'viogpu-api-registration.psm1') -Force

$script:checks = 0
$script:store = [IO.Path]::Combine([IO.Path]::GetTempPath(), 'viogpu-api-contract', 'active-package')
$script:key = 'SOFTWARE\DroidVM\GpuInstallerCi\' + [Guid]::NewGuid().ToString('N')
$script:registry = $null
$script:ownsKey = $false
$script:manifest = [pscustomobject]@{registration=[pscustomobject][ordered]@{
    OpenGLDriverName='viogpuopengl.dll'; OpenGLDriverNameWow='viogpuopengl_x86.dll'
    VulkanDriverName='turnip.json'; VulkanDriverNameWow='turnip-wow.json'
    OpenCLDriverName='viogpucl.dll'; OpenCLDriverNameWow='viogpucl_x86.dll'
}}

function New-Values {
    # Fixture expectations come from the signed package contract, independently
    # of Get-VioGpuApiRegistrationContract. Do not generate from production data.
    $gl = [IO.Path]::Combine($script:store, 'viogpuopengl.dll')
    $wow = [IO.Path]::Combine($script:store, 'viogpuopengl_x86.dll')
    @(
        @{Name='OpenGLDriverName';Kind='MultiString';Value=[string[]]@($gl)}
        @{Name='OpenGLDriverNameWow';Kind='MultiString';Value=[string[]]@($wow)}
        @{Name='VulkanDriverName';Kind='String';Value=[IO.Path]::Combine($script:store,'turnip.json')}
        @{Name='VulkanDriverNameWow';Kind='String';Value=[IO.Path]::Combine($script:store,'turnip-wow.json')}
        @{Name='OpenCLDriverName';Kind='String';Value=[IO.Path]::Combine($script:store,'viogpucl.dll')}
        @{Name='OpenCLDriverNameWow';Kind='String';Value=[IO.Path]::Combine($script:store,'viogpucl_x86.dll')}
        @{Name='OpenGLVersion';Kind='DWord';Value=1}
        @{Name='OpenGLFlags';Kind='DWord';Value=1}
        @{Name='OpenGLVersionWow';Kind='DWord';Value=1}
        @{Name='OpenGLFlagsWow';Kind='DWord';Value=1}
    ) | ForEach-Object {
        [pscustomobject]@{Hive='LocalMachine';View='Registry64';Key=$script:key;
            Name=$_.Name;Present=$true;Kind=$_.Kind;Value=$_.Value}
    }
}

function Convert-ToRegistrySnapshot($Values) {
    if (!$RegistryFixture) { return $Values }
    $key = $script:registry.OpenSubKey($script:key, $true)
    try {
        foreach ($name in $key.GetValueNames()) { $key.DeleteValue($name) }
        foreach ($entry in $Values) {
            if (!$entry.Present) { continue }
            $value = $entry.Value
            if ($entry.Kind -eq 'MultiString') { $value = [string[]]$value }
            $key.SetValue($entry.Name, $value, [Microsoft.Win32.RegistryValueKind]$entry.Kind)
        }
        $key.Flush()
        foreach ($entry in $Values) {
            $present = @($key.GetValueNames()) -contains $entry.Name
            [pscustomobject]@{Hive=$entry.Hive;View=$entry.View;Key=$entry.Key;Name=$entry.Name;
                Present=$present;Kind=$(if ($present) {$key.GetValueKind($entry.Name).ToString()} else {$null});
                Value=$(if ($present) {,$key.GetValue($entry.Name,$null,
                    [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)} else {$null})}
        }
    } finally { $key.Dispose() }
}

function Check([string]$Name, $Values, [bool]$Accept, $Manifest=$script:manifest, [switch]$Raw) {
    $snapshot = if ($Raw) { @($Values) } else { @(Convert-ToRegistrySnapshot $Values) }
    $failure = $null
    try {
        Assert-VioGpuApiRegistration -Manifest $Manifest -StoreRoot $script:store -DriverKey $script:key -Snapshot $snapshot
    } catch { $failure = $_ }
    if ($Accept -and $failure) { throw "$Name unexpectedly failed: $failure" }
    if (!$Accept -and !$failure) { throw "$Name unexpectedly accepted invalid API registration" }
    $script:checks++
    Write-Output "PASS $Name"
}

try {
    if ($RegistryFixture) {
        if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Registry fixture requires Windows' }
        $script:registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
        if ($script:registry.OpenSubKey($script:key)) { throw 'Unexpected existing fixture key' }
        $fixture = $script:registry.CreateSubKey($script:key)
        $script:ownsKey = $true
        $fixture.Dispose()
    }
    Check 'complete native and Wow registrations' (New-Values) $true
    $values = @(New-Values)
    $values[0].Name = 'opengldrivername'
    $values[2].Value = $values[2].Value.ToUpperInvariant()
    Check 'Windows case-insensitive name and path identity' $values $true

    foreach ($name in @('OpenGLVersion','OpenGLFlags','OpenGLVersionWow','OpenGLFlagsWow')) {
        $values = @(New-Values)
        ($values | Where-Object Name -eq $name).Present = $false
        Check "reject missing $name" $values $false
        $values = @(New-Values)
        ($values | Where-Object Name -eq $name).Value = 0
        Check "reject invalid $name" $values $false
        $values = @(New-Values)
        $entry = $values | Where-Object Name -eq $name
        $entry.Kind = 'String'; $entry.Value = '1'
        Check "reject REG_SZ $name" $values $false
    }
    foreach ($name in @('OpenGLDriverName','OpenGLDriverNameWow','VulkanDriverName',
        'VulkanDriverNameWow','OpenCLDriverName','OpenCLDriverNameWow')) {
        $values = @(New-Values)
        $entry = $values | Where-Object Name -eq $name
        $entry.Kind = 'ExpandString'; $entry.Value = [string]@($entry.Value)[0]
        Check "reject REG_EXPAND_SZ $name" $values $false
    }
    $values = @(New-Values); $values[0].Value += 'C:\other\x64.dll'
    Check 'reject multiple native ICD paths' $values $false
    $values = @(New-Values); $values[4].Value = [IO.Path]::Combine([IO.Path]::GetTempPath(),'old','viogpucl.dll')
    Check 'reject another DriverStore package' $values $false
    $values = @(New-Values); $values[0].Key += '\another'
    Check 'reject another adapter key' $values $false
    $values = @(New-Values); $values[0].View = 'Registry32'
    Check 'reject wrong registry view' $values $false
    $values = @(New-Values); $values[0].Hive = 'CurrentUser'
    Check 'reject wrong registry hive' $values $false
    Check 'reject missing snapshot value' @((New-Values) | Select-Object -Skip 1) $false
    $values = @(New-Values); $values[9] = $values[0]
    Check 'reject duplicate snapshot value' $values $false -Raw

    $invalid = $script:manifest | ConvertTo-Json | ConvertFrom-Json
    $invalid.registration.OpenCLDriverName = 'viogpucl_x64.dll'
    Check 'reject single-ABI replacement for ARM64X ICD' (New-Values) $false $invalid
    $invalid.registration.OpenCLDriverName = '..\viogpucl.dll'
    Check 'reject non-flat manifest path' (New-Values) $false $invalid
    $invalid.registration.PSObject.Properties.Remove('OpenCLDriverNameWow')
    Check 'reject incomplete manifest registration' (New-Values) $false $invalid

    $snapshot = @(Convert-ToRegistrySnapshot (New-Values))
    $serialized = [Management.Automation.PSSerializer]::Serialize($snapshot, 8)
    $restored = [Management.Automation.PSSerializer]::Deserialize($serialized)
    Check 'typed journal roundtrip preserves valid registration' $restored $true -Raw

    Write-Output "PASS $script:checks API registration checks; RealWindowsRegistry=$RegistryFixture; GPU rendering not tested"
} finally {
    if ($script:registry) {
        if ($script:ownsKey -and $script:key -match '^SOFTWARE\\DroidVM\\GpuInstallerCi\\[a-f0-9]{32}$') {
            $script:registry.DeleteSubKeyTree($script:key, $false)
        }
        $script:registry.Dispose()
    }
}
