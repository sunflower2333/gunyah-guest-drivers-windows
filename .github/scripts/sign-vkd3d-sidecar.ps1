param(
    [Parameter(Mandatory)][string]$Payload,
    [Parameter(Mandatory)][string]$Output,
    [Parameter(Mandatory)][string]$Kmd,
    [Parameter(Mandatory)][string]$Umd,
    [Parameter(Mandatory)][string]$Pfx,
    [Parameter(Mandatory)][string]$PfxPassword,
    [Parameter(Mandatory)][string]$SignTool
)
$ErrorActionPreference = 'Stop'
$parentSource = (& git rev-parse HEAD).Trim()
if ($LASTEXITCODE) { throw 'Parent identity unavailable' }
$engineSource = ((& git ls-tree HEAD external/vkd3d-proton) -split '\s+')[2]
if ($LASTEXITCODE -or $engineSource -notmatch '^[0-9a-f]{40}$') { throw 'Engine gitlink unavailable' }
$mesaSource = ((& git ls-tree HEAD external/mesa) -split '\s+')[2]
if ($LASTEXITCODE -or $mesaSource -notmatch '^[0-9a-f]{40}$') { throw 'Mesa gitlink unavailable' }
New-Item -ItemType Directory $Output | Out-Null
$destination = Join-Path $Output 'payload'
New-Item -ItemType Directory $destination | Out-Null
foreach ($arch in @('arm64','x64','x86')) {
    $source = Join-Path $Payload "paired-vkd3d-$arch"
    $identity = Get-Content (Join-Path $source 'source.json') -Raw | ConvertFrom-Json
    if ($identity.Source -cne $engineSource -or $identity.Architecture -cne $arch -or $identity.NativeRuntimeValidated -ne $false) {
        throw "vkd3d $arch source/architecture/validation identity mismatch"
    }
    $candidate = Join-Path $destination $arch
    Copy-Item -LiteralPath $source -Destination $candidate -Recurse
    foreach ($pe in Get-ChildItem $candidate -File | Where-Object Extension -in @('.dll','.exe')) {
        & $SignTool sign /fd SHA256 /f $Pfx /p $PfxPassword $pe.FullName
        if ($LASTEXITCODE) { throw "vkd3d signing failed: $($pe.Name)" }
    }
    python .install_scripts/verify-pe-pdb.py "$candidate/viogpud3d12.dll" "$candidate/viogpud3d12.pdb"
    if ($LASTEXITCODE) { throw "vkd3d $arch PE/PDB mismatch" }
    # All three actual executables run on the ARM64 packaging runner.
    & "$candidate/vkd3d-umd-ddi-abi-test.exe" (Resolve-Path "$candidate/viogpud3d12.dll").Path
    if ($LASTEXITCODE) { throw "Signed vkd3d $arch DLL ABI check failed" }
    & "$candidate/vkd3d-umd-runtime-test.exe"
    if ($LASTEXITCODE) { throw "Signed vkd3d $arch native WDK lifecycle fixture failed" }
    Get-ChildItem $candidate -File | Where-Object Name -ne 'SHA256SUMS' | Get-FileHash -Algorithm SHA256 |
        ForEach-Object { $_.Hash + '  ' + [IO.Path]::GetFileName($_.Path) } | Set-Content "$candidate/SHA256SUMS"
}
$binding = [ordered]@{parent_commit=$parentSource; mesa_commit=$mesaSource; vkd3d_commit=$engineSource
    kmd_sha256=(Get-FileHash $Kmd).Hash; d3d_umd_sha256=(Get-FileHash $Umd).Hash
    native_runtime_validated=$false; registered=$false; advertised_ddi_versions=0; files_after_signing=@{}}
foreach ($file in Get-ChildItem $destination -Recurse -File) {
    $relative = $file.FullName.Substring([IO.Path]::GetFullPath($destination).Length + 1).Replace('\','/')
    $binding.files_after_signing[$relative] = (Get-FileHash $file.FullName).Hash
}
$binding | ConvertTo-Json -Depth 6 | Set-Content "$destination/package-binding.json" -Encoding utf8
$catalog = Join-Path $Output 'vkd3d.cat'
New-FileCatalog -Path $destination -CatalogFilePath $catalog -CatalogVersion 2.0 | Out-Null
& $SignTool sign /fd SHA256 /f $Pfx /p $PfxPassword $catalog
if ($LASTEXITCODE) { throw 'vkd3d catalog signing failed' }
if ((Test-FileCatalog -Path $destination -CatalogFilePath $catalog) -ne 'Valid') { throw 'vkd3d signed catalog mismatch' }
Write-Output "PASS vkd3d joint signature, exact KMD/Mesa binding and three-architecture WDK lifecycle; no native runtime acceptance"
