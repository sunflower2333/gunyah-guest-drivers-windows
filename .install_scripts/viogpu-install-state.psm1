# SPDX-License-Identifier: MIT
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-GpuHash([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-GpuRegularPath([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    while ($null -ne $item) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Reparse point in installer path: $($item.FullName)"
        }
        $item = if ($item -is [IO.FileInfo]) { $item.Directory } else { $item.Parent }
    }
}

function Get-GpuRegistrySnapshot($Entries) {
    foreach ($entry in $Entries) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]$entry.Hive, [Microsoft.Win32.RegistryView]$entry.View)
        $key = $base.OpenSubKey($entry.Key)
        try {
            $exists = $null -ne $key -and @($key.GetValueNames()) -contains $entry.Name
            $kind = $null; $value = $null
            if ($exists) {
                $kind = $key.GetValueKind($entry.Name).ToString()
                $value = $key.GetValue($entry.Name, $null,
                    [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            }
            [pscustomobject]@{Hive=$entry.Hive; View=$entry.View; Key=$entry.Key;
                Name=$entry.Name; Present=$exists; Kind=$kind; Value=$value}
        } finally { if ($key) { $key.Dispose() }; $base.Dispose() }
    }
}

function Test-GpuRegistrySnapshot($Snapshot) {
    $actual = @(Get-GpuRegistrySnapshot $Snapshot)
    for ($i = 0; $i -lt @($Snapshot).Count; $i++) {
        $expected = @($Snapshot)[$i]
        if ($actual[$i].Present -ne $expected.Present -or $actual[$i].Kind -ne $expected.Kind -or
            (ConvertTo-Json -InputObject $actual[$i].Value -Compress) -cne
            (ConvertTo-Json -InputObject $expected.Value -Compress)) { return $false }
    }
    return $true
}

function Set-GpuRegistrySnapshot($Snapshot) {
    foreach ($entry in $Snapshot) {
        if ($entry.Hive -notin @('LocalMachine','CurrentUser') -or
            $entry.View -notin @('Registry64','Registry32')) { throw 'Invalid registry snapshot target' }
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]$entry.Hive, [Microsoft.Win32.RegistryView]$entry.View)
        # Absence restoration must not create unrelated empty keys.
        $key = if ($entry.Present) { $base.CreateSubKey($entry.Key) } else { $base.OpenSubKey($entry.Key, $true) }
        try {
            if ($entry.Present) {
                $kind = [Microsoft.Win32.RegistryValueKind]$entry.Kind
                $value = $entry.Value
                if ($entry.Kind -eq 'MultiString') { $value = [string[]]$value }
                if ($entry.Kind -eq 'Binary') { $value = [byte[]]$value }
                if ($entry.Kind -eq 'DWord') { $value = [int]$value }
                if ($entry.Kind -eq 'QWord') { $value = [long]$value }
                $key.SetValue($entry.Name, $value, $kind)
            } elseif ($key) { $key.DeleteValue($entry.Name, $false) }
            if ($key) { $key.Flush() }
        } finally { if ($key) { $key.Dispose() }; $base.Dispose() }
    }
    if (!(Test-GpuRegistrySnapshot $Snapshot)) { throw 'Registry mutation readback mismatch' }
}

function Write-GpuJournal($State, [string]$Path) {
    $temporary = $Path + '.new'
    $State | Export-Clixml -LiteralPath $temporary -Depth 14
    # File.Replace is atomic within the same directory; retain prior journal.
    if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($temporary, $Path, $Path + '.previous') }
    else { [IO.File]::Move($temporary, $Path) }
}

function Copy-GpuOwnedLoader($Record, $State, [string]$Journal) {
    if (Test-Path -LiteralPath $Record.Path) {
        Assert-GpuRegularPath $Record.Path
        $Record.Preexisting = $true
        $Record.BeforeHash = Get-GpuHash $Record.Path
        $Record.Status = 'preserved'
        Write-GpuJournal $State $Journal
        return
    }
    $Record.Status = 'creating'
    Write-GpuJournal $State $Journal
    # Exclusive creation makes a concurrent installer win without overwriting it.
    try { [IO.File]::Copy($Record.Source, $Record.Path, $false) }
    catch {
        $Record.Status = 'copy-failed'
        Write-GpuJournal $State $Journal
        throw
    }
    $Record.Status = 'created'
    Write-GpuJournal $State $Journal
    if ((Get-GpuHash $Record.Path) -ne $Record.Hash) { throw 'Published loader hash mismatch' }
}

function Remove-GpuOwnedLoaders($Records) {
    foreach ($record in $Records) {
        if ($record.Preexisting -or $record.Status -notin @('created','creating')) { continue }
        if (!(Test-Path -LiteralPath $record.Path)) { continue }
        if ($record.Status -eq 'creating') { throw "Loader creation ownership is uncertain; preserve $($record.Path)" }
        Assert-GpuRegularPath $record.Path
        if ((Get-GpuHash $record.Path) -ne $record.Hash) { throw "Owned loader changed: $($record.Path)" }
        Remove-Item -LiteralPath $record.Path
    }
}

function Invoke-GpuInstallTransaction($State, [string]$Journal, $Backend) {
    # Backend delegates isolate actual device calls from real-registry CI tests;
    # the public entry creates a fixed production backend and has no fixture flag.
    Write-GpuJournal $State $Journal
    try {
        $State.Phase = 'staging'; Write-GpuJournal $State $Journal
        $State.PublishedInf = & $Backend.Stage $State.CandidateInf
        $State.CandidateStoreInf = & $Backend.StoreInf $State.PublishedInf
        Write-GpuJournal $State $Journal
        foreach ($loader in $State.Loaders) { Copy-GpuOwnedLoader $loader $State $Journal }
        # These callbacks return no value. Route any diagnostics to the host;
        # the sole success-stream result of a transaction is its state object.
        & $Backend.CheckLoaders $State.Loaders | Out-Host
        if (!(Test-GpuRegistrySnapshot $State.LegacyBefore)) { throw 'Legacy registration changed before install' }
        if (!(& $Backend.CheckBefore $State)) { throw 'GPU binding changed before install' }
        $State.Phase = 'installing'; $State.InstallAttempted = $true
        Write-GpuJournal $State $Journal
        $State.NeedReboot = [bool](& $Backend.Install $State.CandidateInf $false)
        Write-GpuJournal $State $Journal
        & $Backend.VerifyCandidate $State | Out-Host
        # A failed/only-staged driver never loses its working legacy registration.
        foreach ($entry in $State.LegacyBefore) {
            if (!(Test-GpuRegistrySnapshot @($entry))) { throw 'Legacy registration changed during install' }
            $deleted = [pscustomobject]@{Hive=$entry.Hive; View=$entry.View; Key=$entry.Key;
                Name=$entry.Name; Present=$false; Kind=$null; Value=$null}
            $State.LegacyChanges += [pscustomobject]@{Before=$entry; After=$deleted; Status='changing'}
            Write-GpuJournal $State $Journal
            Set-GpuRegistrySnapshot @($deleted)
            $State.LegacyChanges[-1].Status = 'removed'
            Write-GpuJournal $State $Journal
        }
        $State.Phase = 'installed'; Write-GpuJournal $State $Journal
        return $State
    } catch {
        $failure = $_
        $State.Error = $failure.ToString(); $State.Phase = 'restoring'
        Write-GpuJournal $State $Journal
        try {
            # A binding change before our call belongs to another installer.
            # After our call, only an exact candidate binding is attributable;
            # an unknown newer package must never be forced back to Previous.
            if ($State.InstallAttempted -and !(& $Backend.CheckBefore $State)) {
                if ((& $Backend.BindingKind $State) -cne 'candidate') {
                    throw 'Current GPU binding is unrelated or uncertain; preserve it'
                }
                if (!$State.Previous.StoreInf) { throw 'No prior driver package available for automatic rollback' }
                $State.NeedReboot = [bool](& $Backend.Install $State.Previous.StoreInf $true) -or $State.NeedReboot
                & $Backend.VerifyPrevious $State | Out-Host
            }
            foreach ($change in $State.LegacyChanges) {
                if (Test-GpuRegistrySnapshot @($change.After)) { Set-GpuRegistrySnapshot @($change.Before) }
                elseif (!(Test-GpuRegistrySnapshot @($change.Before))) { throw 'Legacy registration changed; recovery snapshot retained' }
            }
            Remove-GpuOwnedLoaders $State.Loaders
            $State.Phase = if (!$State.InstallAttempted -and !(& $Backend.CheckBefore $State)) {
                'cancelled-external-binding'
            } else { 'rolled-back' }
            Write-GpuJournal $State $Journal
        } catch {
            $State.Phase = 'recovery-required'; $State.RecoveryError = $_.ToString()
            Write-GpuJournal $State $Journal
            throw "GPU install failed: $failure; recovery needs attention: $_; journal=$Journal"
        }
        if ($State.Phase -eq 'cancelled-external-binding') {
            throw "GPU install cancelled; external binding preserved: $failure; journal=$Journal"
        }
        throw "GPU install failed and prior state restored: $failure; journal=$Journal"
    }
}

function Invoke-GpuRemovalTransaction($State, [string]$Journal, $Backend,
    [ValidateSet('Rollback','Uninstall')][string]$Action) {
    $State.Phase = 'restoring'; Write-GpuJournal $State $Journal
    try {
        $State.NeedReboot = [bool](& $Backend.Install $State.Previous.StoreInf $true) -or $State.NeedReboot
        & $Backend.VerifyPrevious $State | Out-Host
        foreach ($change in $State.LegacyChanges) {
            if (!(Test-GpuRegistrySnapshot @($change.After))) { throw 'Legacy vendor changed during restore' }
            Set-GpuRegistrySnapshot @($change.Before)
        }
        # Public Khronos loaders may now serve other vendors. Successful removal
        # retains them and ownership evidence; failed installs remove only their
        # newly created loaders before they were advertised as shared runtime.
        if ($Action -eq 'Uninstall') {
            $State.NeedReboot = [bool](& $Backend.Remove $State.CandidateStoreInf) -or $State.NeedReboot
            if (!(& $Backend.CheckBefore $State)) { throw 'Removal changed restored GPU binding' }
            $State.Phase = 'uninstalled'
        } else { $State.Phase = 'rolled-back' }
        Write-GpuJournal $State $Journal
        return $State
    } catch {
        $State.Phase = 'recovery-required'; $State.RecoveryError = $_.ToString()
        Write-GpuJournal $State $Journal
        throw "GPU $Action failed; exact retained packages and journal remain: $_; journal=$Journal"
    }
}

Export-ModuleMember -Function Get-GpuHash,Assert-GpuRegularPath,Get-GpuRegistrySnapshot,
    Test-GpuRegistrySnapshot,Set-GpuRegistrySnapshot,Write-GpuJournal,
    Copy-GpuOwnedLoader,Remove-GpuOwnedLoaders,Invoke-GpuInstallTransaction,Invoke-GpuRemovalTransaction
