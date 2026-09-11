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
Set-StrictMode -Version Latest
New-Item -ItemType Directory -Path $Output | Out-Null
$destination = Join-Path $Output 'payload'
Copy-Item -LiteralPath $Payload -Destination $destination -Recurse
foreach ($file in Get-ChildItem -LiteralPath $destination -Recurse -File | Where-Object Extension -in @('.dll','.exe')) {
    & $SignTool sign /fd SHA256 /f $Pfx /p $PfxPassword $file.FullName
    if ($LASTEXITCODE) { throw "OpenGL PE signing failed: $($file.Name)" }
}
$parent = (& git rev-parse HEAD).Trim()
if ($LASTEXITCODE) { throw 'Parent source identity unavailable' }
$binding = [ordered]@{
    parent_commit = $parent
    kmd_sha256 = (Get-FileHash -LiteralPath $Kmd -Algorithm SHA256).Hash
    d3d_umd_sha256 = (Get-FileHash -LiteralPath $Umd -Algorithm SHA256).Hash
    files_after_signing = @{}
}
foreach ($file in Get-ChildItem -LiteralPath $destination -Recurse -File) {
    $relative = $file.FullName.Substring([IO.Path]::GetFullPath($destination).Length + 1).Replace('\','/')
    $binding.files_after_signing[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
}
$binding | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $destination 'package-binding.json') -Encoding utf8
$catalog = Join-Path $Output 'opengl.cat'
New-FileCatalog -Path $destination -CatalogFilePath $catalog -CatalogVersion 2.0 | Out-Null
if ((Test-FileCatalog -Path $destination -CatalogFilePath $catalog) -ne 'Valid') { throw 'OpenGL catalog did not cover its complete payload' }
& $SignTool sign /fd SHA256 /f $Pfx /p $PfxPassword $catalog
if ($LASTEXITCODE) { throw 'OpenGL catalog signing failed' }
if ((Test-FileCatalog -Path $destination -CatalogFilePath $catalog) -ne 'Valid') { throw 'Signed OpenGL catalog payload mismatch' }
Write-Output "PASS jointly signed OpenGL sidecar, parent=$parent KMD=$($binding.kmd_sha256)"
