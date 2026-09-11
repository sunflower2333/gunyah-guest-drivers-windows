# SPDX-License-Identifier: MIT
# Uses a unique HKCU fixture only; never opens a display adapter registry key.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module "$PSScriptRoot/../../.install_scripts/opengl-registration.psm1" -Force
$path = 'Software\DroidVM-IcdTest-' + [guid]::NewGuid().ToString('N')
$base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
$key = $base.CreateSubKey($path)
try {
    $key.SetValue('OpenGLDriverName','original-native.dll',[Microsoft.Win32.RegistryValueKind]::String)
    $key.SetValue('OpenGLFlags',7,[Microsoft.Win32.RegistryValueKind]::DWord)
    $key.SetValue('VulkanDriverName',[string[]]@('one.json','two.json'),[Microsoft.Win32.RegistryValueKind]::MultiString)
    $previous = @(Get-IcdSnapshot $key)
    $desired = @(Get-IcdRegistration 'C:\Program Files\DroidVM\OpenGL\test\payload')
    Invoke-IcdRegistration $key $desired
    if (!(Test-IcdSnapshot $key $desired)) { throw 'Install roundtrip failed' }
    Invoke-IcdRegistration $key $previous
    if (!(Test-IcdSnapshot $key $previous)) { throw 'Restore failed to retain absent values, types, and MULTI_SZ content' }
    # Force a write failure after three successful values. The production
    # transaction must restore the original native registration and types.
    $bad = @(Get-IcdRegistration 'C:\partial')
    $bad[3].Kind = 'DWord'; $bad[3].Value = 'invalid numeric value'
    $failed = $false
    try { Invoke-IcdRegistration $key $bad } catch { $failed = $true }
    if (!$failed -or !(Test-IcdSnapshot $key $previous)) { throw 'Partial-write failure was not rolled back' }
    $key.SetValue('OpenGLFlags',99,[Microsoft.Win32.RegistryValueKind]::DWord)
    if (Test-IcdSnapshot $key $previous) { throw 'Concurrent registration change was not detected' }
    Write-Output 'PASS install, typed restoration, partial-write rollback, and changed-state detection'
} finally {
    $key.Dispose()
    $base.DeleteSubKeyTree($path)
    $base.Dispose()
}
