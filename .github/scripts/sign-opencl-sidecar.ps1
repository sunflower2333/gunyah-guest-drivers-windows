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
New-Item -ItemType Directory $Output | Out-Null
$destination = Join-Path $Output 'payload'
Copy-Item -LiteralPath $Payload -Destination $destination -Recurse
foreach ($file in Get-ChildItem $destination -Recurse -File | Where-Object Extension -in @('.dll','.exe')) {
    & $SignTool sign /fd SHA256 /f $Pfx /p $PfxPassword $file.FullName
    if ($LASTEXITCODE) { throw "OpenCL signing failed: $($file.Name)" }
}
$parent = (& git rev-parse HEAD).Trim()
if ($LASTEXITCODE) { throw 'Parent identity unavailable' }
$binding = [ordered]@{parent_commit=$parent; kmd_sha256=(Get-FileHash $Kmd).Hash
    d3d_umd_sha256=(Get-FileHash $Umd).Hash; files_after_signing=@{}}
foreach ($file in Get-ChildItem $destination -Recurse -File) {
    $relative = $file.FullName.Substring([IO.Path]::GetFullPath($destination).Length + 1).Replace('\','/')
    $binding.files_after_signing[$relative] = (Get-FileHash $file.FullName).Hash
}
$binding | ConvertTo-Json -Depth 6 | Set-Content "$destination/package-binding.json" -Encoding utf8
$catalog = Join-Path $Output 'opencl.cat'
New-FileCatalog -Path $destination -CatalogFilePath $catalog -CatalogVersion 2.0 | Out-Null
& $SignTool sign /fd SHA256 /f $Pfx /p $PfxPassword $catalog
if ($LASTEXITCODE) { throw 'OpenCL catalog signing failed' }
if ((Test-FileCatalog -Path $destination -CatalogFilePath $catalog) -ne 'Valid') { throw 'OpenCL signed catalog mismatch' }
Write-Output "PASS OpenCL joint signature and KMD/UMD binding parent=$parent"
