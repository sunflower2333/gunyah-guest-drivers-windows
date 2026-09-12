# SPDX-License-Identifier: MIT
# Actual Windows registry/files/catalog trust; device calls are controlled
# delegates. This does not exercise a VIOGPU devnode or claim hardware acceptance.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'viogpu-install-state.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'viogpu-install-certificates.psm1') -Force
Add-Type -Path (Join-Path $PSScriptRoot 'viogpu-install-native.cs')
$script:checks = 0
function Check($Condition,[string]$Message) {
    $script:checks++
    if (!$Condition) { throw "FAIL $Message" }
    Write-Output "PASS $script:checks $Message"
}
function Must-Fail([scriptblock]$Action,[string]$Text) {
    $errorText = $null
    try { & $Action | Out-Null } catch { $errorText = $_.ToString() }
    Check ($null -ne $errorText -and $errorText.Contains($Text)) "expected rejection: $Text; got $errorText"
}
$script:directory = Join-Path $env:RUNNER_TEMP ('gpu-install-fixture-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $script:directory | Out-Null
$script:keyName = 'Software\DroidVM\UnifiedInstallerFixture\' + [Guid]::NewGuid().ToString('N')
$script:entries = @('Registry64','Registry32') | ForEach-Object {
    [pscustomobject]@{Hive='CurrentUser';View=$_;Key=($script:keyName + '\' + $_);Name='C:\Owned\viogpucl.dll';
        Present=$true;Kind='DWord';Value=0}
}
$script:unrelated = [pscustomobject]@{Hive='CurrentUser';View='Registry64';Key=$script:keyName;
    Name='C:\OtherVendor\OpenCL.dll';Present=$true;Kind='DWord';Value=17}
$script:active = 'old'; $script:mode = 'success'; $script:installs = @()
$script:removed = @()
$script:legacyRegistryBefore = @()
$backend = @{
    Stage = { param($inf)
        if ($script:mode -eq 'stage-failure') { throw 'stage injected failure' }
        if ($script:mode -eq 'stage-external-switch') { $script:active='external' }
        $inf
    }
    StoreInf = { param($inf) $inf }
    Remove = { param($inf)
        if ($script:mode -eq 'remove-failure') { throw 'named removal injected failure' }
        $script:removed += $inf; return $true
    }
    Install = { param($inf,$rollback)
        $script:installs += [pscustomobject]@{Inf=$inf;Rollback=$rollback}
        if ($rollback) {
            if ($script:mode -eq 'rollback-failure') { throw 'rollback injected failure' }
            $script:active = 'old'; return $true
        }
        if ($script:mode -eq 'staged-only') { return $false }
        $script:active = 'new'
        if ($script:mode -eq 'install-external-switch') { $script:active='external'; throw 'external post-attempt binding' }
        if ($script:mode -eq 'partial-install') { throw 'partial install injected failure' }
        return $true
    }
    CheckBefore = { param($state) $script:active -eq 'old' }
    BindingKind = { param($state) if($script:active -eq 'new') {'candidate'} elseif($script:active -eq 'old') {'previous'} else {'unknown'} }
    VerifyCandidate = { param($state)
        if ($script:mode -in @('staged-only','verify-failure','rollback-failure')) { throw 'candidate binding verification failed' }
        if ($script:mode -eq 'concurrent-registry') {
            $changed = $script:entries[0].PSObject.Copy(); $changed.Value=99
            Set-GpuRegistrySnapshot @($changed)
        }
    }
    VerifyPrevious = { param($state) if ($script:active -ne 'old') { throw 'prior binding not restored' } }
    CheckLoaders = { param($loaders)
        if ($script:mode -eq 'loader-failure') { throw 'loader ABI injected failure' }
        if ($script:mode -eq 'loader-external-switch') { $script:active='external' }
    }
}
function New-State([string]$Name) {
    $folder = Join-Path $script:directory $Name
    New-Item -ItemType Directory $folder | Out-Null
    $old = Join-Path $folder 'old.inf'; $new = Join-Path $folder 'candidate.inf'
    [IO.File]::WriteAllText($old,'retained old package'); [IO.File]::WriteAllText($new,'candidate package')
    $source = Join-Path $folder 'source.dll'; [IO.File]::WriteAllText($source,'owned loader bytes')
    $target = Join-Path $folder 'OpenCL.dll'
    $script:active='old'; $script:installs=@(); $script:removed=@()
    Set-GpuRegistrySnapshot $script:entries
    Set-GpuRegistrySnapshot @($script:unrelated)
    $state = [pscustomobject]@{Phase='prepared';CandidateInf=$new;PublishedInf=$null;CandidateStoreInf=$null;
        Previous=[pscustomobject]@{StoreInf=$old};LegacyBefore=@(Get-GpuRegistrySnapshot $script:entries);
        LegacyChanges=@();InstallAttempted=$false;NeedReboot=$false;Error=$null;RecoveryError=$null;
        Loaders=@([pscustomobject]@{Source=$source;Path=$target;Hash=(Get-GpuHash $source);
            Architecture='arm64x';Preexisting=$false;BeforeHash=$null;Status='pending'})}
    return [pscustomobject]@{State=$state;Journal=(Join-Path $folder 'state.clixml')}
}
try {
    # Typed snapshot roundtrip includes empty MULTI_SZ/BINARY and unexpanded data.
    $typed = @(
        [pscustomobject]@{Hive='CurrentUser';View='Registry64';Key=$script:keyName;Name='multi';Present=$true;Kind='MultiString';Value=[string[]]@()},
        [pscustomobject]@{Hive='CurrentUser';View='Registry64';Key=$script:keyName;Name='binary';Present=$true;Kind='Binary';Value=[byte[]]@()},
        [pscustomobject]@{Hive='CurrentUser';View='Registry64';Key=$script:keyName;Name='expand';Present=$true;Kind='ExpandString';Value='%SystemRoot%\unchanged'},
        [pscustomobject]@{Hive='CurrentUser';View='Registry32';Key=$script:keyName;Name='qword';Present=$true;Kind='QWord';Value=[long]9223372036854775806}
    )
    Set-GpuRegistrySnapshot $typed
    $roundtrip = @(Get-GpuRegistrySnapshot $typed)
    $roundtrip | Export-Clixml (Join-Path $script:directory 'typed.clixml') -Depth 8
    Check (Test-GpuRegistrySnapshot @(Import-Clixml (Join-Path $script:directory 'typed.clixml'))) 'typed CLIXML registry roundtrip'

    $script:mode='success'; $case=New-State 'success'
    $result=Invoke-GpuInstallTransaction $case.State $case.Journal $backend
    Check ($result.Phase -eq 'installed' -and $result.NeedReboot) 'completed install records reboot requirement'
    Check (@(Get-GpuRegistrySnapshot $script:entries | Where-Object Present).Count -eq 0) 'only exact legacy vendors removed'
    Check (Test-GpuRegistrySnapshot @($script:unrelated)) 'unrelated vendor registration preserved'
    Check (Test-Path $result.Previous.StoreInf) 'prior package retained after success'
    Check ($script:installs.Count -eq 1 -and !$script:installs[0].Rollback) 'normal install never forces old/new driver'
    Check ((Import-Clixml $case.Journal).Phase -eq 'installed') 'durable completed journal'

    foreach ($action in @('Rollback','Uninstall')) {
        $script:mode='success'; $case=New-State ('remove-' + $action)
        $installed=Invoke-GpuInstallTransaction $case.State $case.Journal $backend
        $result=Invoke-GpuRemovalTransaction $installed $case.Journal $backend $action
        Check ($script:active -eq 'old') "$action restores prior exact package"
        Check (Test-GpuRegistrySnapshot $script:entries) "$action restores prior typed vendor values"
        Check (Test-GpuRegistrySnapshot @($script:unrelated)) "$action preserves unrelated vendors"
        Check (Test-Path $result.Loaders[0].Path) "$action preserves potentially shared public loader"
        Check (Test-Path $result.Previous.StoreInf) "$action never deletes rollback source"
        Check ($script:installs[-1].Rollback) "$action forces only the explicitly requested prior source"
        if ($action -eq 'Uninstall') {
            Check ($script:removed.Count -eq 1 -and $script:removed[0] -ceq $installed.CandidateStoreInf) 'uninstall removes only exact candidate INF'
            Check ($result.Phase -eq 'uninstalled') 'uninstall persisted'
        } else {
            Check ($script:removed.Count -eq 0 -and $result.Phase -eq 'rolled-back') 'rollback retains both packages'
        }
    }
    $script:mode='success'; $case=New-State 'remove-failure'
    $installed=Invoke-GpuInstallTransaction $case.State $case.Journal $backend
    $script:mode='remove-failure'
    Must-Fail { Invoke-GpuRemovalTransaction $installed $case.Journal $backend 'Uninstall' } 'named removal injected failure'
    Check ((Import-Clixml $case.Journal).Phase -eq 'recovery-required') 'failed removal cannot report complete'
    Check ($script:active -eq 'old') 'failed candidate deletion retains restored binding'

    foreach ($mode in @('stage-external-switch','loader-external-switch')) {
        $script:mode=$mode; $case=New-State $mode
        Must-Fail { Invoke-GpuInstallTransaction $case.State $case.Journal $backend } 'GPU binding changed before install'
        Check ($script:installs.Count -eq 0) "$mode never invokes native install or forced rollback"
        Check ($script:active -eq 'external') "$mode retains concurrent external binding"
        Check (Test-GpuRegistrySnapshot $script:entries) "$mode does not remove previous legacy values"
        Check ((Import-Clixml $case.Journal).Phase -eq 'cancelled-external-binding') "$mode reports cancellation without claiming prior binding restored"
    }
    $script:mode='install-external-switch'; $case=New-State 'install-external-switch'
    Must-Fail { Invoke-GpuInstallTransaction $case.State $case.Journal $backend } 'unrelated or uncertain'
    Check ($script:installs.Count -eq 1 -and !$script:installs[0].Rollback) 'unknown post-attempt binding is never force-rolled back'
    Check ($script:active -eq 'external') 'unknown post-attempt external binding retained'
    Check ((Import-Clixml $case.Journal).Phase -eq 'recovery-required') 'uncertain attribution journal requires recovery'

    foreach ($mode in @('stage-failure','loader-failure','staged-only','verify-failure','partial-install')) {
        $script:mode=$mode; $case=New-State $mode
        Must-Fail { Invoke-GpuInstallTransaction $case.State $case.Journal $backend } 'prior state restored'
        Check ($script:active -eq 'old') "$mode restores exact prior binding"
        Check (Test-GpuRegistrySnapshot $script:entries) "$mode preserves/restores legacy values"
        Check (Test-GpuRegistrySnapshot @($script:unrelated)) "$mode preserves unrelated values"
        Check (!(Test-Path $case.State.Loaders[0].Path)) "$mode removes only created loader"
        Check (Test-Path $case.State.Previous.StoreInf) "$mode retains prior package"
        Check ((Import-Clixml $case.Journal).Phase -eq 'rolled-back') "$mode durable rollback result"
    }
    $script:mode='loader-failure'; $case=New-State 'existing-loader'
    [IO.File]::WriteAllText($case.State.Loaders[0].Path,'unrelated existing loader')
    $before=Get-GpuHash $case.State.Loaders[0].Path
    Must-Fail { Invoke-GpuInstallTransaction $case.State $case.Journal $backend } 'prior state restored'
    Check ((Get-GpuHash $case.State.Loaders[0].Path) -ceq $before) 'existing unrelated loader never overwritten/deleted'
    Check ($case.State.Loaders[0].Preexisting) 'existing loader ownership not adopted'

    $script:mode='concurrent-registry'; $case=New-State 'concurrent-registry'
    Must-Fail { Invoke-GpuInstallTransaction $case.State $case.Journal $backend } 'prior state restored'
    Check (@(Get-GpuRegistrySnapshot @($script:entries[0]))[0].Value -eq 99) 'newer concurrent vendor value preserved'
    Check ($script:active -eq 'old') 'concurrent-registry candidate binding compensated'

    $script:mode='rollback-failure'; $case=New-State 'rollback-failure'
    Must-Fail { Invoke-GpuInstallTransaction $case.State $case.Journal $backend } 'recovery needs attention'
    Check ((Import-Clixml $case.Journal).Phase -eq 'recovery-required') 'rollback failure explicitly persisted'
    Check (Test-Path $case.State.Loaders[0].Path) 'rollback failure preserves loader needed by remaining candidate'
    Check (Test-GpuRegistrySnapshot $script:entries) 'rollback failure never removed old registration'

    # Interrupted exclusive creation is uncertain ownership, even identical bytes.
    $case=New-State 'uncertain-loader'
    [IO.File]::Copy($case.State.Loaders[0].Source,$case.State.Loaders[0].Path)
    $case.State.Loaders[0].Status='creating'
    Must-Fail { Remove-GpuOwnedLoaders $case.State.Loaders } 'ownership is uncertain'
    Check (Test-Path $case.State.Loaders[0].Path) 'uncertain ownership never deletes a concurrent file'

    # Execute production readback functions while controlling only the devnode.
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'viogpu-unified-install.ps1'),[ref]$tokens,[ref]$errors)
    foreach ($name in @('Assert-FlatName','Read-FlatPackage','Test-AdapterIdentity','New-ProductionBackend','Get-LegacyOpenClValues')) {
        $function=$ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$true) | Where-Object Name -eq $name
        Invoke-Expression $function.Extent.Text
    }
    function Get-Adapter([string]$Id) { $script:readback }
    $script:readback=[pscustomobject]@{InstanceId='fixture';InfHash='old';Version='1';Service='VioGpuWddm';
        Status='OK';ProblemCode=0;DriverKey=$script:keyName}
    $healthState=[pscustomobject]@{InstanceId='fixture';Previous=$script:readback.PSObject.Copy();PreviousApi=@()}
    $productionBackend=New-ProductionBackend
    & $productionBackend.VerifyPrevious $healthState
    Check $true 'production rollback accepts restored healthy prior adapter'
    $script:readback.Status='Error'
    Must-Fail { & $productionBackend.VerifyPrevious $healthState } 'adapter health regressed'
    $script:readback.Status='OK';$script:readback.ProblemCode=43
    Must-Fail { & $productionBackend.VerifyPrevious $healthState } 'adapter health regressed'
    $healthState.Previous.Status='Error';$healthState.Previous.ProblemCode=43
    Check (& $productionBackend.CheckBefore $healthState) 'original unhealthy adapter can enter repair installation'
    & $productionBackend.VerifyPrevious $healthState
    Check $true 'rollback permits restoration of originally unhealthy adapter without claiming health'

    # Exercise actual WinVerifyTrust catalog-member verification on Windows.
    Write-Output 'BEGIN signed catalog membership fixture'
    $principal=New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    Check ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) 'fixture runs elevated for production machine certificate stores'
    Write-Output "Fixture process architecture=$env:PROCESSOR_ARCHITECTURE PowerShell=$($PSVersionTable.PSVersion)"
    $catalogRoot=Join-Path $script:directory 'catalog'
    New-Item -ItemType Directory $catalogRoot | Out-Null
    $member=Join-Path $catalogRoot 'manifest.json'; [IO.File]::WriteAllText($member,'authenticated inventory')
    $catalog=Join-Path $script:directory 'fixture.cat'
    New-FileCatalog -Path $catalogRoot -CatalogFilePath $catalog -CatalogVersion 2.0 | Out-Null
    $certificate=New-SelfSignedCertificate -Subject 'CN=DroidVM Unified Installer Fixture' -Type CodeSigningCert -CertStoreLocation Cert:\CurrentUser\My
    $certificateFile=Join-Path $script:directory 'fixture.cer'
    Export-Certificate -Cert $certificate -FilePath $certificateFile | Out-Null
    Set-AuthenticodeSignature -LiteralPath $catalog -Certificate $certificate -HashAlgorithm SHA256 | Out-Null
    $public=Assert-GpuBundledCertificate $certificateFile $catalog
    Check ((Get-GpuCertificateHash $public) -ceq (Get-GpuCertificateHash $certificate)) 'catalog cryptographic signature matches exact bundled public certificate'
    $otherCertificate=New-SelfSignedCertificate -Subject 'CN=DroidVM Unified Installer Fixture' -Type CodeSigningCert -CertStoreLocation Cert:\CurrentUser\My
    $otherFile=Join-Path $script:directory 'other.cer'
    Export-Certificate -Cert $otherCertificate -FilePath $otherFile | Out-Null
    Must-Fail { Assert-GpuBundledCertificate $otherFile $catalog } 'does not exactly match'
    $tampered=Join-Path $script:directory 'tampered.cat'
    $bytes=[IO.File]::ReadAllBytes($catalog);$bytes[$bytes.Length-1]=$bytes[$bytes.Length-1] -bxor 1
    [IO.File]::WriteAllBytes($tampered,$bytes)
    Must-Fail { Assert-GpuBundledCertificate $certificateFile $tampered } ''
    Check $true 'tampered catalog rejected before any trust mutation'
    $trust=[pscustomobject]@{CertificateTrust=@()}
    $trustJournal=Join-Path $script:directory 'trust.clixml'
    foreach ($storeName in @('Root','TrustedPublisher')) {
        $physical="HKLM:\SOFTWARE\Microsoft\SystemCertificates\$storeName\Certificates"
        Write-Output "Before bootstrap $storeName physical certificate key exists=$(Test-Path $physical)"
    }
    # CurrentUser Root opens interactive trust confirmation. Disposable elevated
    # CI uses LocalMachine like production, with exact-thumbprint cleanup below.
    Add-GpuPackageTrust $public $trust $trustJournal 'LocalMachine'
    Check (@($trust.CertificateTrust | Where-Object Created).Count -eq 2) 'exact package trust records both created stores'
    $repeat=[pscustomobject]@{CertificateTrust=@()}
    Add-GpuPackageTrust $public $repeat (Join-Path $script:directory 'repeat-trust.clixml') 'LocalMachine'
    Check (@($repeat.CertificateTrust | Where-Object Created).Count -eq 0) 'native ADD_NEW preserves existing or concurrent certificates'
    Remove-GpuAttemptTrust $repeat.CertificateTrust
    Check ((Get-AuthenticodeSignature -LiteralPath $catalog).Status -eq 'Valid') 'cleanup does not remove preexisting trusted certificate'
    Import-Certificate -FilePath $otherFile -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Write-Output 'BEGIN WinVerifyTrust catalog member call'
    [DroidVmGpuInstall.Native]::VerifyCatalogMember($catalog,$member)
    Check $true 'actual trusted catalog membership'
    [IO.File]::WriteAllText($member,'tampered inventory')
    Must-Fail { [DroidVmGpuInstall.Native]::VerifyCatalogMember($catalog,$member) } 'Catalog membership failed'

    # Actual production discovery against signed legacy payloads and exact HKLM
    # Khronos values. All fixture names live under this disposable GUID tree.
    $legacyRoot=Join-Path $script:directory 'legacy-opencl'
    New-Item -ItemType Directory $legacyRoot | Out-Null
    function New-LegacyFixture([string]$Label) {
        $temporary=Join-Path $legacyRoot $Label
        $payload=Join-Path $temporary 'payload'
        New-Item -ItemType Directory $payload -Force | Out-Null
        $files=@{}
        foreach ($arch in @('arm64','x64','x86')) {
            New-Item -ItemType Directory (Join-Path $payload $arch) | Out-Null
            $relative="$arch/viogpucl.dll"
            $path=Join-Path $payload $relative
            [IO.File]::WriteAllText($path,"$Label $arch catalog fixture bytes")
            $files[$relative]=Get-GpuHash $path
        }
        @{files_after_signing=$files} | ConvertTo-Json -Depth 5 |
            Set-Content (Join-Path $payload 'package-binding.json')
        $legacyCatalog=Join-Path $temporary 'opencl.cat'
        New-FileCatalog -Path $payload -CatalogFilePath $legacyCatalog -CatalogVersion 2.0 | Out-Null
        Set-AuthenticodeSignature -LiteralPath $legacyCatalog -Certificate $certificate -HashAlgorithm SHA256 | Out-Null
        $destination=Join-Path $legacyRoot (Get-GpuHash $legacyCatalog)
        Move-Item -LiteralPath $temporary -Destination $destination
        $entries=@(@('arm64','x64','x86') | ForEach-Object {
            [pscustomobject]@{Hive='LocalMachine';View=$(if ($_ -eq 'x86') {'Registry32'} else {'Registry64'});
                Key='SOFTWARE\Khronos\OpenCL\Vendors';Name=(Join-Path $destination "payload/$_/viogpucl.dll");
                Present=$true;Kind='DWord';Value=1}
        })
        $script:legacyRegistryBefore += @(Get-GpuRegistrySnapshot $entries)
        [pscustomobject]@{Root=$destination;Payload=(Join-Path $destination 'payload');
            Catalog=(Join-Path $destination 'opencl.cat');Entries=$entries}
    }
    $stale=New-LegacyFixture 'disabled-stale'
    [IO.File]::WriteAllText($stale.Entries[1].Name,'old modified payload')
    Set-GpuRegistrySnapshot $stale.Entries
    Check ((Get-AuthenticodeSignature $stale.Catalog).Status -eq 'Valid' -and
        (Test-FileCatalog -Path $stale.Payload -CatalogFilePath $stale.Catalog) -ne 'Valid') 'stale fixture has valid signer but invalid contents'
    Check (@(Get-LegacyOpenClValues $legacyRoot).Count -eq 0) 'disabled invalid stale package does not block migration'
    Check (Test-GpuRegistrySnapshot $stale.Entries) 'all three disabled architecture entries retain exact DWORD values'
    $stale.Entries[0].Value=0;Set-GpuRegistrySnapshot $stale.Entries
    Must-Fail { Get-LegacyOpenClValues $legacyRoot } 'catalog no longer verifies'
    Check (Test-GpuRegistrySnapshot $stale.Entries) 'enabled invalid package rejects without registry mutation'
    $stale.Entries[0].Value=1;Set-GpuRegistrySnapshot $stale.Entries
    Move-Item -LiteralPath $stale.Catalog -Destination ($stale.Catalog + '.held')
    Check (@(Get-LegacyOpenClValues $legacyRoot).Count -eq 0) 'disabled package without catalog remains irrelevant'
    $stale.Entries[2].Value=0;Set-GpuRegistrySnapshot $stale.Entries
    Must-Fail { Get-LegacyOpenClValues $legacyRoot } 'Enabled legacy OpenCL catalog missing'
    $stale.Entries[2].Value=1;Set-GpuRegistrySnapshot $stale.Entries
    Move-Item -LiteralPath ($stale.Catalog + '.held') -Destination $stale.Catalog

    $current=New-LegacyFixture 'current-mixed'
    $current.Entries[0].Value=0;$current.Entries[2].Value=0
    Set-GpuRegistrySnapshot $current.Entries
    $otherVendor=[pscustomobject]@{Hive='LocalMachine';View='Registry64';Key='SOFTWARE\Khronos\OpenCL\Vendors';
        Name=(Join-Path $script:directory 'unrelated-vendor.dll');Present=$true;Kind='DWord';Value=0}
    $script:legacyRegistryBefore += @(Get-GpuRegistrySnapshot @($otherVendor))
    Set-GpuRegistrySnapshot @($otherVendor)
    $selected=@(Get-LegacyOpenClValues $legacyRoot)
    Check ($selected.Count -eq 2 -and $selected[0].Name -ceq $current.Entries[0].Name -and
        $selected[0].View -eq 'Registry64' -and $selected[1].Name -ceq $current.Entries[2].Name -and
        $selected[1].View -eq 'Registry32') 'only enabled native and Wow entries selected from valid mixed package'
    $malformed=$current.Entries[1].PSObject.Copy();$malformed.Kind='String';$malformed.Value='1'
    Set-GpuRegistrySnapshot @($malformed)
    Must-Fail { Get-LegacyOpenClValues $legacyRoot } 'vendor value changed'
    Check (Test-GpuRegistrySnapshot @($malformed)) 'malformed owned value is preserved on rejection'
    Set-GpuRegistrySnapshot $current.Entries
    $script:mode='success';$case=New-State 'actual-legacy-migration'
    $case.State.LegacyBefore=$selected
    $installed=Invoke-GpuInstallTransaction $case.State $case.Journal $backend
    Check (@(Get-GpuRegistrySnapshot $selected | Where-Object Present).Count -eq 0) 'actual transaction removes only selected enabled legacy values'
    Check (Test-GpuRegistrySnapshot $stale.Entries) 'migration preserves disabled invalid stale package values'
    Check (Test-GpuRegistrySnapshot @($current.Entries[1],$otherVendor)) 'migration preserves disabled architecture and unrelated active vendor'
    $restored=Invoke-GpuRemovalTransaction $installed $case.Journal $backend 'Rollback'
    Check (Test-GpuRegistrySnapshot $current.Entries) 'rollback restores exact mixed architecture legacy DWORD values'
    Check (Test-GpuRegistrySnapshot @($stale.Entries + @($otherVendor))) 'rollback preserves disabled stale and unrelated active vendor values'
    [IO.File]::WriteAllText($current.Entries[1].Name,'modified disabled member of active package')
    Must-Fail { Get-LegacyOpenClValues $legacyRoot } 'catalog no longer verifies'

    Remove-GpuAttemptTrust $trust.CertificateTrust
    Check (!(Test-Path "Cert:\LocalMachine\Root\$($public.Thumbprint)")) 'pre-stage verification cleanup removes exact newly created root entry'
    Check (!(Test-Path "Cert:\LocalMachine\TrustedPublisher\$($public.Thumbprint)")) 'pre-stage verification cleanup removes exact newly created publisher entry'
    Check (Test-Path "Cert:\LocalMachine\Root\$($otherCertificate.Thumbprint)") 'unrelated same-subject certificate remains trusted'
    $public.Dispose()

    # Execute the actual production manifest function without device entry code.
    $badRoot=Join-Path $script:directory 'unsigned-package'; New-Item -ItemType Directory $badRoot | Out-Null
    '{"schema":1,"phase":"before-signing","inf":"viogpuwddm.inf","cat":"viogpuwddm.cat","hardware_ids":["PCI\\VEN_1AF4&DEV_1050"]}' |
        Set-Content (Join-Path $badRoot 'viogpu-flat-package.json')
    Must-Fail { Read-FlatPackage $badRoot } 'Invalid or unsigned flat package manifest'
    Write-Output "PASS unified installer real registry/files/catalog and controlled lifecycle: $script:checks checks; no device installation or GPU acceptance"
} finally {
    if ($script:legacyRegistryBefore.Count) { Set-GpuRegistrySnapshot $script:legacyRegistryBefore }
    foreach ($view in @('Registry64','Registry32')) {
        $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]$view)
        try { $base.DeleteSubKeyTree($script:keyName,$false) } finally { $base.Dispose() }
    }
    if (Get-Variable certificate -ErrorAction SilentlyContinue) {
        Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -ErrorAction SilentlyContinue
        foreach ($store in @('Root','TrustedPublisher')) {
            Remove-Item -LiteralPath "Cert:\LocalMachine\$store\$($certificate.Thumbprint)" -ErrorAction SilentlyContinue
        }
    }
    if (Get-Variable otherCertificate -ErrorAction SilentlyContinue) {
        Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($otherCertificate.Thumbprint)" -ErrorAction SilentlyContinue
        foreach ($store in @('Root','TrustedPublisher')) {
            Remove-Item -LiteralPath "Cert:\LocalMachine\$store\$($otherCertificate.Thumbprint)" -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $script:directory -Recurse -Force
}
