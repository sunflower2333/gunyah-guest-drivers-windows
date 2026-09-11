# SPDX-License-Identifier: MIT
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Names = @('OpenGLDriverName','OpenGLVersion','OpenGLFlags',
    'OpenGLDriverNameWow','OpenGLVersionWow','OpenGLFlagsWow',
    'VulkanDriverName','VulkanDriverNameWow')

function Get-IcdRegistration([string]$Directory) {
    $values = @{
        OpenGLDriverName = (Join-Path $Directory 'viogpuopengl.dll')
        OpenGLVersion = 1; OpenGLFlags = 1
        OpenGLDriverNameWow = (Join-Path $Directory 'viogpuopengl_x86.dll')
        OpenGLVersionWow = 1; OpenGLFlagsWow = 1
        VulkanDriverName = (Join-Path $Directory 'turnip.json')
        VulkanDriverNameWow = (Join-Path $Directory 'turnip-wow.json')
    }
    @($Names | ForEach-Object {
        [pscustomobject]@{ Name = $_; Present = $true
            Kind = $(if ($values[$_] -is [int]) {'DWord'} else {'String'})
            Value = $values[$_] }
    })
}

function Get-IcdSnapshot([Microsoft.Win32.RegistryKey]$Key) {
    $present = @($Key.GetValueNames())
    @($Names | ForEach-Object {
        $exists = $present -contains $_
        $kind = $null
        $value = $null
        if ($exists) {
            $kind = $Key.GetValueKind($_).ToString()
            # Do not wrap the method in a pipeline/subexpression: that would
            # enumerate an empty REG_MULTI_SZ or REG_BINARY into $null.
            $value = $Key.GetValue($_, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        }
        [pscustomobject]@{ Name = $_; Present = $exists
            Kind = $kind; Value = $value }
    })
}

function Assert-IcdSnapshot($Snapshot) {
    $items = @($Snapshot)
    if ($items.Count -ne $Names.Count -or (Compare-Object $Names @($items | ForEach-Object Name))) {
        throw 'ICD registry snapshot must contain exactly the eight supported values'
    }
    foreach ($item in $items) {
        if ($item.Present -and $item.Kind -notin @('String','ExpandString','Binary','DWord','MultiString','QWord','None')) {
            throw "Invalid saved registry kind: $($item.Kind)"
        }
    }
}

function Set-IcdSnapshot([Microsoft.Win32.RegistryKey]$Key, $Snapshot) {
    Assert-IcdSnapshot $Snapshot
    foreach ($item in $Snapshot) {
        if ($item.Present) {
            $kind = [Microsoft.Win32.RegistryValueKind]$item.Kind
            $value = $item.Value
            if ($kind -eq [Microsoft.Win32.RegistryValueKind]::MultiString) { $value = [string[]]$value }
            if ($kind -eq [Microsoft.Win32.RegistryValueKind]::Binary) { $value = [byte[]]$value }
            if ($kind -eq [Microsoft.Win32.RegistryValueKind]::DWord) { $value = [int]$value }
            if ($kind -eq [Microsoft.Win32.RegistryValueKind]::QWord) { $value = [long]$value }
            $Key.SetValue($item.Name, $value, $kind)
        } else {
            $Key.DeleteValue($item.Name, $false)
        }
    }
    $Key.Flush()
}

function Test-IcdSnapshot([Microsoft.Win32.RegistryKey]$Key, $Snapshot) {
    Assert-IcdSnapshot $Snapshot
    $current = Get-IcdSnapshot $Key
    foreach ($expected in $Snapshot) {
        $actual = @($current | Where-Object Name -eq $expected.Name)[0]
        if ($actual.Present -ne $expected.Present -or $actual.Kind -ne $expected.Kind -or
            (ConvertTo-Json -InputObject $actual.Value -Compress) -cne (ConvertTo-Json -InputObject $expected.Value -Compress)) { return $false }
    }
    return $true
}

function Invoke-IcdRegistration([Microsoft.Win32.RegistryKey]$Key, $Desired) {
    $previous = Get-IcdSnapshot $Key
    try {
        Set-IcdSnapshot $Key $Desired
        if (!(Test-IcdSnapshot $Key $Desired)) { throw 'ICD registration readback mismatch' }
    } catch {
        Set-IcdSnapshot $Key $previous
        if (!(Test-IcdSnapshot $Key $previous)) { throw 'ICD registration failed and rollback verification failed' }
        throw
    }
}
Export-ModuleMember -Function Get-IcdRegistration,Get-IcdSnapshot,Set-IcdSnapshot,Test-IcdSnapshot,Invoke-IcdRegistration
