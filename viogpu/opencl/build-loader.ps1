param([string]$Source = 'opencl-loader', [string]$Headers = 'opencl-headers', [string]$Output = 'opencl-loaders')
$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path $Source).Path
$headersRoot = (Resolve-Path $Headers).Path
$out = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory $out | Out-Null
$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC unavailable' }
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$sources = @('icd.c','icd_dispatch.c','icd_dispatch_generated.c','icd_trace.c') | ForEach-Object { Join-Path "$sourceRoot/loader" $_ }
$sources += @(Get-ChildItem "$sourceRoot/loader/windows/icd_windows*.c" | ForEach-Object FullName)
'/* Windows has neither secure_getenv variant. */' | Set-Content "$out/icd_cmake_config.h" -Encoding ascii
function Build([string]$Arch, [string[]]$Commands) {
    $script = "$out/build-$Arch.cmd"
    @('@echo off', "call `"$dev`" -no_logo -host_arch=x64 -arch=$(if ($Arch -eq 'arm64ec') {'arm64'} else {$Arch})", 'if errorlevel 1 exit /b 1') +
        @($Commands | ForEach-Object { $_; 'if errorlevel 1 exit /b 1' }) | Set-Content $script -Encoding ascii
    & cmd.exe /d /c $script
    if ($LASTEXITCODE) { throw "OpenCL loader $Arch build failed" }
}
foreach ($arch in @('arm64','x64','x86','arm64ec')) {
    New-Item -ItemType Directory "$out/$arch" | Out-Null
    $commands = @()
    foreach ($file in $sources) {
        $object = "$out/$arch/$([IO.Path]::GetFileNameWithoutExtension($file)).obj"
        $ec = if ($arch -eq 'arm64ec') {'/arm64EC'} else {''}
        $commands += "cl /nologo /c /O2 /MT $ec /DCL_SHARED_BUILD /DOPENCL_ICD_LOADER_VERSION_MAJOR=3 /DOPENCL_ICD_LOADER_VERSION_MINOR=1 /DOPENCL_ICD_LOADER_VERSION_REV=0 /I`"$headersRoot`" /I`"$sourceRoot/include`" /I`"$sourceRoot/loader`" /I`"$out`" `"$file`" /Fo`"$object`""
    }
    if ($arch -ne 'arm64ec') {
        $capture = if ($arch -eq 'arm64') {"/LINKREPROFULLPATHRSP:`"$out/native.rsp`""} else {''}
        $commands += "link /DLL /OUT:`"$out/$arch/OpenCL.dll`" /DEF:`"$sourceRoot/loader/windows/OpenCL.def`" `"$out/$arch/*.obj`" cfgmgr32.lib runtimeobject.lib advapi32.lib $capture"
        $probe = (Resolve-Path 'viogpu/opencl/loader-check.cpp').Path
        $commands += "cl /nologo /EHsc /MT `"$probe`" /Fo`"$out/$arch/check.obj`" /Fe:`"$out/$arch/loader-check.exe`""
    }
    Build $arch $commands
}
$native = @(Get-Content "$out/native.rsp" | Where-Object { $_ -match '(?i)\.(obj|lib)"$' })
if (!$native.Count) { throw 'Missing native linker inputs' }
$native | Set-Content "$out/native-merge.rsp" -Encoding ascii
New-Item -ItemType Directory "$out/arm64x" | Out-Null
Build arm64ec @("link /DLL /MACHINE:ARM64X `"$out/arm64ec/*.obj`" @`"$out/native-merge.rsp`" /DEFARM64NATIVE:`"$sourceRoot/loader/windows/OpenCL.def`" /DEF:`"$sourceRoot/loader/windows/OpenCL.def`" /OUT:`"$out/arm64x/OpenCL.dll`" cfgmgr32.lib runtimeobject.lib advapi32.lib")
