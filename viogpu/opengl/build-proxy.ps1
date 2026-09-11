param([string]$Output = 'opengl-system')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path '.').Path
$source = Join-Path $root 'viogpu/opengl'
$mesa = Join-Path $root 'external/mesa'
$out = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force $out | Out-Null
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC installation not found' }
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$exports = @(Get-Content "$source/proxy-exports.txt" | ForEach-Object {
    $parts = $_ -split ' '; @{ Name = $parts[0]; Bytes = $parts[1] }
})
function Invoke-Compiler([string]$arch, [string[]]$commands) {
    $script = Join-Path $out "build-$arch.cmd"
    @('@echo off', "call `"$dev`" -no_logo -host_arch=x64 -arch=$arch", 'if errorlevel 1 exit /b 1') +
        @($commands | ForEach-Object { $_; 'if errorlevel 1 exit /b 1' }) |
        Set-Content $script -Encoding ascii
    & cmd.exe /d /c $script
    if ($LASTEXITCODE) { throw "MSVC $arch build failed" }
}
foreach ($arch in @('arm64','x64','x86')) {
    $repro = if ($arch -eq 'arm64') { "/LINKREPROFULLPATHRSP:`"$out/arm64-inputs.rsp`"" } else { '' }
    $def = Join-Path $out "proxy-$arch.def"
    @('EXPORTS') + @($exports | ForEach-Object {
        if ($arch -eq 'x86') { "$($_.Name)=_$($_.Name)@$($_.Bytes)" } else { $_.Name }
    }) | Set-Content $def -Encoding ascii
    Invoke-Compiler $arch @(
        "cl /nologo /W4 /WX /EHsc /MT /LD /I`"$mesa/include`" /I`"$mesa/src/gallium/frontends/wgl`" `"$source/icd-proxy.cpp`" /Fo`"$out/proxy-$arch.obj`" /link /DEF:`"$def`" /OUT:`"$out/viogpuopengl_$arch.dll`" /PDB:`"$out/viogpuopengl_$arch.pdb`" /DEBUG $repro",
        "cl /nologo /W4 /WX /EHsc /MT /I`"$mesa/include`" `"$source/system-probe.cpp`" /Fo`"$out/probe-$arch.obj`" /Fe:`"$out/system-probe-$arch.exe`" user32.lib gdi32.lib opengl32.lib"
    )
}
# Merge native and EC adapter code using Microsoft's documented ARM64X recipe.
# A pure export forwarder needs its basename targets on the application's DLL
# search path. Real adapter code instead resolves every dependency absolutely.
$nativeInputs = @(Get-Content "$out/arm64-inputs.rsp" | Where-Object { $_ -match '(?i)\.(obj|lib)"$' })
if (!$nativeInputs.Count) { throw 'No native ARM64 link inputs captured' }
$nativeInputs | Set-Content "$out/arm64-merge.rsp" -Encoding ascii
Invoke-Compiler arm64 @(
    "cl /nologo /W4 /WX /EHsc /MT /LD /arm64EC /I`"$mesa/include`" /I`"$mesa/src/gallium/frontends/wgl`" `"$source/icd-proxy.cpp`" /Fo`"$out/proxy-arm64ec.obj`" /link /MACHINE:ARM64X @`"$out/arm64-merge.rsp`" /DEFARM64NATIVE:`"$out/proxy-arm64.def`" /DEF:`"$out/proxy-x64.def`" /OUT:`"$out/viogpuopengl.dll`" /PDB:`"$out/viogpuopengl.pdb`" /DEBUG"
)
