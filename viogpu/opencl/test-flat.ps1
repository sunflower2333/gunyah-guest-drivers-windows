param([string]$Payload = 'opencl-payload')
$ErrorActionPreference = 'Stop'
if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'Arm64') { throw 'Native ARM64 runner required' }
$root = (Resolve-Path $Payload).Path
$manifest = Get-Content "$root/flat-runtime.json" -Raw | ConvertFrom-Json
if ($manifest.schema -ne 1 -or $manifest.family -ne 'opencl') { throw 'Wrong manifest schema' }
if (@(Get-ChildItem $root -Directory).Count) { throw 'Subdirectory in flat runtime' }
foreach ($item in $manifest.files.PSObject.Properties) {
    if ((Get-FileHash (Join-Path $root $item.Name) -Algorithm SHA256).Hash -ne $item.Value.sha256) { throw "Hash mismatch $($item.Name)" }
}
foreach ($arch in @('arm64','x64','x86')) {
    & "$root/windows-flat-check-$arch.exe" "$root/viogpucl_$arch.dll" "viogpucl_vk_$arch.dll"
    if ($LASTEXITCODE) { throw "Real flat runtime/compiler $arch failed" }
    $icd = if ($arch -eq 'x86') {"$root/viogpucl_x86.dll"} else {"$root/viogpucl.dll"}
    & "$root/opencl-proxy-check-$arch.exe" $icd "$root/viogpucl_$arch.dll"
    if ($LASTEXITCODE) { throw "Real proxy extension ABI $arch failed" }
}
# Fixtures live only in disposable CI directories. Production package retains
# actual runtimes and the fixture files have role=probe, excluded from INF.
$temp = Join-Path $env:TEMP ('VIOGPU flat CL ABI ' + [Guid]::NewGuid().ToString('N'))
New-Item -Type Directory $temp | Out-Null
$registry = @()
try {
    Copy-Item "$root/viogpucl.dll" $temp
    Copy-Item "$root/OpenCL.dll" $temp
    Copy-Item "$root/OpenCL32.dll" $temp
    foreach ($arch in @('arm64','x64','x86')) {
        Copy-Item "$root/opencl-fixture-$arch.dll" "$temp/viogpucl_$arch.dll"
    }
    # The official loader intentionally ignores OCL_ICD_FILENAMES at elevated
    # integrity. Use normal registry discovery in this disposable CI machine,
    # adding only new per-run absolute names and removing exactly those names.
    foreach ($entry in @(@('Registry64','viogpucl.dll'), @('Registry32','viogpucl_x86.dll'))) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', $entry[0])
        try {
            $key = $base.CreateSubKey('SOFTWARE\Khronos\OpenCL\Vendors')
            try {
                $name = Join-Path $temp $entry[1]
                if ($key.GetValueNames() -contains $name) { throw 'Fixture registry collision' }
                $key.SetValue($name, 0, [Microsoft.Win32.RegistryValueKind]::DWord)
                $registry += @{View=$entry[0]; Name=$name}
            } finally { $key.Dispose() }
        } finally { $base.Dispose() }
    }
    foreach ($arch in @('arm64','x64','x86')) {
        $icd = if ($arch -eq 'x86') {"$temp/viogpucl_x86.dll"} else {"$temp/viogpucl.dll"}
        $loader = if ($arch -eq 'x86') {"$temp/OpenCL32.dll"} else {"$temp/OpenCL.dll"}
        & "$root/opencl-proxy-check-$arch.exe" $icd "$temp/viogpucl_$arch.dll" $loader
        if ($LASTEXITCODE) { throw "Khronos fixture dispatch ABI $arch failed" }
    }
    Remove-Item "$temp/viogpucl_arm64.dll", "$temp/viogpucl_x64.dll"
    foreach ($arch in @('arm64','x64')) {
        & "$root/opencl-proxy-check-$arch.exe" "$temp/viogpucl.dll" "$temp/viogpucl_$arch.dll" missing
        if ($LASTEXITCODE) { throw "Missing sibling $arch did not fail closed" }
    }
} finally {
    foreach ($entry in $registry) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', $entry.View)
        try {
            $key = $base.OpenSubKey('SOFTWARE\Khronos\OpenCL\Vendors', $true)
            try { if ($key) { $key.DeleteValue($entry.Name, $false) } }
            finally { if ($key) { $key.Dispose() } }
        } finally { $base.Dispose() }
    }
    Remove-Item -LiteralPath $temp -Recurse -Force
}
Write-Output 'PASS real flat modules/compiler, ARM64X native+EC extension calls, all3 standard-loader fixture dispatch and missing siblings; no GPU claim'
