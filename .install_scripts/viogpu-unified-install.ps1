# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [ValidateSet('Install','Verify','Rollback','Uninstall')][string]$Action = 'Install',
    [string]$PackageRoot = (Join-Path $PSScriptRoot 'drivers/viogpu'),
    [string]$InstanceId,
    [string]$JournalPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'viogpu-install-state.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'viogpu-api-registration.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'viogpu-install-certificates.psm1') -Force
if (!('DroidVmGpuInstall.Native' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'viogpu-install-native.cs') }

function Assert-NativeAdministrator {
    if (![Environment]::Is64BitProcess -or $env:PROCESSOR_ARCHITECTURE -ne 'ARM64') {
        throw 'Run the unified GPU installer in native ARM64 PowerShell'
    }
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrator required' }
}

function Assert-FlatName([string]$Name) {
    if (!$Name -or $Name -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]+$' -or
        [IO.Path]::GetFileName($Name) -cne $Name) { throw "Invalid flat filename: $Name" }
}

function Read-FlatPackage([string]$Root) {
    $Root = (Resolve-Path -LiteralPath $Root).Path
    Assert-GpuRegularPath $Root
    $manifestPath = Join-Path $Root 'viogpu-flat-package.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $manifest.phase -ne 'signed-files' -or
        $manifest.inf -cne 'viogpuwddm.inf' -or $manifest.cat -cne 'viogpuwddm.cat' -or
        @($manifest.hardware_ids).Count -ne 1 -or
        $manifest.hardware_ids[0] -cne 'PCI\VEN_1AF4&DEV_1050') { throw 'Invalid or unsigned flat package manifest' }
    $cat = Join-Path $Root $manifest.cat
    Assert-GpuRegularPath $cat
    [DroidVmGpuInstall.Native]::VerifyCatalogMember($cat, $manifestPath)
    $names = @($manifest.files.PSObject.Properties.Name)
    foreach ($required in @('viogpuwddm.inf','viogpuwddm.sys','viogpud3d.dll','OpenCL.dll','OpenCL32.dll')) {
        if ($required -cnotin $names) { throw "Manifest lacks $required" }
    }
    foreach ($entry in $manifest.files.PSObject.Properties) {
        Assert-FlatName $entry.Name
        if ($entry.Name -in @($manifest.cat,'viogpu-flat-package.json') -or
            $entry.Value -notmatch '^[a-f0-9]{64}$') { throw 'Invalid manifest inventory' }
        $file = Join-Path $Root $entry.Name
        Assert-GpuRegularPath $file
        if ((Get-GpuHash $file) -cne $entry.Value) { throw "Package hash mismatch: $($entry.Name)" }
        [DroidVmGpuInstall.Native]::VerifyCatalogMember($cat, $file)
    }
    $expectedLoaders = @(@('OpenCL.dll','System32','arm64x'), @('OpenCL32.dll','SysWOW64','x86'))
    if (@($manifest.system_loaders).Count -ne 2) { throw 'Expected two public OpenCL loader sources' }
    foreach ($loader in $manifest.system_loaders) {
        $matched = 0
        foreach ($expectedLoader in $expectedLoaders) {
            if ($expectedLoader[0] -ceq $loader.source -and $expectedLoader[1] -ceq $loader.system_directory -and
                $expectedLoader[2] -ceq $loader.arch) { $matched++ }
        }
        if ($matched -ne 1 -or $loader.name -cne 'OpenCL.dll' -or
            $loader.sha256 -cne $manifest.files.($loader.source)) { throw 'Invalid public loader mapping' }
    }
    if (@($manifest.system_loaders.source | Select-Object -Unique).Count -ne 2) { throw 'Duplicate loader mapping' }
    foreach ($arch in @('arm64','x64','x86')) {
        $probe = $manifest.loader_probes.$arch
        Assert-FlatName $probe
        if ($probe -cnotin $names) { throw "Unhashed $arch public loader probe" }
    }
    $allowedNames = @('OpenGLDriverName','OpenGLDriverNameWow','VulkanDriverName','VulkanDriverNameWow',
        'OpenCLDriverName','OpenCLDriverNameWow')
    foreach ($entry in $manifest.registration.PSObject.Properties) {
        if ($entry.Name -cnotin $allowedNames -or $entry.Value -cnotin $names) { throw 'Invalid adapter registration inventory' }
    }
    if (@($manifest.registration.PSObject.Properties.Name).Count -ne $allowedNames.Count) {
        throw 'Incomplete adapter API registration map'
    }
    return [pscustomobject]@{Root=$Root; Manifest=$manifest; ManifestHash=(Get-GpuHash $manifestPath);
        Inf=(Join-Path $Root $manifest.inf); InfHash=$manifest.files.($manifest.inf)}
}

function Get-Adapter([string]$Id) {
    $devices = @(Get-PnpDevice -PresentOnly -Class Display | Where-Object {
        $_.InstanceId -match '^PCI\\VEN_1AF4&DEV_1050(?:&|\\)' -and (!$Id -or $_.InstanceId -eq $Id)
    })
    if ($devices.Count -ne 1) { throw 'Exactly one matching present VIOGPU display adapter is required' }
    $device = $devices[0]
    $properties = @{}
    foreach ($name in @('DriverInfPath','DriverVersion','Driver','Service','ProblemCode')) {
        $properties[$name] = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName "DEVPKEY_Device_$name").Data
    }
    if ($properties.DriverInfPath -notmatch '^[A-Za-z0-9_.-]+\.inf$' -or
        $properties.Driver -notmatch '^\{4d36e968-e325-11ce-bfc1-08002be10318\}\\\d{4}$') {
        throw 'Unexpected active display driver identity'
    }
    $inf = Join-Path "$env:SystemRoot/INF" $properties.DriverInfPath
    $store = [DroidVmGpuInstall.Native]::StoreInf($inf)
    Assert-GpuRegularPath $store
    [pscustomobject]@{InstanceId=$device.InstanceId; PublishedInf=$inf; StoreInf=$store;
        InfHash=(Get-GpuHash $store); Version=[string]$properties.DriverVersion;
        DriverKey="SYSTEM\CurrentControlSet\Control\Class\$($properties.Driver)";
        Service=[string]$properties.Service; Status=[string]$device.Status; ProblemCode=[uint32]$properties.ProblemCode}
}

function Get-AdapterApiEntries([string]$Key) {
    foreach ($name in @('OpenGLDriverName','OpenGLVersion','OpenGLFlags','OpenGLDriverNameWow',
        'OpenGLVersionWow','OpenGLFlagsWow','VulkanDriverName','VulkanDriverNameWow',
        'OpenCLDriverName','OpenCLDriverNameWow')) {
        [pscustomobject]@{Hive='LocalMachine';View='Registry64';Key=$Key;Name=$name}
    }
}

function Test-AdapterIdentity($Expected) {
    $actual = Get-Adapter $Expected.InstanceId
    $actual.InfHash -ceq $Expected.InfHash -and $actual.Version -ceq $Expected.Version -and
        $actual.Service -ceq $Expected.Service
}

function Assert-CandidateBinding($State) {
    $actual = Get-Adapter $State.InstanceId
    if ($actual.InfHash -cne $State.CandidateInfHash -or $actual.Version -cne $State.Version -or
        $actual.Service -ine 'VioGpuWddm') { throw 'Driver staged but adapter did not bind the exact candidate' }
    if ($actual.Status -cne 'OK' -or $actual.ProblemCode -ne 0) {
        throw "Candidate adapter not healthy: Status=$($actual.Status) ProblemCode=$($actual.ProblemCode)"
    }
    $store = Split-Path -Parent $actual.StoreInf
    foreach ($entry in $State.Manifest.files.PSObject.Properties) {
        if ((Get-GpuHash (Join-Path $store $entry.Name)) -cne $entry.Value) { throw "DriverStore file mismatch: $($entry.Name)" }
    }
    $snap = @(Get-GpuRegistrySnapshot (Get-AdapterApiEntries $actual.DriverKey))
    Assert-VioGpuApiRegistration -Manifest $State.Manifest -StoreRoot $store -DriverKey $actual.DriverKey -Snapshot $snap
    $State.InstalledApi = $snap
    $State.Installed = $actual
}

function Get-LegacyOpenClValues([string]$Root = (Join-Path $env:ProgramFiles 'DroidVM/OpenCL')) {
    if (!(Test-Path -LiteralPath $Root)) { return }
    Assert-GpuRegularPath $Root
    foreach ($package in Get-ChildItem -LiteralPath $Root -Directory) {
        if ($package.Name -notmatch '^[a-fA-F0-9]{64}$') { continue }
        $payload = Join-Path $package.FullName 'payload'
        $cat = Join-Path $package.FullName 'opencl.cat'
        $enabled = @()
        foreach ($arch in @('arm64','x64','x86')) {
            $entry = [pscustomobject]@{Hive='LocalMachine';View=$(if ($arch -eq 'x86') {'Registry32'} else {'Registry64'});
                Key='SOFTWARE\Khronos\OpenCL\Vendors';Name=(Join-Path $payload "$arch/viogpucl.dll")}
            $saved = @(Get-GpuRegistrySnapshot @($entry))[0]
            if (!$saved.Present) { continue }
            if ($saved.Kind -ne 'DWord') { throw 'Legacy OpenCL vendor value changed; preserve it' }
            # Nonzero DWORD entries are disabled by the Khronos loader. Preserve
            # their exact values and stale payloads without requiring validity.
            if ($saved.Value -eq 0) { $enabled += $saved }
        }
        if (!$enabled.Count) { continue }
        if (!(Test-Path -LiteralPath $cat)) { throw 'Enabled legacy OpenCL catalog missing; preserve registrations' }
        # An enabled architecture makes the entire signed package relevant.
        # Retain full catalog, path and all-architecture binding verification.
        Assert-GpuRegularPath $payload; Assert-GpuRegularPath $cat
        if ((Get-GpuHash $cat) -ine $package.Name) { throw 'Legacy OpenCL package identity changed' }
        $signature = Get-AuthenticodeSignature -LiteralPath $cat
        if ($signature.Status -ne 'Valid' -or (Test-FileCatalog -Path $payload -CatalogFilePath $cat) -ne 'Valid') {
            throw 'Legacy OpenCL catalog no longer verifies; preserve registrations'
        }
        $binding = Get-Content -LiteralPath (Join-Path $payload 'package-binding.json') -Raw | ConvertFrom-Json
        foreach ($arch in @('arm64','x64','x86')) {
            $relative = "$arch/viogpucl.dll"
            $file = Join-Path $payload $relative
            Assert-GpuRegularPath $file
            if ((Get-GpuHash $file) -ine $binding.files_after_signing.$relative) { throw 'Legacy OpenCL DLL ownership mismatch' }
        }
        $enabled
    }
}

function Get-ProtectedStateRoot {
    $root = Join-Path $env:ProgramData 'DroidVM/GpuInstaller'
    # Reject untrusted parent indirection before creating an elevated state tree.
    $parent = Split-Path -Parent $root
    if (!(Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory $parent | Out-Null }
    Assert-GpuRegularPath $parent
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in @('S-1-5-18','S-1-5-32-544')) {
        $id = New-Object Security.Principal.SecurityIdentifier($sid)
        $rule = New-Object Security.AccessControl.FileSystemAccessRule($id, 'FullControl',
            'ContainerInherit,ObjectInherit', 'None', 'Allow')
        $acl.AddAccessRule($rule)
    }
    $acl.SetOwner((New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')))
    if (!(Test-Path -LiteralPath $root)) {
        # Windows PowerShell/.NET Framework creates the directory with its ACL
        # atomically. Never trust a user-created state tree by merely relabeling it.
        [IO.Directory]::CreateDirectory($root, $acl) | Out-Null
    }
    Assert-GpuRegularPath $root
    $actualAcl = Get-Acl -LiteralPath $root
    if ($actualAcl.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin @('S-1-5-18','S-1-5-32-544')) {
        throw 'Installer state directory has an unexpected owner'
    }
    foreach ($access in $actualAcl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])) {
        $writes = [Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::Delete -bor
            [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership
        if ($access.AccessControlType -eq 'Allow' -and ($access.FileSystemRights -band $writes) -and
            $access.IdentityReference.Value -notin @('S-1-5-18','S-1-5-32-544')) { throw 'Installer state directory is writable by another identity' }
    }
    return $root
}

function New-ProductionBackend {
    @{
        Stage = { param($inf) [DroidVmGpuInstall.Native]::Stage($inf) }
        StoreInf = { param($inf) [DroidVmGpuInstall.Native]::StoreInf($inf) }
        Install = { param($inf,$rollback) [DroidVmGpuInstall.Native]::Install($inf,$rollback) }
        Remove = { param($inf) [DroidVmGpuInstall.Native]::Remove($inf) }
        CheckBefore = { param($state) Test-AdapterIdentity $state.Previous }
        BindingKind = { param($state)
            $current = Get-Adapter $state.InstanceId
            if ($current.InfHash -ceq $state.CandidateInfHash -and $current.Version -ceq $state.Version -and
                $current.Service -ieq 'VioGpuWddm') { 'candidate' }
            elseif (Test-AdapterIdentity $state.Previous) { 'previous' }
            else { 'unknown' }
        }
        VerifyCandidate = { param($state) Assert-CandidateBinding $state }
        VerifyPrevious = { param($state)
            if (!(Test-AdapterIdentity $state.Previous)) { throw 'Prior GPU package did not rebind' }
            $current = Get-Adapter $state.InstanceId
            # Permit repairing an unhealthy original adapter. If it was healthy
            # before this attempt, a same-INF error devnode is not a rollback.
            if ($state.Previous.Status -ceq 'OK' -and $state.Previous.ProblemCode -eq 0 -and
                ($current.Status -cne 'OK' -or $current.ProblemCode -ne 0)) {
                throw "Prior GPU package rebound but adapter health regressed: Status=$($current.Status) ProblemCode=$($current.ProblemCode)"
            }
            $values = @($state.PreviousApi | ForEach-Object {
                [pscustomobject]@{Hive=$_.Hive;View=$_.View;Key=$current.DriverKey;
                    Name=$_.Name;Present=$_.Present;Kind=$_.Kind;Value=$_.Value}
            })
            Set-GpuRegistrySnapshot $values
        }
        CheckLoaders = { param($loaders)
            foreach ($arch in @('arm64','x64','x86')) {
                $target = @($loaders | Where-Object Architecture -eq $(if ($arch -eq 'x86') {'x86'} else {'arm64x'}))[0].Path
                # Child diagnostics must stay visible without becoming part of
                # the installation transaction's single returned state object.
                & (Join-Path $script:Package.Root $script:Package.Manifest.loader_probes.$arch) $target |
                    ForEach-Object { Write-Host $_ }
                $probeExitCode = $LASTEXITCODE
                if ($probeExitCode) { throw "Public OpenCL loader cannot serve $arch; ExitCode=$probeExitCode; existing loader preserved" }
            }
        }
    }
}

Assert-NativeAdministrator
$stateRoot = Get-ProtectedStateRoot
$lock = [IO.File]::Open((Join-Path $stateRoot 'install.lock'), [IO.FileMode]::OpenOrCreate,
    [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$trustState = $null; $trustJournal = $null; $stageStarted = $false
try {
    if ($Action -in @('Verify','Install')) {
        if ($Action -eq 'Verify') {
            $script:Package = Read-FlatPackage $PackageRoot
            Write-Output 'PASS authenticated flat package; GPU execution not tested'; return
        }
        $previous = Get-Adapter $InstanceId
        $transaction = Join-Path $stateRoot ([Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory $transaction | Out-Null
        $JournalPath = Join-Path $transaction 'state.clixml'
        $trustJournal = Join-Path $transaction 'trust.clixml'
        $trustState = [pscustomobject]@{Schema=1;Phase='verifying';CertificateTrust=@();Error=$null}
        Write-GpuJournal $trustState $trustJournal
        $certificatePath = Join-Path $PSScriptRoot 'DroidVM_Test.cer'
        $catalogPath = Join-Path $PackageRoot 'viogpuwddm.cat'
        Assert-GpuRegularPath $certificatePath; Assert-GpuRegularPath $catalogPath
        $certificate = Assert-GpuBundledCertificate $certificatePath $catalogPath
        try { Add-GpuPackageTrust $certificate $trustState $trustJournal }
        finally { $certificate.Dispose() }
        $script:Package = Read-FlatPackage $PackageRoot
        if ($previous.InfHash -ceq $script:Package.InfHash) {
            $check = [pscustomobject]@{InstanceId=$previous.InstanceId;CandidateInfHash=$script:Package.InfHash;
                Version=$script:Package.Manifest.driver_version;Manifest=$script:Package.Manifest;InstalledApi=@();Installed=$null}
            Assert-CandidateBinding $check
            $targets = @($script:Package.Manifest.system_loaders | ForEach-Object {
                [pscustomobject]@{Architecture=$_.arch;Path=(Join-Path (Join-Path $env:SystemRoot $_.system_directory) $_.name)}
            })
            $backend = New-ProductionBackend
            & $backend.CheckLoaders $targets
            if (@(Get-LegacyOpenClValues).Count) { throw 'Exact INF is active but legacy CL values remain; recover the saved installation transaction' }
            $trustState.Phase='already-active'; Write-GpuJournal $trustState $trustJournal
            Write-Output 'PASS exact GPU package, adapter registration and public loader ABI already active'; return
        }
        $state = [pscustomobject]@{Schema=1;Phase='prepared';InstanceId=$previous.InstanceId;
            Previous=$previous;PreviousApi=@(Get-GpuRegistrySnapshot (Get-AdapterApiEntries $previous.DriverKey));
            CandidateInf=$script:Package.Inf;CandidateInfHash=$script:Package.InfHash;
            CandidateStoreInf=$null;PublishedInf=$null;Version=$script:Package.Manifest.driver_version;
            Manifest=$script:Package.Manifest;ManifestHash=$script:Package.ManifestHash;
            Installed=$null;InstalledApi=@();LegacyBefore=@(Get-LegacyOpenClValues);LegacyChanges=@();
            Loaders=@();InstallAttempted=$false;NeedReboot=$false;Error=$null;RecoveryError=$null;
            CertificateTrust=$trustState.CertificateTrust;TrustJournal=$trustJournal}
        foreach ($loader in $state.Manifest.system_loaders) {
            $state.Loaders += [pscustomobject]@{Source=(Join-Path $script:Package.Root $loader.source);
                Path=(Join-Path (Join-Path $env:SystemRoot $loader.system_directory) $loader.name);
                Hash=$loader.sha256;Architecture=$loader.arch;Preexisting=$false;BeforeHash=$null;Status='pending'}
        }
        $stageStarted = $true
        $result = Invoke-GpuInstallTransaction $state $JournalPath (New-ProductionBackend)
        $trustState.Phase='retained-for-staged-package'; Write-GpuJournal $trustState $trustJournal
        Write-Output "PASS exact unified GPU package installed; NeedReboot=$($result.NeedReboot); Journal=$JournalPath"
        Write-Output 'Existing Vulkan public loader is application/system supplied. Rendering validation remains required.'
        return
    }
    if (!$JournalPath) { throw 'Rollback/uninstall requires the exact saved JournalPath' }
    $JournalPath = (Resolve-Path -LiteralPath $JournalPath).Path
    if (!(Split-Path -Parent (Split-Path -Parent $JournalPath)).Equals($stateRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $JournalPath) -cne 'state.clixml') { throw 'Journal must belong to the protected installer state root' }
    Assert-GpuRegularPath $JournalPath
    $state = Import-Clixml -LiteralPath $JournalPath
    if ($state.Schema -ne 1 -or $state.Phase -ne 'installed') { throw 'Journal is not a completed active installation' }
    if ($InstanceId -and $InstanceId -ne $state.InstanceId) { throw 'Journal belongs to another adapter' }
    if (!(Test-AdapterIdentity $state.Installed) -or !(Test-GpuRegistrySnapshot $state.InstalledApi)) {
        throw 'GPU binding or adapter registration changed; preserve newer state'
    }
    foreach ($change in $state.LegacyChanges) {
        if (!(Test-GpuRegistrySnapshot @($change.After))) { throw 'Legacy vendor registration changed after installation' }
    }
    foreach ($loader in $state.Loaders) {
        if (!$loader.Preexisting -and $loader.Status -eq 'created' -and
            (!(Test-Path -LiteralPath $loader.Path) -or (Get-GpuHash $loader.Path) -cne $loader.Hash)) {
            throw 'Owned public loader changed after installation; preserve newer state'
        }
    }
    if ((Get-GpuHash $state.Previous.StoreInf) -cne $state.Previous.InfHash -or
        (Get-GpuHash $state.CandidateStoreInf) -cne $state.CandidateInfHash) { throw 'Retained driver identity changed' }
    $result = Invoke-GpuRemovalTransaction $state $JournalPath (New-ProductionBackend) $Action
    Write-Output "PASS $Action exact GPU package; NeedReboot=$($result.NeedReboot); retained journal=$JournalPath"
} catch {
    $failure = $_
    if ($Action -eq 'Install' -and $null -ne $trustState -and !$stageStarted) {
        try {
            Remove-GpuAttemptTrust $trustState.CertificateTrust
            $trustState.Phase='verification-failed';$trustState.Error=$failure.ToString()
            Write-GpuJournal $trustState $trustJournal
        } catch { throw "Pre-stage verification failed: $failure; certificate cleanup requires journal $trustJournal : $_" }
    }
    throw $failure
} finally { $lock.Dispose() }
