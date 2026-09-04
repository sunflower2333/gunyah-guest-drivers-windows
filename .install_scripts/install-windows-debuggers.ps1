[CmdletBinding()]
param(
    [string]$InstallerUrl = 'https://go.microsoft.com/fwlink/?linkid=2376216'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$toolsDirectory = 'C:\DroidVM\debug-tools'
$installerPath = Join-Path $toolsDirectory 'winsdksetup.exe'
New-Item -ItemType Directory -Path $toolsDirectory -Force | Out-Null

Invoke-WebRequest -Uri $InstallerUrl -OutFile $installerPath -UseBasicParsing
$signature = Get-AuthenticodeSignature -LiteralPath $installerPath
if ($signature.Status -ne 'Valid') {
    throw "Windows SDK installer signature is $($signature.Status), expected Valid."
}

$arguments = @(
    '/features',
    'OptionId.WindowsDesktopDebuggers',
    '/quiet',
    '/norestart',
    '/ceip',
    'off'
)
$process = Start-Process -FilePath $installerPath -ArgumentList $arguments -Wait -PassThru
if ($process.ExitCode -notin @(0, 3010)) {
    throw "Windows SDK installer exited with code $($process.ExitCode)."
}

$debuggerRoots = @(
    'C:\Program Files (x86)\Windows Kits\10\Debuggers',
    'C:\Program Files\Windows Kits\10\Debuggers'
)
$debuggers = @(
    foreach ($root in $debuggerRoots) {
        if (Test-Path -LiteralPath $root -PathType Container) {
            Get-ChildItem -LiteralPath $root -File -Recurse -ErrorAction Stop |
                Where-Object { $_.Name -in @('kd.exe', 'cdb.exe', 'dumpchk.exe') } |
                Select-Object FullName, Length, VersionInfo
        }
    }
)
if ($debuggers.Count -eq 0) {
    throw 'Debugger installation completed but no command-line debugger was found.'
}

[pscustomobject]@{
    InstalledAt = (Get-Date).ToString('o')
    InstallerSha256 = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash
    InstallerSigner = $signature.SignerCertificate.Subject
    InstallerExitCode = $process.ExitCode
    Debuggers = $debuggers
} | ConvertTo-Json -Depth 5
