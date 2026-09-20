# SPDX-License-Identifier: BSD-3-Clause
# Executes the production signed verifier on a valid package and disposable
# damaged copies. Never installs a driver or modifies the baseline package.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$ToolsDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$package=(Resolve-Path $PackageDirectory).Path
$verify=Join-Path (Resolve-Path $ToolsDirectory).Path 'verify-video-package.ps1'
$baseline=& $verify -PackageDirectory $package
if (!$baseline.Verified -or $baseline.CatalogMembers -ne 4) { throw 'Signed baseline verification failed' }
$scratch=Join-Path $env:RUNNER_TEMP ('vpu-catalog-negatives-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    $cases=@(
        @{Name='binary-code';Failure='Catalog member failed';Mutate={param($dir)
            $file=Join-Path $dir 'viogpuvideo_mft.dll'
            $bytes=[IO.File]::ReadAllBytes($file)
            $pe=[BitConverter]::ToInt32($bytes,0x3c)
            $optional=[BitConverter]::ToUInt16($bytes,$pe+20)
            $section=$pe+24+$optional
            $raw=[BitConverter]::ToInt32($bytes,$section+20)
            if ($raw -le 0 -or $raw -ge $bytes.Length) { throw 'Invalid mutation code-section offset' }
            $bytes[$raw]=$bytes[$raw] -bxor 1
            [IO.File]::WriteAllBytes($file,$bytes)
        }},
        @{Name='signed-inventory';Failure='Catalog member failed';Mutate={param($dir)
            $file=Join-Path $dir 'viogpuvideo-package.json'
            $data=[IO.File]::ReadAllText($file)
            [IO.File]::WriteAllText($file,$data+' ')
        }},
        @{Name='missing-dll';Failure='exactly the five flat';Mutate={param($dir)
            Remove-Item -LiteralPath (Join-Path $dir 'viogpuvideo_mft.dll')
        }},
        @{Name='extra-file';Failure='exactly the five flat';Mutate={param($dir)
            [IO.File]::WriteAllText((Join-Path $dir 'unexpected.txt'),'extra')
        }},
        @{Name='subdirectory';Failure='exactly the five flat';Mutate={param($dir)
            New-Item -ItemType Directory -Path (Join-Path $dir 'nested') | Out-Null
        }}
    )
    foreach ($case in $cases) {
        $copy=Join-Path $scratch $case.Name
        New-Item -ItemType Directory -Path $copy | Out-Null
        Get-ChildItem -LiteralPath $package -File | Copy-Item -Destination $copy
        & $case.Mutate $copy
        $caught=$false
        try { & $verify -PackageDirectory $copy | Out-Null }
        catch {
            if ($_.Exception.Message -notlike ('*'+$case.Failure+'*')) { throw }
            $caught=$true
            Write-Output "PASS intended rejection: $($case.Name)"
        }
        if (!$caught) { throw "Verifier accepted mutation: $($case.Name)" }
    }
    & $verify -PackageDirectory $package | Out-Null
    Write-Output 'PASS signed baseline and five package corruption/layout negatives'
} finally {
    # Only this test-created GUID directory is removed.
    if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
}
