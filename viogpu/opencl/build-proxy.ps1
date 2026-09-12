param([string]$Headers = 'opencl-headers', [string]$Output = 'opencl-proxy')
$ErrorActionPreference = 'Stop'
$headersRoot = (Resolve-Path $Headers).Path
$source = $PSScriptRoot
$out = [IO.Path]::GetFullPath($Output)
New-Item -Type Directory $out | Out-Null
$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC unavailable' }
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
function Build([string]$Arch, [string[]]$Commands) {
    $script = "$out/build-$Arch.cmd"
    @('@echo off', "call `"$dev`" -no_logo -host_arch=x64 -arch=$Arch", 'if errorlevel 1 exit /b 1') +
        @($Commands | ForEach-Object { $_; 'if errorlevel 1 exit /b 1' }) | Set-Content $script -Encoding ascii
    & cmd.exe /d /c $script
    if ($LASTEXITCODE) { throw "OpenCL proxy $Arch build failed" }
}
$exports = @(@('clGetExtensionFunctionAddress',4), @('clGetExtensionFunctionAddressForPlatform',8), @('clIcdGetPlatformIDsKHR',12))
foreach ($arch in @('arm64','x64','x86')) {
    $def = "$out/proxy-$arch.def"
    @('EXPORTS') + @($exports | ForEach-Object { if ($arch -eq 'x86') {"$($_[0])=_$($_[0])@$($_[1])"} else {$_[0]} }) | Set-Content $def -Encoding ascii
    $capture = if ($arch -eq 'arm64') {"/LINKREPROFULLPATHRSP:`"$out/native.rsp`""} else {''}
    $flags = '/nologo /O2 /W4 /WX /EHsc /std:c++17 /MT /DCL_TARGET_OPENCL_VERSION=300 /DCL_USE_DEPRECATED_OPENCL_1_2_APIS'
    Build $arch @(
        "cl $flags /LD /I`"$headersRoot`" `"$source/icd-proxy.cpp`" /Fo`"$out/proxy-$arch.obj`" /link /DEF:`"$def`" /OUT:`"$out/viogpucl_proxy_$arch.dll`" $capture",
        "cl $flags /I`"$headersRoot`" `"$source/proxy-abi-check.cpp`" /Fo`"$out/check-$arch.obj`" /Fe:`"$out/opencl-proxy-check-$arch.exe`" /link advapi32.lib /MANIFEST:EMBED `"/MANIFESTUAC:level='asInvoker' uiAccess='false'`"",
        "cl $flags /LD /I`"$headersRoot`" `"$source/abi-fixture.cpp`" /Fo`"$out/fixture-$arch.obj`" /link /DEF:`"$def`" /OUT:`"$out/opencl-fixture-$arch.dll`""
    )
}
$native = @(Get-Content "$out/native.rsp" | Where-Object { $_ -match '(?i)\.(obj|lib)"$' })
if (!$native.Count) { throw 'Missing ARM64 link inputs' }
$native | Set-Content "$out/native-merge.rsp" -Encoding ascii
Build arm64 @(
    "cl /nologo /O2 /W4 /WX /EHsc /std:c++17 /MT /c /arm64EC /DCL_TARGET_OPENCL_VERSION=300 /DCL_USE_DEPRECATED_OPENCL_1_2_APIS /I`"$headersRoot`" `"$source/icd-proxy.cpp`" /Fo`"$out/proxy-arm64ec.obj`"",
    "link /DLL /MACHINE:ARM64X `"$out/proxy-arm64ec.obj`" @`"$out/native-merge.rsp`" /DEFARM64NATIVE:`"$out/proxy-arm64.def`" /DEF:`"$out/proxy-x64.def`" /OUT:`"$out/viogpucl.dll`""
)
