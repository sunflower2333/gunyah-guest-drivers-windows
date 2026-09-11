# SPDX-License-Identifier: MIT
# Ephemeral CI-only certificate and copied payload. No adapter/device changes.
param([Parameter(Mandatory)][string]$Payload)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$fixture = Join-Path $env:RUNNER_TEMP ('opengl-sign-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$cert = $null
$trusted = $null
try {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=DroidVM OpenGL CI fixture' -CertStoreLocation Cert:\CurrentUser\My
    $pfx = Join-Path $fixture 'fixture.pfx'
    $password = 'ci-fixture-only'
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password (ConvertTo-SecureString $password -AsPlainText -Force) | Out-Null
    $cer = Join-Path $fixture 'fixture.cer'
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    $trusted = Import-Certificate -FilePath $cer -CertStoreLocation Cert:\CurrentUser\Root
    $tool = Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/bin" -Recurse -Filter signtool.exe |
        Where-Object FullName -Match '\\x64\\signtool\.exe$' | Sort-Object FullName -Descending | Select-Object -First 1
    if (!$tool) { throw 'SDK signtool not found for the signed package regression' }
    $output = Join-Path $fixture 'signed'
    # The fixture binds two ordinary proxy PEs only to exercise catalog creation;
    # it is never uploaded as a driver bundle. The combined job uses real KMD/UMD.
    & "$PSScriptRoot/../../.github/scripts/sign-opengl-sidecar.ps1" -Payload $Payload -Output $output `
        -Kmd (Join-Path $Payload 'viogpuopengl_arm64.dll') -Umd (Join-Path $Payload 'viogpuopengl_x64.dll') `
        -Pfx $pfx -PfxPassword $password -SignTool $tool.FullName
    & "$PSScriptRoot/../../.install_scripts/install-opengl-icd.ps1" -Action Verify -PackageRoot $output
    # A modified manifest must fail before any adapter operation could occur.
    Add-Content (Join-Path $output 'payload/turnip.json') 'tampered'
    $rejected = $false
    try { & "$PSScriptRoot/../../.install_scripts/install-opengl-icd.ps1" -Action Verify -PackageRoot $output } catch { $rejected = $true }
    if (!$rejected) { throw 'Tampered signed sidecar was accepted' }
    Write-Output 'PASS signed full-tree catalog verification and tamper rejection'
} finally {
    if ($trusted) { Remove-Item -LiteralPath "Cert:\CurrentUser\Root\$($trusted.Thumbprint)" }
    if ($cert) { Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($cert.Thumbprint)" }
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
