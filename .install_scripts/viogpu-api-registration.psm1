# SPDX-License-Identifier: MIT
Set-StrictMode -Version Latest

# Mirrors the flat display package's HKR contract. The WDDM 2.1 DriverStore
# model uses REG_MULTI_SZ for OpenGL ICD paths; Vulkan/OpenCL use REG_SZ.
# GL version/flags are part of discovery and must be checked as well as names.
function Get-VioGpuApiRegistrationContract {
    [ordered]@{
        OpenGLDriverName    = @{Kind='MultiString'; File='viogpuopengl.dll'}
        OpenGLDriverNameWow = @{Kind='MultiString'; File='viogpuopengl_x86.dll'}
        VulkanDriverName   = @{Kind='String'; File='turnip.json'}
        VulkanDriverNameWow = @{Kind='String'; File='turnip-wow.json'}
        OpenCLDriverName    = @{Kind='String'; File='viogpucl.dll'}
        OpenCLDriverNameWow = @{Kind='String'; File='viogpucl_x86.dll'}
        OpenGLVersion      = @{Kind='DWord'; Value=1}
        OpenGLFlags        = @{Kind='DWord'; Value=1}
        OpenGLVersionWow   = @{Kind='DWord'; Value=1}
        OpenGLFlagsWow     = @{Kind='DWord'; Value=1}
    }
}

function Assert-VioGpuApiRegistration {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)]$Manifest,
        [Parameter(Mandatory=$true)][string]$StoreRoot,
        [Parameter(Mandatory=$true)][string]$DriverKey,
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][object[]]$Snapshot
    )
    if (![IO.Path]::IsPathRooted($StoreRoot)) { throw 'DriverStore root must be absolute' }
    $contract = Get-VioGpuApiRegistrationContract
    $registrations = @($Manifest.registration.PSObject.Properties)
    $paths = @($contract.Keys | Where-Object { $contract[$_].ContainsKey('File') })
    if ($registrations.Count -ne $paths.Count) { throw 'Incomplete or extra API registration manifest entries' }
    $seenManifest = @{}
    foreach ($entry in $registrations) {
        if ($entry.Name -cnotin $paths -or $seenManifest.ContainsKey($entry.Name) -or
            $entry.Value -isnot [string] -or $entry.Value -cne $contract[$entry.Name].File) {
            throw "Unexpected API registration manifest entry: $($entry.Name)"
        }
        $seenManifest[$entry.Name] = $true
    }
    if ($Snapshot.Count -ne $contract.Count) { throw 'Incomplete or extra adapter API snapshot entries' }
    $actual = @{}
    foreach ($entry in $Snapshot) {
        if ($entry.Name -notin $contract.Keys -or $actual.ContainsKey($entry.Name)) {
            throw "Duplicate or unexpected adapter API value: $($entry.Name)"
        }
        if ($entry.Hive -cne 'LocalMachine' -or $entry.View -cne 'Registry64' -or
            ![string]::Equals($entry.Key, $DriverKey, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Adapter API snapshot belongs to another registry location: $($entry.Name)"
        }
        $actual[$entry.Name] = $entry
    }
    foreach ($name in $contract.Keys) {
        $expected = $contract[$name]
        $entry = $actual[$name]
        if ($entry.Present -isnot [bool] -or !$entry.Present -or $entry.Kind -cne $expected.Kind) {
            throw "Missing or incorrectly typed adapter API value: $name"
        }
        if ($expected.Kind -eq 'DWord') {
            if ($entry.Value -isnot [int] -or $entry.Value -ne $expected.Value) {
                throw "Incorrect adapter API version/flags: $name"
            }
            continue
        }
        $value = $entry.Value
        if ($expected.Kind -eq 'MultiString') {
            # Native registry reads return string[]; CLIXML journal recovery
            # returns ArrayList. Both retain the same ordered multi-string data.
            if ($value -isnot [Collections.IList] -or $value.Count -ne 1 -or $value[0] -isnot [string]) {
                throw "Expected one ICD path in REG_MULTI_SZ: $name"
            }
            $value = $value[0]
        } elseif ($value -isnot [string]) {
            throw "Expected a REG_SZ ICD path: $name"
        }
        $path = [IO.Path]::Combine($StoreRoot, $expected.File)
        if (![string]::Equals($value, $path, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Adapter API path does not select the active DriverStore package: $name"
        }
    }
}

Export-ModuleMember -Function Get-VioGpuApiRegistrationContract,Assert-VioGpuApiRegistration
