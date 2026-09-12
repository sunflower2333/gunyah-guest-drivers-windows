param([string]$Clvk = 'clvk', [string]$Headers = 'opencl-headers',
      [string]$Loaders = 'opencl-loaders', [string]$Output = 'opencl-system-probes')
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path "$Clvk/tools/viogpu-opencl-check.cpp").Path
$headersRoot = (Resolve-Path $Headers).Path
$loadersRoot = (Resolve-Path $Loaders).Path
$out = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory $out | Out-Null
$build = New-Item -ItemType Directory "$loadersRoot/probe-build"
$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC unavailable' }
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
foreach ($arch in @('arm64','x64','x86')) {
    # Link the official public loader import library, never the CLVK backend.
    # Building that library with the actual loader preserves x86 stdcall ABI.
    $import = Join-Path $loadersRoot "$arch/OpenCL.lib"
    if (!(Test-Path -LiteralPath $import)) { throw "Missing public import library: $import" }
    $exe = Join-Path $out "viogpu-opencl-check-$arch.exe"
    $script = Join-Path $build.FullName "$arch.cmd"
    @('@echo off',
      "call `"$dev`" -no_logo -host_arch=x64 -arch=$arch",
      'if errorlevel 1 exit /b 1',
      "cl /nologo /O2 /EHsc /std:c++17 /MT /I`"$headersRoot`" `"$source`" /Fo`"$($build.FullName)/$arch.obj`" /Fe:`"$exe`" /link `"$import`" /MANIFEST:EMBED `"/MANIFESTUAC:level='asInvoker' uiAccess='false'`"",
      'if errorlevel 1 exit /b 1') | Set-Content $script -Encoding ascii
    & cmd.exe /d /c $script
    if ($LASTEXITCODE) { throw "Ordinary OpenCL $arch probe build failed" }
}
